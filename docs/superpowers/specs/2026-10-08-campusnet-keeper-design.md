# CampusNet Keeper — 设计文档

日期：2026-10-08
状态：待用户审阅

## 1. 背景与目标

### 1.1 痛点

校园网 Web Portal 认证会话有效期约 2-3 天。会话失效后网络静默中断，用户必须手动打开浏览器重新登录。若中断发生在游戏过程中，体验损失显著。

### 1.2 目标

一个常驻 Windows 托盘的守护程序，持续探测网络可达性，检测到认证失效时自动重新登录，使用户在绝大多数情况下感知不到中断。

### 1.3 成功标准

| 编号 | 标准 | 验证方式 |
|---|---|---|
| S1 | 断线后 ≤ 15 秒内发起重登 | 日志时间戳 |
| S2 | 后台常驻内存 < 10 MB | 任务管理器实测 |
| S3 | 空闲 CPU 占用 ≈ 0% | 任务管理器实测 |
| S4 | 设置界面可填写：认证网址、账号、密码、运营商 | UI 走查 |
| S5 | 关闭设置窗口后守护进程继续运行 | 实测 |
| S6 | 开机静默自启至托盘，不弹窗 | 重启实测 |
| S7 | 交付单个 exe | 产物检查 |
| S8 | 公开 GitHub 仓库含 CI 自动构建 Release | 仓库检查 |

### 1.4 非目标

- 不支持 802.1X / EAPOL 客户端认证（如锐捷 Supplicant、华为客户端）。本设计假定使用 HTTP Web Portal 认证。
- 不支持 macOS / Linux。
- 不实现多用户共享、远程管理、Web 控制台。
- 不做自动破解或绕过认证，仅自动化用户已拥有凭据的合法登录流程。

### 1.5 用户与运行环境

- 单一用户：学生本人，在个人 Windows 10/11 x64 机器上运行。
- 使用者可能将 exe 分发给同学，因此需要「零依赖」发布选项。

## 2. 术语

本项目有两个容易混淆的概念，全文严格区分：

- **运营商（ISP）**：网络服务提供方，取值如 中国移动 / 中国电信 / 中国联通 / 校园网。**由用户在 UI 中选择**，映射为认证请求中的 `domain` 或 `service` 参数。
- **认证厂商（Vendor）**：Portal 认证系统的软件供应商，取值如 深澜 Srun / 锐捷 eportal / Dr.COM / 华为 iPortal / 通用表单 / 自定义模板。**由程序自动识别**，用户通常无需关心。

## 3. 架构总览

### 3.1 单 exe 双形态

交付物是一个原生 C++17 可执行文件 `CampusNet-Keeper.exe`，静态链接 CRT，零动态库依赖。它以两种形态运行：

**守护形态（无参数启动，常驻）**

原生进程，**不加载 .NET 运行时**。职责：托盘图标与菜单、定时探测、认证请求发送、配置与日志读写、命名管道服务端、开机自启管理。

常驻内存目标 3-5 MB，空闲 CPU 0%。

**UI 形态（`--ui` 启动，按需）**

同一个 exe 再次 `CreateProcess` 自身并传入 `--ui`。该形态的 native 宿主通过 `hostfxr` API 按需加载 .NET 运行时，将内嵌在 exe 资源中的 Avalonia UI 程序集解压到 `%LOCALAPPDATA%\CampusNet-Keeper\ui\`，再调用托管入口点显示窗口。用户关闭窗口后子进程退出，内存完全释放。

### 3.2 为什么不同进程内加载 CLR

.NET Core 的 CLR 一旦加载进进程便无法卸载。若在守护进程内直接加载 CLR，即使 UI 窗口关闭，内存也不会回落——直接违背 S2。子进程方案让守护进程的常驻内存与 UI 完全解耦。

### 3.3 组件图

```
┌─────────────────── CampusNet-Keeper.exe (native C++) ────────────────────┐
│                                                                          │
│  ┌──────── 守护形态（常驻，无 CLR） ────────┐   ┌──── UI 形态（子进程）──┐ │
│  │                                          │   │   native host          │ │
│  │  Tray        托盘图标 + 原生右键菜单      │   │     ↓ hostfxr          │ │
│  │  Scheduler   状态机 + 退避重试            │   │   Avalonia 11 (C#)     │ │
│  │  Probe       连通性探测 + 地址变更通知    │   │     ↓ IpcClient        │ │
│  │  Auth/*      5 种认证策略                 │   │                        │ │
│  │  Crypto      BCrypt + srun xEncode        │   │                        │ │
│  │  Store       JSON 配置 + DPAPI + 日志     │   │                        │ │
│  │  PipeServer  命名管道服务端               │   │                        │ │
│  │  Sys         自启注册 + 单实例互斥体      │   │                        │ │
│  └──────────────────┬───────────────────────┘   └───────────┬────────────┘ │
│                     └──────── 命名管道 IPC ──────────────────┘             │
└──────────────────────────────────────────────────────────────────────────┘
```

### 3.4 技术栈

| 层 | 选型 | 理由 |
|---|---|---|
| 守护核心 | C++17 / Win32 API | 无运行时，内存与启动开销最低 |
| HTTP | WinHTTP | 系统自带，无需第三方库 |
| 加密 | BCrypt (CNG) | 系统自带，提供 MD5/SHA1/HMAC |
| 密码存储 | DPAPI (`CryptProtectData`) | 绑定当前用户，配置被拷走也无法解密 |
| JSON | nlohmann/json（header-only） | 单头文件，不引入运行时依赖 |
| UI | C# / Avalonia 11 / .NET 9 | 现代 Fluent 视觉，无 WinRT 依赖，易嵌入原生宿主 |
| CLR 加载 | `nethost` + `hostfxr` | .NET 官方 native hosting API |
| 构建 | CMake 3.31 + MSVC v143 + Ninja | VS2022 自带，CI 一致 |
| C++ 单测 | doctest（header-only） | 轻量，零构建复杂度 |

不引入任何 UI 相关的 C++ 库（无 Qt、无 MFC、无 Win32 自绘控件库），不使用 WinForms / WPF。

## 4. 核心组件接口

### 4.1 认证策略接口

所有认证方式实现同一接口，使 Scheduler 与具体协议解耦：

```cpp
// src/native/auth/IAuth.h
struct NetContext {
    std::string local_ip;      // 访问 Portal 的本机源 IP
    std::string portal_url;    // 探测阶段发现的重定向目标
    std::string portal_body;   // Portal 页面 HTML
    std::string gateway_mac;   // 可选，部分协议需要
};

struct LoginResult {
    bool success;
    std::string message;       // 失败原因或成功提示
    int vendor_code;           // 厂商返回的原始码
};

class IAuth {
public:
    virtual ~IAuth() = default;
    virtual LoginResult Login(const Profile& profile,
                              const NetContext& ctx) = 0;
    virtual const char* Name() const = 0;
};
```

新增一种认证方式 = 新增一个 `IAuth` 实现 + 在 `AuthFactory` 注册，不改动其他组件。

### 4.2 认证策略清单

| 实现 | 触发条件 | 复杂度 |
|---|---|---|
| `SrunAuth` | 识别为深澜 | 高（HMAC-MD5 + xEncode + 自定义码表） |
| `EportalAuth` | 识别为锐捷 | 中（GET 带参 + JSONP 解析） |
| `DrcomAuth` | 识别为 Dr.COM | 低（GET 带参） |
| `GenericFormAuth` | 未识别 / 用户指定 | 中（HTML 表单解析 + 字段启发式匹配） |
| `CustomTemplateAuth` | 用户选「自定义」 | 低（模板占位符替换） |

`GenericFormAuth` 的字段匹配规则（大小写不敏感，按序取首个命中）：

- 用户名字段：`username` | `user` | `account` | `name` | `DDDDD` | `user_account` | `user_name` | `login_name`
- 密码字段：`password` | `pwd` | `pass` | `upass` | `user_password` | `passwd`
- 运营商字段：`domain` | `service` | `isp` | `operator` | `type`
- 其余 `<input>`（含 `type="hidden"`）原样保留并提交

提交方式取 `<form>` 的 `method`；`action` 为相对路径时相对 Portal 页面 URL 解析。若页面无 `<form>` 标签，从页面内的 `location.href=` / `window.location=` 赋值语句中提取跳转目标作为候选。

### 4.3 UI 与守护的 IPC 契约

- 管道名：`\\.\pipe\CampusNet-Keeper`
- 传输：换行分隔的 JSON 对象（JSON Lines）
- 访问控制：管道安全描述符仅授予当前登录用户 SID，阻止本机其他账户读写配置

请求 `{"id":<int>,"cmd":"<命令>","args":{...}}`
响应 `{"id":<int>,"ok":<bool>,"data":{...},"error":"<字符串>"}`

| 命令 | 方向 | 说明 |
|---|---|---|
| `get_config` | UI → 守护 | 返回完整配置，**密码字段以 `has_password: true` 代替，不回传明文** |
| `get_password` | UI → 守护 | 用户点击「显示密码」时单独取一次明文 |
| `set_config` | UI → 守护 | 写入配置并热重载，不重启进程 |
| `get_status` | UI → 守护 | 当前状态、上次登录时间、上次错误、IP |
| `reconnect_now` | UI → 守护 | 立即触发一次探测 + 登录 |
| `test_profile` | UI → 守护 | 用指定配置试登录一次，返回详细过程供排查 |
| `get_log_tail` | UI → 守护 | 返回最近 N 行日志 |
| `events` | 守护 → UI | 状态变化时主动推送 |

密码不随 `get_config` 回传，是为了避免明文密码经 IPC 面在进程间反复流动。

### 4.4 Avalonia UI 结构

```
src/ui/
  CampusNet.UI.csproj       net9.0-windows, Avalonia 11
  Program.cs                入口，被 native host 调用
  App.axaml(.cs)            应用级主题与依赖注入
  Views/
    MainWindow.axaml        主设置窗口
    LogPanel.axaml          日志查看
  ViewModels/
    MainViewModel.cs
    ProfileViewModel.cs
  Services/
    IpcClient.cs            命名管道客户端
  Models/
    AppConfig.cs / Profile.cs / Status.cs
  Themes/
    Light.axaml / Dark.axaml
```

主窗口布局（单页，不做多步向导）：

- 顶部：连接状态指示灯 + 当前 IP + 上次登录时间 + 「立即重连」按钮
- 中部：认证网址、账号、密码（掩码 + 显示按钮）、**运营商下拉**、认证方式（自动识别 / 手动指定）
- 底部：「高级」可折叠区（探测间隔、重试上限、开机自启、重连通知）+ 保存
- 侧栏：配置档案列表，支持新增 / 切换 / 删除

**主题**：Avalonia Fluent 主题，亮/暗跟随系统；窗口使用 `TransparencyLevelHint` 取得亚克力背景。

**运营商下拉的行为**（这是本设计里唯一有厂商差异的 UI 细节）：

1. 预设四项：中国移动、中国电信、中国联通、校园网
2. 第五项「自定义…」展开文本框，供少数学校使用非标服务名（例如数字 `1`/`2`/`3` 或中文全称）
3. 程序识别到 Portal 页面中内嵌的可用运营商列表时，用识别结果**覆盖**预设项

映射规则：

| 运营商 | 深澜 `domain` | 锐捷 `service` |
|---|---|---|
| 中国移动 | `@cmcc` | 依学校而定 |
| 中国电信 | `@telecom` | 依学校而定 |
| 中国联通 | `@unicom` | 依学校而定 |
| 校园网 | `@campus` | 空字符串 |

锐捷的 `service` 取值各校差异大（有英文、有中文、有数字），因此**必须保留自定义输入**，不能只靠预设。

## 5. 数据结构

### 5.1 配置文件

位置：`%APPDATA%\CampusNet-Keeper\config.json`（UTF-8，缩进 2 空格）

```json
{
  "version": 1,
  "active_profile": "宿舍",
  "settings": {
    "probe_interval_seconds": 5,
    "probe_timeout_seconds": 3,
    "probe_url": "http://connect.rom.miui.com/generate_204",
    "retry_backoff_seconds": [5, 10, 20, 30, 60],
    "autostart": true,
    "notify_on_reconnect": true,
    "log_level": "info",
    "log_max_kb": 512,
    "log_keep_files": 3
  },
  "profiles": [
    {
      "name": "宿舍",
      "auth_url": "http://10.10.10.10",
      "username": "20230000000",
      "password_enc": "<base64 encoded DPAPI blob>",
      "vendor": "auto",
      "isp": { "preset": "telecom", "custom": "" },
      "custom_template": {
        "method": "POST",
        "url": "http://10.10.10.10/login",
        "body": "user={username}&pwd={password}&isp={isp}&t={timestamp}",
        "headers": { "Content-Type": "application/x-www-form-urlencoded" },
        "success_regex": "(成功|success|\"result\":1)"
      }
    }
  ]
}
```

`vendor` 取值：`auto` | `srun` | `eportal` | `drcom` | `generic` | `custom`。
`password_enc` 为 `CryptProtectData` 输出（含 entropy）的 base64，仅当前 Windows 用户在当台机器可解。

### 5.2 状态模型

```
Disconnected ──探测──► Probing ──在线──► Online
                         │
                      离线/劫持
                         ▼
                    PortalDetected
                         │
                         ▼
                    Authenticating ──成功──► Online
                         │
                       失败
                         ▼
                    AuthFailed(reason) ──退避──► Probing
```

托盘图标颜色映射：Online 绿 / Probing 灰 / Authenticating 蓝 / AuthFailed 红。

### 5.3 日志

位置：`%APPDATA%\CampusNet-Keeper\logs\keeper.log`。滚动策略：超过 `log_max_kb` 后轮转，保留 `log_keep_files` 个文件。

日志**不记录密码明文**；认证请求 URL 中若含凭据参数，写入前脱敏为 `***`。

## 6. 探测与重连逻辑

### 6.1 主动探测

每隔 `probe_interval_seconds`（默认 5 秒）向 `probe_url` 发起带 `Cache-Control: no-cache` 的请求，超时 3 秒。

判定规则（按序匹配，先命中者生效）：

- HTTP 204 → `Online`（`generate_204` 类地址的标准应答）
- HTTP 200 且响应体匹配预期标识 → `Online`
- HTTP 3xx 且 `Location` 主机与 `probe_url` 主机不同 → Portal 劫持，进入 `PortalDetected`
- HTTP 200 但响应体为空或不含预期标识 → Portal 劫持，进入 `PortalDetected`
- HTTP 200 且响应体含预期标识但长度异常偏大（> 8 KB）→ Portal 劫持（部分设备返回篡改后的完整页面）
- 连接失败 / 超时 / DNS 失败 → `Disconnected`，进入重试

### 6.2 被动触发（降低响应延迟）

注册 `NotifyAddrChange` 监听本地地址变更事件。DHCP 续约、网卡上下线、认证会话被踢导致的地址变化会立即触发一次探测，无需等待下个轮询周期。这是达成 S1（≤ 15 秒）的主要手段。

### 6.3 退避重试

登录失败后按 `retry_backoff_seconds` 序列退避：5s → 10s → 20s → 30s → 60s，之后固定 60 秒封顶。登录成功后序列重置。

退避上限存在的意义：避免高频重试触发校方系统的频率限制或验证码，反而使账号被锁。

### 6.4 网络环境变化

从 `Online` 变为其他状态时，清空缓存的 `NetContext`（本地 IP、Portal 页面），下次登录重新采集。避免使用过期 IP 发出无效请求。

## 7. 厂商识别

识别在 `Probe` 检测到 Portal 劫持后执行，输入是 Portal 的响应头与页面 HTML。

| 指纹特征 | 判定厂商 |
|---|---|
| 页面/URL 含 `srun_portal`、`get_challenge`、`srun_bx1`、或同时含 `ac_id` 与 `callback` | 深澜 Srun |
| URL 含 `/eportal/`、或含 `c=Portal`、或同时含 `wlanuserip` 与 `nasip`、或含 `jsVersion` | 锐捷 eportal |
| URL 含 `/drcom/` 或 `0MKKey` | Dr.COM |
| URL 含 `/portal/login` 或 `iPortal` 或 `portalpage` | 华为 iPortal（走 GenericForm 实现） |
| 以上均不匹配 | 通用表单 |

识别结果记录到日志，并在 UI 首次连接时以状态栏文字告知用户，满足「帮我识别」的需求。

## 8. 错误处理

| 场景 | 处理 |
|---|---|
| Portal 页无法访问 | 保持重试，状态 `Disconnected`，不做登录尝试 |
| 识别失败 | 回退到 `GenericFormAuth`；若表单解析也失败，日志给出明确提示引导用户改用自定义模板 |
| 凭据错误（厂商返回明确错误） | 停止重试，托盘气泡提示「账号或密码错误」，状态钉在 `AuthFailed`，等待用户改配置 |
| 凭据错误与网络错误无法区分 | 保守起见按网络错误处理，继续退避重试；连续失败 10 次后停止并通知用户 |
| 配置文件损坏 | 备份为 `config.json.bak` 后使用默认配置，日志记录 |
| DPAPI 解密失败（配置来自其他机器/用户） | UI 提示需重新输入密码，`has_password` 置 false |
| 已有实例在运行 | 单实例互斥体检测到后，`--ui` 调用转为唤起既有实例的 UI，不重复起守护 |
| UI 子进程崩溃 | 守护进程不受影响，继续工作 |

## 9. 测试策略

### 9.1 可在开发机验证（C++ 单测，doctest）

- `SrunXEncode`：固定输入 → 固定输出，与已知实现交叉验证
- 自定义 base64 码表编解码往返
- HMAC-MD5 / MD5 与 BCrypt 输出对照标准测试向量
- 各认证策略的**请求拼装**：给定 Profile 与 NetContext，断言生成的 URL/body 字符串正确
- 厂商指纹识别：构造样本 HTML/URL，断言识别结果
- 配置 JSON 序列化往返
- DPAPI 加密解密往返
- 退避序列生成

### 9.2 模拟服务器端到端测试

内置 `--simulate=<vendor>` 模式，在本地 `127.0.0.1` 起一个模拟 Portal 服务器，返回对应厂商格式的挑战与响应。该模式下 `probe_url` 与 `auth_url` 均被强制指向本机模拟服务器（忽略配置中的线上地址，且不写入配置文件），守护进程对着它走完整流程：探测 → 识别 → 登录 → 成功判定。

模拟服务器同时覆盖失败路径：`--simulate-fail=badcred` 返回厂商的凭据错误码，`--simulate-fail=timeout` 挂起连接，用于验证退避与错误分类逻辑。

这使协议逻辑能在**没有真实校园网**的情况下端到端验证，是本项目的主要测试手段。

### 9.3 需在真实校园网验证（开发机无法覆盖）

- 真实 Portal 的响应格式与模拟的差异
- 真实运营商参数取值
- 真实会话超时行为下 S1 是否达标

缓解：内置 `--probe-now` 与 `--test-login` 命令行开关，输出完整请求与响应（脱敏），供用户在其网络下运行后反馈，据此校正指纹表与协议实现。

**此风险不可在开发阶段消除，必须在用户真实网络环境下完成至少一次验证。**

## 10. 发布

### 10.1 两个发布包

| 包 | 内容 | 体积 | 依赖 |
|---|---|---|---|
| 精简版 | `CampusNet-Keeper.exe` | ~4 MB | 需 .NET 9 Desktop Runtime |
| 完整版 | 同上，但 .NET 运行时经压缩内嵌进 exe 资源，首次打开 UI 时解压到 `%LOCALAPPDATA%` | ~45 MB | 无 |

完整版的守护功能与精简版完全一致；二者仅在「能否打开设置界面」上有差别。若运行时缺失，守护进程仍正常工作，仅在用户点「设置」时弹出原生对话框，提供下载链接。

### 10.2 构建

`CMakeLists.txt`（原生部分）+ `dotnet publish`（UI 部分），由 `scripts/build.ps1` 串联：

1. 构建 UI：`dotnet publish src/ui -c Release -r win-x64`
2. 生成资源：把 UI 产物（或加上自包含运行时）压成 payload
3. 构建原生：`cmake --build` 产出 `CampusNet-Keeper.exe`
4. 输出到 `dist/`

### 10.3 GitHub

- 仓库：`LunaRime/CampusNet-Keeper`，公开，MIT License
- `.github/workflows/build.yml`：`windows-latest` 上构建，打 tag（`v*`）时创建 Release 并挂上两个包
- README 中英文双语，含截图、使用说明、常见问题（识别失败怎么办 / 自定义模板怎么写）

## 11. 文件结构

```
CampusNet-Keeper/
├─ CMakeLists.txt
├─ LICENSE                     MIT
├─ README.md                   中英双语
├─ .github/workflows/build.yml
├─ docs/superpowers/specs/     本设计文档
├─ scripts/
│   ├─ build.ps1
│   └─ package.ps1
├─ third_party/
│   ├─ json.hpp                nlohmann/json 单头
│   └─ doctest.h
├─ src/native/
│   ├─ main.cpp                形态分发
│   ├─ core/                   Http · Probe · Scheduler · Log
│   ├─ auth/                   IAuth · Detector · 5 个策略实现 · AuthFactory
│   ├─ crypto/                 BCrypt 封装 · SrunXEncode
│   ├─ store/                  Config · Dpapi
│   ├─ ipc/                    PipeServer
│   ├─ tray/                   TrayIcon
│   ├─ sys/                    Autostart · SingleInstance
│   ├─ ui_host/                Hostfxr · PayloadExtract
│   └─ sim/                    模拟 Portal 服务器
├─ src/ui/                     Avalonia 工程
└─ tests/                      doctest 用例
```

## 12. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 无法在开发机验证真实校园网 | 高 | 模拟服务器 + `--probe-now` 让用户实测反馈；通用表单与自定义模板两层兜底 |
| 学校使用非 HTTP Portal（802.1X） | 高（方案失效） | 已确认为 Portal 场景；若实测不符，需重新设计 |
| 各校锐捷 `service` 取值不统一 | 中 | 自定义输入框；识别时尝试从页面提取候选列表 |
| .NET 运行时缺失 | 低 | 完整版内嵌运行时；缺失时仅 UI 受影响 |
| 深澜 xEncode 实现偏差 | 中 | 单测对已知向量；`--test-login` 输出请求供比对 |
| 首次打开 UI 的解压延迟 | 低 | 仅首次；后续命中已解压缓存 |

## 13. 未决事项

无。所有设计决策已在本文档内明确。
