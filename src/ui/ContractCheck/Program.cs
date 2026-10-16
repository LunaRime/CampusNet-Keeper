// 跨语言契约自检：C# 客户端 × 真实 C++ PipeServer。
//
// 为什么用手写断言而不是 xunit/nunit：它们都要从 NuGet 取包，而本机校园网 HTTPS 不通
//（Avalonia 也因此装不上）。零依赖的控制台程序同样能钉住契约，退出码 0 = 全过。
//
// 用法：
//   1) 起 C++ 侧探针服务端：build-m1\Release\cnk_ipc_probe.exe
//   2) dotnet run --project src/ui/ContractCheck -c Release
using System.Diagnostics;
using System.Text.Json.Nodes;
using CampusNet.UI.Services;

int passed = 0, failed = 0;
void Check(bool ok, string name)
{
    if (ok) { passed++; Console.WriteLine($"  [PASS] {name}"); }
    else { failed++; Console.WriteLine($"  [FAIL] {name}"); }
}

var pipeName = args.Length > 0 ? args[0] : PipeProtocol.PipeNameForCurrentUser();
Console.WriteLine($"契约自检：管道 = {pipeName}");

// 1) 可达性
await using var client = new IpcClient(pipeName);
Check(await client.IsDaemonRunningAsync(), "IsDaemonRunningAsync → true（服务在监听）");

// 2) ping 往返
var pong = await client.SendAsync("ping");
Check(pong?["ok"]?.GetValue<bool>() == true, "ping → ok=true");
Check(pong?["id"] is not null, "响应带回 id");

// 3) args 原样透传（含非 ASCII，验证 UTF-8 线协议）
var echo = await client.SendAsync("echo", new JsonObject { ["profile"] = "默认", ["n"] = 3 });
Check(echo?["data"]?["profile"]?.GetValue<string>() == "默认", "args 中文原样往返");
Check(echo?["data"]?["n"]?.GetValue<int>() == 3, "args 数字原样往返");

// 4) 未知命令 → ok=false + error 非空
var bad = await client.SendAsync("no_such_command_xyz");
Check(bad?["ok"]?.GetValue<bool>() == false, "未知命令 → ok=false");
Check(!string.IsNullOrEmpty(bad?["error"]?.GetValue<string>()), "未知命令 → error 非空");

// 5) 「一个连接一条请求」契约：连续 30 次都必须成功（客户端每次新建连接）
var okCount = 0;
for (var i = 0; i < 30; i++)
{
    var r = await client.SendAsync("ping");
    if (r?["ok"]?.GetValue<bool>() == true) okCount++;
}
Check(okCount == 30, $"连续 30 次请求全成功（实际 {okCount}）");

// 6) 服务不存在：快速失败，不得挂死
var sw = Stopwatch.StartNew();
await using var dead = new IpcClient("CampusNetKeeper.NoSuchPipe4Test");
var alive = await dead.IsDaemonRunningAsync();
sw.Stop();
Check(!alive, "不存在的管道 → false（而不是抛异常/挂死）");
Check(sw.ElapsedMilliseconds < 4000, $"不存在的管道快速失败（{sw.ElapsedMilliseconds}ms < 4000ms）");

// 7) 超大响应：服务端明确拒绝而不是挂死（C++ 侧上限 32KB）
var big = await client.SendAsync("blob");
Check(big?["ok"]?.GetValue<bool>() == false, "超大响应 → ok=false（服务端拒绝）");
Check((big?["error"]?.GetValue<string>() ?? "").Contains("过大"), "超大响应 → error 提示「过大」");

Console.WriteLine($"\n通过 {passed}，失败 {failed}");
return failed == 0 ? 0 : 1;
