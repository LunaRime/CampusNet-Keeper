# CampusNet.UI（任务 23）

守护进程的图形界面。**当前状态：核心已实现并跨语言验证；Avalonia 外壳尚未构建验证。**

## 工程结构

| 路径 | 说明 | 能否独立构建 |
|---|---|---|
| `CampusNet.UI.Core/` | IPC 客户端、管道名规则、协议常量。**只依赖 BCL**（含 `System.IO.Pipes`） | ✅ 能（无需 NuGet） |
| `ContractCheck/` | 跨语言契约自检（对真实 C++ `PipeServer` 跑） | ✅ 能（无需 NuGet） |
| `CampusNet.UI/` | Avalonia 外壳：App / 占位窗口 / 设计令牌字典 | ❌ **暂不能**（取不到 Avalonia 包） |

## 为什么外壳暂时构建不了

本机校园网 **HTTPS 不通**（已实测：`api.nuget.org`、`www.microsoft.com`、`github.com` 的 TLS
握手全部失败，而 HTTP 探测显示网络 Online），因此 **NuGet 取不到 Avalonia 包**
（`~/.nuget/packages` 也没有任何缓存）。等网络放行 HTTPS（或离线预置包）后，在 `src/ui/CampusNet.UI`
上跑一次 `dotnet build` 即可继续——首次构建预计要微调几处 Avalonia 的既定写法。

在此之前，能验证的部分已经固化成脚本，**不依赖编译器**：

```powershell
pwsh -File tools/check_ui_tokens.ps1
```

它检查：① 所有 XAML/csproj/manifest 是良构 XML；② 亮暗两份主题字典的键**一一对应**；
③ 除主题文件外**没有任何裸色值**（色值只能有一个来源）；④ 每个 `{DynamicResource Cnk*}`
引用都能解析。当前 4 项全过（键各 34 个 + 字体 2 个）。

## 跨语言契约（已验证）

```powershell
# 终端 1：起探针服务端（测试工具，45 秒后自动退出）
build-m1\Release\cnk_ipc_probe.exe --seconds=45
# 终端 2：
dotnet run --project src/ui/ContractCheck -c Release
```

2026-10-16 实测：**12/12 全过**（可达性、ping 往返、中文 args 的 UTF-8 往返、未知命令的
错误形态、**连续 30 次请求**、不存在的管道 1.5 秒内快速失败、超大响应被服务端明确拒绝）。

### 契约要点（两侧必须同时遵守）

- **一个连接只处理一条请求**：读到换行 → 应答一条 → 关闭。所以 `IpcClient` **每次请求新建连接**。
  ⚠ 计划原文的 `IpcClient` 保持长连接复用，与本项目 C++ 服务端的契约**不兼容**（第二次请求必失败），
  已按服务端契约实现。
- 客户端必须在收到响应后**及时读取并关闭**：服务端断开时会丢弃未被读取的数据。
- 连接要重试两类瞬时错误：`ERROR_PIPE_BUSY`（实例忙）与 `ERROR_FILE_NOT_FOUND`（实例尚未建出）。
- 响应上限 32KB（服务端保护），更大的数据要用 `args` **分页**请求。

## 目标框架

`net10.0-windows`。计划原文写的是 `net9.0-windows`，但本机**只有 .NET SDK 10.0.401**
（无 .NET 9 运行时），故改用 net10。`-windows` 后缀是计划本身的选择，也让
`WindowsIdentity` 可用（管道名需要当前用户 SID）。

## 构建产物位置

`obj/` 与 `bin/` 由仓库根目录的 `Directory.Build.props` 统一重定向到 `build-ui/`：
受限沙箱不允许在 `src/` 下新建文件（obj 会被拒），这样也顺带让源码树保持干净。
