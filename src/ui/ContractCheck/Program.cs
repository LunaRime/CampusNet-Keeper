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

// ⚠ 真入口断言：保存走的是 AppConfig.ToJson()（整包配置），不是 Profile.ToJson()。
// 只测 Profile.ToJson 会漏掉「明文密码根本没进报文」——那正是「界面显示已保存、
// 自连却报密码为空」的根因（PasswordPlain 带 [JsonIgnore]，STJ 不认 ShouldSerializeXxx）。
var sendCfg = new AppConfig { ActiveProfile = "默认" };
var sendProfile = Profile.Create("默认", "http://10.0.0.1/", "202400000001", "auto", "campus", "");
sendProfile.PasswordPlain = "界面输入的密码";
sendCfg.Profiles.Add(sendProfile);
var sendJson = sendCfg.ToJson();
var sendNode = JsonNode.Parse(sendJson)!;
Check(sendNode["profiles"]![0]!["password_plain"]?.GetValue<string>() == "界面输入的密码",
      "整包配置（set_config 真报文）带上 password_plain");
Check(!sendJson.Contains("password_enc"), "整包配置不含 password_enc");

var plainCfg = new AppConfig();
plainCfg.Profiles.Add(Profile.Create("默认", "http://10.0.0.1/", "202400000001", "auto", "campus", ""));
var plainJson = plainCfg.ToJson();
Check(!plainJson.Contains("password_plain"), "没输入新密码时整包配置不带 password_plain");
Check(AppConfig.FromJson(sendJson).Profiles[0].Username == "202400000001",
      "带 password_plain 的整包配置仍能往返（对端不认这个键也不该崩）");

// 密码三态：foreign 表示密文在、但本机解不开（换机器/换账号），界面必须据此提示重输
var foreign = AppConfig.FromJson(
    """{"profiles":[{"name":"x","has_password":false,"password_state":"foreign"}]}""");
Check(foreign.Profiles[0].PasswordState == "foreign", "读取 password_state=foreign");
Check(!foreign.Profiles[0].HasPassword, "foreign 不算「有可用密码」");
var noneState = AppConfig.FromJson("""{"profiles":[{"name":"y"}]}""");
Check(noneState.Profiles[0].PasswordState == "none", "缺省 password_state=none");

Console.WriteLine("== 运营商下拉合并规则（任务 24 的重点）==");
var fallback = CarrierOptions.Merge(null, "campus");
Check(fallback.Any(o => o.IsCustom), "始终保留「自定义…」项");
Check(fallback.Any(o => o.Value == "campus"), "取不到服务器列表时回退内置预设");
Check(fallback[^1].IsCustom, "「自定义…」在末位");

// 守护进程的实际形态（计划任务 25）：{"options":[{"id","name","suffix"}]}
var fromServer = JsonNode.Parse(
    """{"options":[{"id":"cmcc","name":"移动","suffix":"@cmcc"}],"title":"请选择运营商","default_id":"cmcc"}""")!;
var merged = CarrierOptions.Merge(fromServer, "cmcc");
Check(merged[0].Value == "cmcc" && merged[0].Label == "移动", "服务器项的 id/name 原样使用");
Check(merged[^1].IsCustom, "服务器列表下同样保留「自定义…」");

var stale = CarrierOptions.Merge(fromServer, "telecom");
Check(stale.Any(o => o.Value == "telecom"), "当前值不在服务器列表里也要能显示（避免下拉空白）");
Check(stale.First(o => o.Value == "telecom").Label == "中国电信",
      "补上的当前值用中文名，不是秃的 telecom");

// 真机症状：某些部署只下发 id、name 为空 → 下拉里出现过一条「cmcc」
var noName = JsonNode.Parse(
    """{"options":[{"id":"cmcc","name":""},{"id":"unicom","name":"unicom"}]}""")!;
var noNameList = CarrierOptions.Merge(noName, "cmcc");
Check(noNameList.First(o => o.Value == "cmcc").Label == "中国移动", "服务器 name 为空时用内置中文名");
Check(noNameList.First(o => o.Value == "unicom").Label == "中国联通", "服务器把 id 当 name 原样回显时也用中文名");

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

    // ===== 守护进程契约（对真实守护进程跑）=====
    Console.WriteLine("\n== 守护进程契约 ==");
    var st = await client.SendAsync("get_status");
    Check(st?["ok"]?.GetValue<bool>() == true, "get_status → ok");
    Check(st?["data"]?["state"] is not null, "get_status 含 state");

    var cfgResp = await client.SendAsync("get_config");
    Check(cfgResp?["ok"]?.GetValue<bool>() == true, "get_config → ok");
    var uiCfgJson = cfgResp?["data"]?.ToJsonString() ?? "";
    Check(uiCfgJson.Length > 0, "get_config 返回了配置");
    Check(!uiCfgJson.Contains("password_enc"), "get_config 不含 password_enc（安全红线）");
    Check(uiCfgJson.Contains("has_password"), "get_config 含 has_password");

    var logResp = await client.SendAsync("get_log_tail");
    Check((logResp?["data"]?["lines"] as JsonArray)?.Count > 0, "get_log_tail 有日志行");

    var carriers = await client.SendAsync("get_carrier_options");
    Check(carriers?["ok"]?.GetValue<bool>() == true, "get_carrier_options → ok");

    // 保存往返：把读到的配置原样写回（幂等）。不带 password_plain 时守护进程按档案名沿用旧密码，
    // 这正是"界面没改密码也不会把密码清掉"的那条保证。
    if (cfgResp?["data"] is JsonNode cfgNode)
    {
        var save = await client.SendAsync("set_config",
            new JsonObject { ["config"] = cfgNode.DeepClone() });
        var ok = save?["ok"]?.GetValue<bool>() == true;
        Check(ok, "set_config 往返 → ok（保存路径可用）");
        if (!ok) Console.WriteLine("    set_config 错误：" + save?["error"]?.GetValue<string>());

        var after = await client.SendAsync("get_config");
        Check(after?["ok"]?.GetValue<bool>() == true, "保存后仍能读回配置");
        Check(!(after?["data"]?.ToJsonString() ?? "").Contains("password_enc"),
              "保存后配置仍不含 password_enc");
    }
}

Console.WriteLine($"\n通过 {passed}，失败 {failed}");
return failed == 0 ? 0 : 1;
