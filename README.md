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

## 构建

依赖：Visual Studio 2022（含 C++ 桌面开发工作负载）。

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ./build/Release/cnk_tests.exe

## 许可证

MIT
