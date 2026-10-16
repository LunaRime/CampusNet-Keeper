# CampusNet.UI（任务 23）

守护进程的图形界面。**当前状态：已构建、已运行、已可视化核验。**

![外壳截图](../docs/design/ui-shell-20261016.png)

## 工程结构

| 路径 | 说明 |
|---|---|
| `CampusNet.UI.Core/` | IPC 客户端、管道名规则、协议常量。**只依赖 BCL**（含 `System.IO.Pipes`） |
| `ContractCheck/` | 跨语言契约自检（对真实 C++ `PipeServer` 跑） |
| `CampusNet.UI/` | Avalonia 外壳：App / 占位窗口 / 设计令牌字典 / `--self-test` 自检 |

`CampusNet.UI.Core` 刻意不依赖 Avalonia：核心能在没有 UI 框架的情况下构建与验证。

## 构建与验证

```powershell
# 环境（沙箱内 dotnet 需要可写的 CLI_HOME；obj/bin 由根 Directory.Build.props 重定向到 build-ui/）
$env:DOTNET_CLI_HOME = "$PWD\build-ui\dotnet-home"
$env:NUGET_PACKAGES  = "$PWD\build-ui\nuget"

dotnet build src/ui/CampusNet.UI/CampusNet.UI.csproj -c Release     # 外壳
dotnet build src/ui/ContractCheck -c Release                        # 契约自检
pwsh -File tools/check_ui_tokens.ps1                                # 令牌层静态检查
```

### 三层验证，各管一段

| 手段 | 覆盖什么 | 结果（2026-10-16） |
|---|---|---|
| `tools/check_ui_tokens.ps1` | XML 良构、亮暗键一一对应、无裸色值、引用可解析（**不需要编译器**） | 4/4 通过 |
| `CampusNet.UI.exe --self-test` | 真跑 Avalonia，逐个解析令牌并与 DESIGN.md 的色值比对（不显示窗口） | **PASS**：23 个颜色键 × 明暗 + 圆角/字号/字体栈全对 |
| 真实窗口 + 截图 | 窗口渲染、IPC 连通 | 见上图：状态行显示「已连接守护进程」 |

`--self-test` 的结论会同时写入产物目录下的 `ui-selftest.txt`（WinExe 的退出码在 PowerShell 里
不可靠，脚本读文件更稳）。

### 跨语言契约（已跑通）

```powershell
build-m1\Release\cnk_ipc_probe.exe --seconds=45      # 终端 1：探针服务端（测试工具）
dotnet run --project src/ui/ContractCheck -c Release # 终端 2：契约自检
```

**12/12 全过**：可达性、ping 往返、中文 args 的 UTF-8 往返、未知命令的错误形态、
**连续 30 次请求**、不存在的管道 1.5 秒内快速失败、超大响应被服务端明确拒绝。

### 契约要点（两侧必须同时遵守）

- **一个连接只处理一条请求**：读到换行 → 应答一条 → 关闭。所以 `IpcClient` **每次请求新建连接**。
  ⚠ 计划原文的 `IpcClient` 保持长连接复用，与本项目 C++ 服务端的契约**不兼容**（第二次请求必失败）。
- 客户端必须在收到响应后**及时读取并关闭**：服务端断开时会丢弃未被读取的数据。
- 连接要重试两类瞬时错误：`ERROR_PIPE_BUSY`（实例忙）与 `ERROR_FILE_NOT_FOUND`（实例尚未建出）。
- 响应上限 32KB（服务端保护），更大的数据要用 `args` **分页**请求。

## 环境注意事项（都是踩过的坑）

- **HTTPS 与 GUI 都需要在非沙箱下跑**：受限沙箱既拦外部 TLS（NuGet 取不到包，**不是**校园网问题），
  也拦 Avalonia 的 Win32 平台初始化（`--self-test` 会卡住，连结论文件都写不出）。
  `dotnet restore` 需要在非沙箱下执行一次；包进缓存后 `dotnet build` 在沙箱内即可离线完成。
- **`dotnet` 需要 `DOTNET_CLI_HOME`** 指向工作区内可写目录，否则会因写不了
  `C:\Users\0\.dotnet` 的哨兵文件而报错。
- **WinExe 的坑**：PowerShell 用 `&` 启动**不会等它**，退出码无意义；`Start-Process` 若不重定向
  子进程 stdout，继承的句柄会让 shell 一直等。故自检用 `Environment.Exit` 主动结束，
  并可写结论文件供脚本读取。
- **截图**：截图进程要先 `SetProcessDPIAware()`，否则 DPI 虚拟化会让区域偏移（截出来会偏且缺边）。

## 目标框架

`net10.0-windows`。计划原文写的是 `net9.0-windows`，但本机**只有 .NET SDK 10.0.401**
（无 .NET 9 运行时），故改用 net10；`-windows` 后缀是计划本身的选择，也让 `WindowsIdentity`
可用（管道名需要当前用户 SID）。

## 构建产物位置

`obj/` 与 `bin/` 由仓库根目录的 `Directory.Build.props` 统一重定向到 `build-ui/`：
受限沙箱不允许在 `src/` 下新建文件（obj 会被拒），这样也顺带让源码树保持干净。

