# CampusNet-Keeper

校园网自动重连守护：会话失效时自动重新登录，让你在游戏里感觉不到掉线。

一个仓库两部分：**Windows 守护 + Avalonia 设置界面**（同一个 exe 双形态）、**路由器端子工程**（`router/`，在 OpenWrt 上自己认证并共享会话）。当前版本 **0.1.7**。

**最新发布**：[v0.1.7](https://github.com/LunaRime/CampusNet-Keeper/releases/tag/v0.1.7)（完整版 + 精简版两个 zip；SHA256 与签名证书见对应 Release 页）。历史：[v0.1.6](https://github.com/LunaRime/CampusNet-Keeper/releases/tag/v0.1.6) / [v0.1.4](https://github.com/LunaRime/CampusNet-Keeper/releases/tag/v0.1.4) / [v0.1.3](https://github.com/LunaRime/CampusNet-Keeper/releases/tag/v0.1.3) / [v0.1.1](https://github.com/LunaRime/CampusNet-Keeper/releases/tag/v0.1.1) / [v0.1.0](https://github.com/LunaRime/CampusNet-Keeper/releases/tag/v0.1.0)。

| 部分 | 目录 | 语言 / 运行位置 | 说明 |
|---|---|---|---|
| 守护 + CLI + 托盘 | `src/native/` | C++17 / Windows | 探测、认证、调度、IPC、单 exe 宿主；**不依赖 .NET** |
| 设置界面 | `src/ui/` | C# / Avalonia 11 | 框架依赖，需 .NET 8/9/10；按需从 exe 载荷释放 |
| 路由器端 | `router/` | C99 / OpenWrt | 独立子工程，纯静态交叉编译，与 Windows 端不共用构建 |

---

## 快速开始（Windows）

    CampusNet-Keeper.exe                              # 双击/无参数：守护 + 自动打开设置界面
    CampusNet-Keeper.exe --autostart                  # 开机自启：静默驻留托盘，不弹界面
    CampusNet-Keeper.exe --probe-now                  # 检查当前网络状态
    CampusNet-Keeper.exe --test-login                 # 试一次登录（需先在门户注销）
    CampusNet-Keeper.exe --detect                     # 识别学校认证系统与运营商
    CampusNet-Keeper.exe --set-password --user=学号   # 录入密码（DPAPI 加密）
    CampusNet-Keeper.exe --export-secret=D://s.json   # 导出密码（口令加密，可拷到别的电脑）
    CampusNet-Keeper.exe --import-secret=D://s.json   # 在新电脑导入

完整现场步骤见 `docs/M1-实测指南.md`。

配置文件 `%APPDATA%\CampusNet-Keeper\config.json`（可漫游，**不含本机密文**）；密码库 `%LOCALAPPDATA%\CampusNet-Keeper\secret.json`（DPAPI 绑定「本机 + 当前 Windows 账号」）；日志 `%APPDATA%\CampusNet-Keeper\logs\keeper.log`。

---

## Windows 守护

- **单 exe 双形态**：界面程序集以**存储型 ZIP** 内嵌（不引第三方解压库），首次打开设置时释放到 `%LOCALAPPDATA%\CampusNet-Keeper\ui`；载荷带版本戳，升级后自动重释放。`--ui-info`：0=就绪，3=精简版，4=缺 .NET 运行时。
- **GUI 子系统**：双击/自启不闪黑窗；从 cmd/pwsh 运行会 `AttachConsole` 接上父控制台，仍能 Ctrl+C。
- **三层判定**：探测页 → AC 复核（`chkstatus`，30s 节流且复用上次结论）→ 出网验证。默认探测目标 `http://connectivitycheck.platform.hicloud.com/generate_204`，`probe_expect` 留空；**204 也必须走后两层**，否则僵尸会话查不出来。`captive.apple.com` 在本网被 DNS 劫持且上游 DNS 就是 AC，已弃用。
- **探测节奏**：间隔默认 **30 秒**（配置里 `<10` 或 `>3600` 钳回 30）；超时 6 秒；退避 `5,10,20,30,60`。已有旧配置里的探测地址不会自动改写，需手动换。
- **会话异常自愈**：「终端 IP 已在线 / inuse」判成功后复测；僵尸会话、账号被占会先注销再重登，连续 3 次无效即停手并提示去门户处理，不做无意义空转。
- **网络感知**：`NotifyAddrChange` 监听地址变化，切网/睡眠唤醒立即探测；网关 ARP 状态写进 `get_status`，`ARP 未解析` 表示二层断，此时重试登录无效。
- **退出留痕**：`last_exit.txt` 记录正常退出的来源（托盘菜单 / WM_CLOSE / 关机 / Ctrl+C），读不到且非首次会 WARN；排查「守护进程莫名消失」先看日志开头。
- **构建戳**：`--version` 第一行 `CampusNet-Keeper 0.1.7`，第二行 `git <短哈希>, built <UTC 时间>`（CMake 注入），用于证明真机跑的是哪次产物。

### 托盘（0.1.7 重构）

守护进程没有可最小化的窗口，从启动起只以托盘图标常驻；图标是**应用图标 + 右下角状态圆点**，颜色与设置界面 v2 同源（在线绿 / 探测认证蓝 / 认证失败红 / 非校园网琥珀 / 未连接灰）。左键单击或双击打开设置界面，右键弹出原生 Win32 菜单：

| 菜单项 | 行为 |
|---|---|
| `状态：<中文状态> · <调度器消息>` | 灰显；暂停时追加「已暂停自动重连」 |
| `版本 0.1.7 · git <短哈希>` | 灰显，便于对照产物 |
| 打开设置… | 启动 `--ui`；界面已在跑则还原并置前；精简版弹气泡说明 |
| 立即重连 | 立即触发一次探测/重连，**暂停态下也放行** |
| 暂停自动重连 / 恢复自动重连 | 暂停时仍探测、仍显示状态，只是**不自动发起登录** |
| 打开日志目录 | 打开 `...\logs`（与设置界面同名按钮一致） |
| 退出 | 记录退出来源「托盘菜单『退出』」后退出 |

设置窗口最小化后是普通任务栏按钮，右键为 Windows 系统菜单；**关窗即进程退出**，不会缩回托盘。再点托盘「打开设置…」会 `SW_RESTORE` 还原并置前。

### 命令行选项

| 选项 | 说明 |
|---|---|
| （无选项） / `--autostart` / `--ui` / `--ui-info` | 见「快速开始」与上文 |
| `--probe-now` / `--test-login` / `--detect` | 探测 / 试登录 / 厂商识别 |
| `--set-password` / `--logout` | 录入密码 / 主动注销会话（可配 `--user=账号`） |
| `--export-secret=<文件>` / `--import-secret=<文件>` | 口令取环境变量 `CNK_PASSPHRASE` 或交互输入 |
| `--config=<路径>` | 指定配置文件（该形态下密文内联） |
| `--simulate=<厂商>` | 本地模拟 Portal（`drcom` / `eportal` / `srun` / `generic`） |
| `--version` / `--help` | 版本与构建戳 / 用法 |

### 认证策略

| 厂商 | 策略 | 真机状态 |
|---|---|---|
| Dr.COM 哆点（`:801` 新式 API，AES-128-ECB `params`） | `DrcomAuth` | **实测通过**（2026-10-16，见 `docs/evidence/`） |
| 锐捷 eportal | `EportalAuth` | 未实测（本机是 Dr.COM） |
| 深澜 srun（SHA1 chksum + xEncode） | `SrunAuth` | 未实测，三份独立来源交叉核对 |
| 通用表单 / 自定义模板 | `GenericFormAuth` / `CustomTemplateAuth` | 模拟环境端到端验证 |

`vendor=auto` 陷阱：运营商后缀按档案 vendor 分支、策略按识别结果选，认证前必须写进档案副本。后缀 `campus=""` / `cmcc=@cmcc` / `telecom=@dx` / `unicom=@lt`。自定义模板写在配置的 `profiles[].custom_template`，占位符 `{username} {password} {isp} {ip} {timestamp}`（**不做 URL 编码**，需自行拼装），`success_regex` 判成功；细节见 `core/Template.h`。

---

## 设置界面（UI v2）

v0.1.7 起界面**全面推倒重写**，设计真源是 `DESIGN.md` v2「现代桌面原生」（取代原 shadcn/ui 纯黑白网页系统），落地映射见 `docs/design/avalonia-mapping.md`。可交互高保真原型：`docs/design/ui-redesign-prototype-20261110.html`（离线单文件，支持 `?theme=dark&page=logs&state=offline` 直达）。

**布局**：默认窗口 **1000×700**（最小 880×600），保留系统标题栏；应用头 48px、侧栏 208px、底栏 64px，只有内容区滚动。侧栏同时承载导航、档案快捷列表与守护状态；底栏固定刷新 / 测试登录 / 保存，长错误文案强制截断，不会把按钮顶出窗口。

| 分区 | 内容 |
|---|---|
| 概览 | 状态大卡、四段链路图（本机→网关→门户→互联网）、本机 IP / 网关 ARP / 上次登录 / 守护四项指标、最近事件、当前档案与探测参数 |
| 配置档案 | 档案 chips、认证网址 / 账号 / 密码、运营商、认证方式、自定义后缀、密码三态提示 |
| 高级设置 | 探测间隔 / 超时 / 地址 / 退避 / 日志级别、开机自启与通知、密码迁移导出导入 |
| 运行日志 | 等宽控制台、级别着色、2 秒自动刷新、打开日志目录 |

**设计系统**：克制品牌蓝 `#2f6bff` 只用于主操作与选中态；明暗主题各 **47 个 `Cnk*` 令牌**（品牌浅底、语义浅底、两级 `BoxShadows`），控件圆角 8 / 卡片 12 / 大面 16，字体栈 `Segoe UI Variable → Microsoft YaHei UI`（中文回退必需）。色值只出现在 `Theme.Light/Dark.axaml`，控件样式全部集中在 `Controls.axaml`，视图只写结构与绑定。状态语义：离线用中性灰不用红，色永不单独承载含义。

**分区导航是纯视图层状态**：`MainWindow.NavigateTo(tag)` 切 `IsVisible` 与 `Classes.active`，不新增 ViewModel 字段，IPC 契约与 `MainViewModel` / `ProfileViewModel` 的数据语义不变。所有控件都对应真实属性（6 种状态、6 种认证方式、密码三态、11 个命令）。

**密码三态**：`ok` 已保存 / `foreign` 来自其他电脑或账号（**保留原密文原样写回**，绝不因点一次保存删掉别人的密码）/ `none` 未设置。界面**永不读取已存明文**，只显示「已保存密码」状态；跨机迁移只走显式导出/导入（PBKDF2-SHA256 200k + AES-256-GCM），绝不使用 `CRYPTPROTECT_LOCAL_MACHINE`。保存 ≠ 能登录：当前档案没有密码时界面会明确提示用「测试登录」做一次真实认证验证。

**验收**：`pwsh -File tools/check_ui_tokens.ps1`（XML 良构、明暗键一一对应、禁裸色值、引用可解析、长文案截断、按钮居中）、`CampusNet.UI.exe --self-test`（32 个色键 × 明暗 + 圆角/字号/字体/图标）、`dotnet build`、ContractCheck 跨语言契约。

---

## 路由器端子工程（`router/`）

**定位**：路由器自己完成认证，NAT 后的手机/电脑共享同一会话；电脑端守护可以停掉。只实现 Dr.COM + `http://`，AES 与主项目 BCrypt 516 组交叉校验零差异；`e2e_test.py` 用纯 Python 独立解密 `params` 逐字段断言。

**真机**：Kwrt 24.10.4 / aarch64 / ipq60xx / WAN DHCP，常驻 VSZ 约 284 KB；CI 用 OpenWrt 官方 SDK 交叉编译，产物纯静态（`readelf -d` 无 `NEEDED` 是门禁）。内存选型与目录结构见 `router/README.md` 第一、九节。

**三层判定**（2026-11-09 改造，与 Windows 端同源）：探测页 → AC 复核 → 出网验证。探测默认 `http://connectivitycheck.platform.hicloud.com/generate_204`（实测 204 / 0.038s / 无重定向），`probe_expect` 置空；MIUI `generate_204` 保留作出网验证的独立目标。**204 绝不能直接判在线**。`captive.apple.com` 在本网被 DNS 劫持（CNAME 到 CDN），已弃用。

**DNS 抖动识别**（2026-11-10）：把「域名解析失败」与真断网区分开——第 1 层 DNS 失败时用 IP 直连的 AC 复核 + 门户可达 + 近期曾解析成功做二次确认，全通过则进入 `NET_DNS` 观察态（保持在线、限频 WARN、不计 `offline_streak`），1.5s 快重探 ×3 后才降级为真实离线；另有解析结果缓存（8 槽 / TTL 1h）与 `connect_ip` + 自定义 Host 头兜底。旁路只在 DNS 失败时生效，不碰 204/200/3xx 三层判定。

**AC 会话回收取证**：环形缓冲存最近 8 次 `chkstatus` 快照（result / olmac / ss1 / oltime / v4ip），离线判层前回放并现场再问 AC。三种死法：整段黑洞（最常见）、僵尸会话（AC 说在线但出不去网）、瞬时 DNS 抖动。实测根因是 **AC 按 WAN IP 把转发面黑洞**（DNS 服务器就是 AC，一黑洞 DNS + 门户一起挂）。恢复靠软拔插：`handle_link_down` 判网关 ARP 未解析 + 门户不可达 → `ip link set wan down/up` 换 IP，固有耗时 7~10s。在线持续期静默，状态切换才打日志。

**运维脚本**：`cnk-traffic` 用 nftables named counter 在 forward 链按 LAN IP 统计字节（cron `*/5`，日志保留 3 天）；`cnk-cleanup` 清旧日志（主日志 7 天；busybox `find` 没有 `-delete`，用 `-exec rm -f {} +`）。

**防检测**（按需）：TTL 已统一为 64（`/usr/share/nftables.d/.../10-ttl.nft`，验证临时挂 `ip ttl != 64 log` 命中 0 才算过）；IPv6 未暴露；UA 维度未处理，需要 UA2F/UA3F；TCP 时间戳属进阶项。边界与升级判据见 `router/README.md` 第六节。

**部署要点**：`build/.rput.py` 分块传 `/tmp`（无 sftp）→ 远程实跑 `--version` + `--selftest` → `mv` 替换（避开 `ETXTBSY`）→ `uci set` → **必须 `restart`**（`reload` 只发 SIGHUP，不重新渲染配置）。`SIGTERM` 不注销，换版不断网。`.gitattributes` 钉 `router/**`→LF（CRLF 会毁 init 脚本）。改默认值要同步 4 处：`config_defaults()`、UCI 模板、`init.d` 的 `config_get`、`.deploy_conf.py`。

**连通性检查 7 层**（命令均真机实测，见 `router/README.md` 第七节）：L0 链路 `ip neigh` / ping 网关、L1 DNS、L2 `wget -S | grep HTTP/`、L3 `chkstatus`、L4 MIUI/百度、L5 `cnkrouter --probe`、L6 日志与取证、L7 nft counter。busybox `wget -w` 是 `--wait` 不是 curl 的 `--write-out`，`-T 5` 才是超时。

> ⚠ 操作纪律：`busybox` 上任何命令先验证选项语义（曾因 `passwd -S root` 未知参数被丢弃而把 root 密码改空、SSH 全断）；「看着在跑的 cron」不等于真的清理成功，必须看删除结果。

---

## 构建

依赖：Visual Studio 2022（C++ 桌面开发）、Windows SDK；设置界面额外需要 .NET SDK 10。

    pwsh -File build.ps1                    # 精简版单 exe（不含界面载荷）
    pwsh -File build.ps1 -SingleExe         # 完整版：发布界面 → 存储型载荷 → 编进 exe
    pwsh -File build.ps1 -SingleExe -Sign   # 同上并打数字签名
    pwsh -File scripts/package.ps1          # 正式发布：精简版 + 完整版两个 zip

脚本末步会把产物同步到 `build\Release`、断言新 exe 含本次构建标记并比对 SHA256 —— 曾出现「新功能只在开发树、用户跑着旧二进制」的事故，所以把同步固化成脚本行为。

手动等价：`cmake -S . -B build-m1 -G "Visual Studio 17 2022" -A x64` → `cmake --build build-m1 --config Release` → `./build-m1/Release/cnk_tests.exe`；界面 `dotnet build src/ui/CampusNet.UI -c Release` + `src/ui/ContractCheck`。

> **MSB6001**：本机环境同时存在大小写两种 `*_PROXY` 变量时，MSBuild 的 Hashtable 会报重复键。PowerShell 设空串无效，用 Git Bash `env -u HTTP_PROXY -u HTTPS_PROXY -u ... -u https_proxy` 精确删除后再 `MSYS_NO_PATHCONV=1 cmake --build ...`。

`scripts/package.ps1` 产出到 `dist/`：精简版 zip（单 exe，守护/CLI 完整，点「设置」提示用完整版）与完整版 zip（单 exe 双形态，exe 约 24 MB）。两个包都会**解包自检**：对解出的 exe 跑 `--version` 与 `--ui-info`，精简版必须退 3、完整版必须退 0 且载荷「全部为存储型 + CRC 校验通过」。

## 测试与验证

    ./build-m1/Release/cnk_tests.exe              # C++ 全量测试（当前 311 例 / 1395 断言）
    pwsh -File tools/check_ui_tokens.ps1          # UI 令牌静态检查（不需编译器）
    CampusNet.UI.exe --self-test                  # 明暗令牌逐值断言
    pwsh -File tools/verify_ui_host.ps1           # 载荷释放 → 弹窗 → 关窗退出 → 版本戳
    pwsh -File tools/verify_tray_open_ui.ps1      # 托盘「设置…」拉起 --ui
    pwsh -File tools/verify_startup_ux.ps1        # 双击自动开界面 / --autostart 静默
    pwsh -File tools/verify_autostart.ps1         # 自启端到端（会临时改动并恢复注册表）
    python docs/evidence/audit_redaction.py       # 证据文件脱敏审计

`verify_*.ps1` 需真机 GUI，不能在受限沙箱跑；跑之前先退出正在运行的托盘实例。IPC 测试用独立测试管道名，不会串到真守护。`build-m1\Release\cnk_ipc_probe.exe --seconds=45` 可起探针服务端，再跑 ContractCheck 验证跨语言契约。

## CI 与发布

`.github/workflows/ci.yml`：推送/PR 走「构建精简版 → 测试 → `--ui-info` 退 3 → 脱敏审计 → UI 令牌/契约检查」；推 `v*` tag 时先校验 tag 与 `Version.h` 一致，再用 `scripts/package.ps1` 构建两个包、解包自检后上传 GitHub Release（配 `CODESIGN_PFX_BASE64` / `CODESIGN_PFX_PASSWORD` 时自动签名）。

发版流程（**README 的「最新发布」链接必须与 tag 同一次提交**）：

    git push origin master:main
    git tag -a v0.1.7 -m "说明"
    git push origin v0.1.7

推送前跑 `python docs/evidence/audit_redaction.py`（CI 也会跑）：全仓文本查学号样数字串 / MAC / 私有网段，图片与压缩包资产须登记白名单。**新截图必须逐张打开看过**——栅格图里的文字文本审计看不见。发布说明里的 SHA256 必须取 **CI 产物**，不能取本地包。

## 安全与隐私

- 密码：`config.json`（可漫游，不含密文）+ `secret.json`（本机 DPAPI），解不开时保留原密文原样写回；界面进程永不接触已存明文。
- 跨机：只走显式导出/导入，口令加密；绝不放宽到 `CRYPTPROTECT_LOCAL_MACHINE`。
- 日志：`RedactSecrets` 抹凭据，取证快照里的 olmac/ss1 走 8 位哈希掩码；真实 `params` / `rcn` / HAR 一律不入库。
- 签名：自签名可消除本机「未知发布者」但 SmartScreen 仍会拦陌生机器；OV/EV 证书才能逐步建立信誉。`signtool.exe` 随 Windows SDK 分发，查找顺序见「数字签名」脚本 `scripts/sign.ps1`。

## 已知限制

1. 锐捷 / 深澜 / 通用表单三条策略未在真实校园网验证（本机是 Dr.COM），依据是单测 + `--simulate` 端到端。
2. 自包含界面载荷只在装了 .NET 的机器验证过，真「零依赖」还需一台无 .NET 的机器实测。
3. 契约自检的 `echo` / `blob` 是探针专用命令，对真守护跑会误报。
4. 路由器端 UA 维度未处理（需 UA2F/UA3F）；IPv6 未暴露；AC 黑洞频率不受客户端版本影响。
5. `captive.apple.com` 在本网被 DNS 劫持，旧配置里若还留着该地址需手动改；`connectivitycheck.gstatic.com`（历史实测 0/5）与新默认 `platform.hicloud.com` 是不同域名。
6. 托盘菜单仍是原生 Win32 样式（0.1.7 统一了功能与文案，未做自绘），视觉与 v2 界面不完全一致。

## 许可证

MIT
