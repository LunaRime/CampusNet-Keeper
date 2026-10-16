# CampusNet-Keeper

校园网自动重连守护程序。会话失效时自动重新登录，让你在游戏里感觉不到掉线。

> 当前为 **M1（核心）** 阶段：命令行守护程序已可用，设置界面在 M2 提供。

## 特性

- **原生实现**：C++17，静态链接运行库（/MT），零动态库依赖
- **极低占用**：后台常驻内存约 16 MB，空闲 CPU ≈ 0%（500ms 定时器驱动）
- **自动重连**：网络断开或被 Portal 劫持时自动重新登录（Dr.COM 完整实现）
- **厂商识别**：自动识别 Portal 类型（Dr.COM / 锐捷 / 深澜 / 华为，其余回退通用表单/自定义模板）
- **密码加密**：使用 Windows DPAPI，配置文件被拷走也无法在其它机器解密
- **网络感知**：监听系统地址变更（`NotifyAddrChange`），切网/睡眠唤醒后立即探测

## 快速开始

    CampusNet-Keeper.exe --detect                      # 识别你学校的认证系统
    CampusNet-Keeper.exe --probe-now                   # 检查当前网络状态
    CampusNet-Keeper.exe --set-password --user=学号    # 录入密码（DPAPI 加密保存）
    CampusNet-Keeper.exe --test-login                  # 试一次登录（需先在门户注销）
    CampusNet-Keeper.exe                               # 后台守护（托盘驻留）

完整步骤与反馈要求见 `docs/M1-实测指南.md`。

## 命令行选项

| 选项 | 说明 |
|---|---|
| （无选项） | 以守护模式运行，托盘驻留 |
| `--probe-now` | 执行一次网络探测并输出结果 |
| `--test-login` | 一次探测 + 登录尝试，输出完整请求（已脱敏） |
| `--detect` | 厂商识别与运营商列表 |
| `--set-password` | 录入密码（配 `--user=账号` 指定目标；支持环境变量 `CNK_PASSWORD`） |
| `--config=<路径>` | 使用指定的配置文件 |
| `--simulate=<厂商>` | 使用本地模拟 Portal 服务器（开发/测试用） |
| `--version` / `--help` | 版本 / 用法 |

配置文件位于 `%APPDATA%\CampusNet-Keeper\config.json`；
日志位于 `%APPDATA%\CampusNet-Keeper\logs\keeper.log`。

## 支持的认证策略

| 厂商 | 策略 | 实测状态 |
|---|---|---|
| Dr.COM 哆点（`:801` 新式 API + AES 参数） | `DrcomAuth` | **真机实测通过**（2026-10-16，见 `docs/evidence/newapi_login_20261016.md`） |
| 锐捷 eportal（`/eportal/?c=Portal&a=login` + JSONP） | `EportalAuth` | **未实测**（本机环境为 Dr.COM，无锐捷校园网） |
| 深澜 srun（`get_challenge` → `srun_portal`，HMAC-MD5 + xEncode） | `SrunAuth` | **未实测**（同上） |
| 未知厂商 / 改版门户（解析页面第一个表单） | `GenericFormAuth` | 未实测（兜底策略） |
| 任意学校（自定义模板） | `CustomTemplateAuth` | 已在模拟环境验证 |

## 已知限制

1. **锐捷 / 深澜 / 通用表单三条策略尚未在真实校园网验证**——本机所在学校是 Dr.COM。
   它们的正确性目前依据：单元测试 + 本地模拟服务器端到端（`--simulate=<厂商>`）。
   若你在锐捷/深澜学校使用，`--test-login` 的完整输出就是唯一的现场判据，请把输出贴回。
   自测方法：`CampusNet-Keeper.exe --simulate=eportal --test-login`（把 `eportal` 换成
   `srun` / `generic` / `drcom` 可分别验证四条链路）。
2. **深澜的加密细节无官方文档**：实现依据是**三份独立来源**交叉核对（深大 srun 逆向的
   门户 `Portal.js` 原文 + 可运行 Python 脚本、Rust 客户端 `srunc`、Rust 客户端
   `bitgateway-client`）——包括 `chksum = SHA1(chkstr)`（**不是** HMAC-MD5）、
   `password = {MD5} + HMAC-MD5(密码, challenge)`、`info = {SRBX1} + 换表base64(xEncode(...))`、
   `n=200/type=1/double_stack=0`、base64 带标准 padding。xEncode 的可复算脚本见
   `tools/srun_xencode_oracle.js`（两份独立实现 150 组输入零差异），`chksum` 另有
   **请求级 KAT**（在测试里独立复算，不与产品共享公式）。
   已知变体：`srunc` 走「明文 `md5(密码)` + `double_stack=1`」，本实现按前两份来源；
   若现场反馈「参数看着对但不认账号」，先试这两个开关。
   **非 ASCII 密码**存在实现口径差异：本客户端按 UTF-8 字节打包（与 srunc/Python 一致），
   门户自己的 JS 用 `charCodeAt`（UTF-16 码元）；纯 ASCII 密码两者完全一致。
3. **配置里的探测地址**：默认 `http://captive.apple.com/hotspot-detect.html`。若你的学校
   未放行该地址，改为你校 `visit_blacklist` 里的任意地址（换地址后 `--probe-now` 应回 Online）。
4. **不要求管理员权限**：因此无法复位网卡/续租 DHCP——链路本身卡死时（连门户页都打不开）
   仍需人工处理（重插网线 / 禁用启用网卡）。

## 构建

依赖：Visual Studio 2022（含 C++ 桌面开发工作负载）。

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ./build/Release/cnk_tests.exe

## 许可证

MIT
