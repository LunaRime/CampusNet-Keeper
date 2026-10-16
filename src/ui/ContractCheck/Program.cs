// 核心自检：① 跨语言契约（C# 客户端 × 真实 C++ PipeServer）② 配置镜像与运营商合并逻辑。
//
// 为什么用手写断言而不是 xunit/nunit：它们都要从 NuGet 取包。零依赖控制台程序同样能钉住契约，
// 退出码 0 = 全过。
//
// 用法：
//   1) 起 C++ 侧探针服务端：build-m1\Release\cnk_ipc_probe.exe --seconds=45
//   2) dotnet run --project src/ui/ContractCheck -c Release
//   守护进程不在时自动只跑「逻辑自检」部分（沙箱内也能跑，不需要命名管道）。
using System.Diagnostics;
using System.Text.Json.Nodes;
using CampusNet.UI.Models;
using CampusNet.UI.Services;

var passed = 0;
var failed = 0;
void Check(bool ok, string name)
{
    if (ok) { passed++; Console.WriteLine($"  [PASS] {name}"); }
    else { failed++; Console.WriteLine($"  [FAIL] {name}"); }
}

// ================= 逻辑自检（不依赖守护进程，沙箱内也能跑）=================

Console.WriteLine("== 配置镜像（与 C++ Config.h 逐字段对应）==");

var cfg = new AppConfig();
cfg.ActiveProfile = "默认";
cfg.Settings.ProbeIntervalSeconds = 7;
cfg.Profiles.Add(Profile.Create("默认", "http://10.0.0.1/", "202400000001@cmcc", "auto", "cmcc", ""));

var cfgJson = cfg.ToJson();
Check(cfgJson.Contains("\"probe_interval_seconds\":7"), "序列化用 C++ 的字段名（snake_case）");
Check(cfgJson.Contains("\"auth_url\""), "档案含 auth_url");
Check(!cfgJson.Contains("password_enc"), "UI 永不写出 password_enc（只由 C++ 侧用 DPAPI 生成）");

var back = AppConfig.FromJson(cfgJson);
Check(back.Settings.ProbeIntervalSeconds == 7, "往返：probe_interval_seconds 保持");
Check(back.ActiveProfile == "默认", "往返：active_profile 保持（含中文）");
Check(back.Profiles.Count == 1 && back.Profiles[0].Username == "202400000001@cmcc", "往返：账号保持");
Check(!back.Profiles[0].HasPassword, "未设密码时 has_password=false");

Console.WriteLine("== 密码规则（UI 不接触已存密码）==");
var p = Profile.Create("x", "http://a/", "u", "auto", "campus", "");
p.HasPassword = true;
p.PasswordPlain = "新密码";
var pj = p.ToJson();
Check(pj.Contains("\"password_plain\""), "新密码以 password_plain 传递");
Check(!pj.Contains("password_enc"), "password_plain 不伴随 password_enc");
Check(pj.Contains("\"has_password\":true"), "has_password 回传（只读语义）");
p.PasswordPlain = "";
Check(!p.ToJson().Contains("password_plain"), "未改密码时不发 password_plain");

Console.WriteLine("== 运营商下拉合并规则（任务 24 的重点）==");
var fallback = CarrierOptions.Merge(null, "campus");
Check(fallback.Any(o => o.IsCustom), "始终保留「自定义…」项");
Check(fallback.Any(o => o.Value == "campus"), "取不到服务器列表时回退内置预设");
Check(fallback[^1].IsCustom, "「自定义…」在末位");

var fromServer = JsonNode.Parse("""[{"key":"cmcc","label":"移动"}]""")!.AsArray();
var merged = CarrierOptions.Merge(fromServer, "cmcc");
Check(merged[0].Value == "cmcc" && merged[0].Label == "移动", "服务器项的 key/label 原样使用");
Check(merged[^1].IsCustom, "服务器列表下同样保留「自定义…」");

var stale = CarrierOptions.Merge(fromServer, "telecom");
Check(stale.Any(o => o.Value == "telecom"), "当前值不在服务器列表里也要能显示（避免下拉空白）");

// ================= 跨语言契约（需要探针服务端）=================

var pipeName = args.Length > 0 ? args[0] : PipeProtocol.PipeNameForCurrentUser();
await using var client = new IpcClient(pipeName);
if (!await client.IsDaemonRunningAsync())
{
    Console.WriteLine($"\n== 跨语言契约：跳过（{pipeName} 上没有守护进程；沙箱禁止命名管道，需非沙箱跑探针）==");
}
else
{
    Console.WriteLine($"\n== 跨语言契约（管道 {pipeName}）==");
    Check(true, "IsDaemonRunningAsync → true（服务在监听）");

    var pong = await client.SendAsync("ping");
    Check(pong?["ok"]?.GetValue<bool>() == true, "ping → ok=true");
    Check(pong?["id"] is not null, "响应带回 id");

    var echo = await client.SendAsync("echo", new JsonObject { ["profile"] = "默认", ["n"] = 3 });
    Check(echo?["data"]?["profile"]?.GetValue<string>() == "默认", "args 中文原样往返");
    Check(echo?["data"]?["n"]?.GetValue<int>() == 3, "args 数字原样往返");

    var bad = await client.SendAsync("no_such_command_xyz");
    Check(bad?["ok"]?.GetValue<bool>() == false, "未知命令 → ok=false");
    Check(!string.IsNullOrEmpty(bad?["error"]?.GetValue<string>()), "未知命令 → error 非空");

    var okCount = 0;
    for (var i = 0; i < 30; i++)
        if ((await client.SendAsync("ping"))?["ok"]?.GetValue<bool>() == true) okCount++;
    Check(okCount == 30, $"连续 30 次请求全成功（实际 {okCount}）");

    var sw = Stopwatch.StartNew();
    await using var dead = new IpcClient("CampusNetKeeper.NoSuchPipe4Test");
    var alive = await dead.IsDaemonRunningAsync();
    sw.Stop();
    Check(!alive, "不存在的管道 → false（而不是抛异常/挂死）");
    Check(sw.ElapsedMilliseconds < 4000, $"不存在的管道快速失败（{sw.ElapsedMilliseconds}ms < 4000ms）");

    var big = await client.SendAsync("blob");
    Check(big?["ok"]?.GetValue<bool>() == false, "超大响应 → ok=false（服务端拒绝）");
    Check((big?["error"]?.GetValue<string>() ?? "").Contains("过大"), "超大响应 → error 提示「过大」");
}

Console.WriteLine($"\n通过 {passed}，失败 {failed}");
return failed == 0 ? 0 : 1;
