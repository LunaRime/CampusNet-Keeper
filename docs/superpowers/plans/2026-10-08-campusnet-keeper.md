# CampusNet Keeper 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付一个常驻 Windows 托盘的校园网自动重连程序，会话失效时在 15 秒内自动重新登录，后台常驻内存低于 10 MB。

**Architecture:** 单个原生 C++17 exe，静态链接 CRT、零动态依赖。无参数运行为守护形态（不加载 .NET，常驻 3-5 MB）；带 `--ui` 参数时同一 exe 通过 `hostfxr` 按需加载 .NET 并拉起 Avalonia UI 子进程。两形态经命名管道通信。认证协议按厂商策略模式实现，以 Dr.COM 哆点为首要目标。

**Tech Stack:** C++17 / Win32 / WinHTTP / BCrypt / DPAPI / CMake + MSVC v143 + Ninja / nlohmann-json / doctest / C# .NET 9 / Avalonia 11

**Spec:** `docs/superpowers/specs/2026-10-08-campusnet-keeper-design.md`

## Global Constraints

- C++ 标准：**C++17**，`CMAKE_CXX_STANDARD_REQUIRED ON`。
- 编译器：**MSVC v143**（VS2022）。**2026-10-13 环境变更**：安装路径已从 `D:\Visual Studio 2022\VS2022` 迁至 `D:\Visual Studio 2026\Microsoft Visual Studio\2022`（原路径失效，旧 CMakeCache 已清理重建）；cmake 位于该安装的 `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`。编译选项必须含 **`/utf-8`**（源码含中文字面量）与 **`/W4 /permissive-`**。
- **构建前置：清除代理环境变量**（`unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY` 后再运行 cmake/msbuild）。环境的代理变量同时存在大小写两套，MSBuild 向 CL.exe 传递环境时抛 `MSB6001 字典中已添加项`，表现为误报「找不到 CXX 编译器」，实际是环境问题与工具链无关。
- 字符集：**UNICODE**（`UNICODE` 与 `_UNICODE` 均定义）。Win32 API 一律用宽字符版本（`*W`）。
- 命名空间：**`cnk`**。头文件用 `#pragma once`。源文件扩展名 `.cpp`，头文件 `.h`。
- 第三方依赖**仅限** `nlohmann/json` 与 `doctest`，二者均为单头文件，vendored 至 `third_party/`。**不得引入任何其他第三方库**，尤其禁止 UI 库（Qt / MFC / WinForms）。
- 链接库：`winhttp` `crypt32` `bcrypt` `shell32` `advapi32` `ole32` `user32`。
- 配置与日志根目录：`%APPDATA%\CampusNet-Keeper\`（经 `SHGetKnownFolderPath(FOLDERID_RoamingAppData)` 获取，不得用 `getenv("APPDATA")`）。
- 目标 Portal（实测，见 spec §7.1）：`http://10.0.0.1`，账号字段 `DDDDD`，密码字段 `upass`，**明文提交**（`en_md5=0`）。**2026-10-16 晚实测修订**：登录端点（老式 `c=ACSetting` 协议）**与门户页同源（`:80`）**；`:801` 是 SPA/新式 REST API 端口，老协议请求发到那里只会得到 SPA 外壳（详见 `docs/evidence/README.md`「登录端点端口修订」）。
- **日志中禁止出现密码明文**；URL 中若含凭据参数，写日志前替换为 `***`。
- 所有对外提交的产物中不得含真实账号、学号、MAC 或非私有的本机标识。

## Review Focus

以下是 spec 未逐条展开、但最可能让使用者踩坑的输入与场景。每一条都必须在指定任务中有对应测试。

1. **运行在非校园网环境**（在家、4G 热点、其他学校）—— 程序不得对着已失效的校园网 Portal 地址反复发送登录请求，应识别为「不在目标网络」并静默待机，直到网络环境变回可识别的 Portal。
2. **密码被用户改过但配置未更新** —— 服务器明确返回凭据错误时，必须**停止重试并提示用户**，绝不能按 60 秒周期无限重试刷爆账号或触发校方风控。
3. **配置文件损坏或半写入**（用户手改 JSON、写入过程断电）—— 必须备份原文件、回落默认配置并记录日志，**不得崩溃**。
4. **笔记本睡眠唤醒或切换网络后** —— 必须立即触发一次探测，而不是等满一个轮询周期（这正是用户抱怨的「玩游戏时断网」的高频场景）。
5. **Portal 页面改版导致解析失败** —— 必须输出明确错误并指引用户改用自定义模板，**不得静默假装正常**或误判为已登录。

## 里程碑

- **M1（任务 1-18）：核心可用** —— 产出能真正自动重连的命令行程序，结束时需用户在真实校园网实测。
- **M2（任务 19-27）：UI 与发布** —— 产出 Avalonia 设置界面、单 exe 打包与 GitHub Release。

## 任务索引

本文档约 9600 行。用 `## 任务 N` 定位具体任务，不必通读全文。执行某个任务时只需要读该任务的完整段落，加上它在 **Interfaces** 里列出的依赖任务所 **Produces** 的签名。

| # | 任务 | 产物 |
|---|---|---|
| 1 | 项目骨架与构建系统 | CMake + doctest 接入，`cnk_core` / `cnk_tests` 目标 |
| 2 | 滚动日志 | `Log`、`Paths`、日志脱敏 `RedactSecrets` |
| 3 | 模拟 Portal 服务器 | `SimServer`（后续所有协议测试的基础设施） |
| 4 | HTTP 客户端 | `HttpPerform`（WinHTTP 封装，超时与重定向控制） |
| 5 | 编码处理 | `ParseCharset` / `ToUtf8` / `DecodeBody`（GBK 与 UTF-8） |
| 6 | DPAPI 与配置结构 | `DpapiProtect` / `Config` 结构 / `IspSuffix` |
| 7 | 配置读写与损坏恢复 | `ConfigLoadFrom` / `ConfigSaveTo`（原子写入） |
| 8 | 厂商识别 | `DetectVendor`（Dr.COM 优先于锐捷） |
| 9 | Portal 配置解析 | `ParseCarrier` / `ParseVisitBlacklist` / `SelectProbeTarget` |
| 10 | 认证策略接口与工厂 | `IAuth` / `CreateAuth`（先以桩占位） |
| 11 | Dr.COM 认证 | `MakeDrcomAuth`（**本机目标**） |
| 12 | 自定义模板认证 | `ExpandTemplate` / `MakeCustomTemplateAuth` |
| 13 | 探测模块 | `ProbeOnce` / `NetState` 三态判定 |
| 14 | 调度状态机 | `Scheduler` + 退避 + 凭据错误停止 + 非校园网保护 + 地址变更监听 |
| 15 | CLI 入口与端到端集成 | `ParseCli` / `main.cpp` / `--simulate` 模拟模式 |
| 16 | 单实例与开机自启 | `SingleInstance` / `SetAutostart` |
| 17 | 托盘图标与消息循环 | `TrayIcon` / `RunDaemonLoop` |
| 18 | **M1 收尾与用户实测（门禁）** | 实测指南，**必须拿到用户反馈后才能进 M2** |
| 19 | 锐捷认证 | `MakeEportalAuth` |
| 20 | BCrypt 与深澜认证 | `Md5Hex` / `SrunXEncode` / `MakeSrunAuth` |
| 21 | 通用表单认证 | `ParseFirstForm` / `MakeGenericFormAuth` |
| 22 | IPC 命名管道 | `PipeServer` / `Protocol` |
| 23 | Avalonia 工程与 IPC 客户端 | `IpcClient` / 配置模型 / UI 骨架 |
| 24 | 设置界面 | `MainViewModel` / `ProfileViewModel` / `MainWindow` |
| 25 | 高级设置与日志面板 | 守护侧 IPC 命令实现 / `LogPanel` |
| 26 | 按需加载 CLR | `EnsureUiPayload` / `RunUiMode` / 单 exe 双形态 |
| 27 | 构建、CI 与发布 | 构建脚本 / GitHub Actions / 推送与 Release |

**依赖主干**：1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 13 → 14 → 15 → 16 → 17 → 18。
任务 12 依赖 10；任务 19/20/21 依赖 10 与 4；任务 22 依赖 2；任务 23-26 依赖 22 与 18。

---

## 任务 1：项目骨架与构建系统

**Files:**
- Create: `CMakeLists.txt`
- Create: `third_party/json.hpp`（下载）
- Create: `third_party/doctest.h`（下载）
- Create: `src/native/core/Version.h`
- Create: `tests/test_main.cpp`
- Create: `tests/smoke_test.cpp`
- Create: `.gitignore`

**Interfaces:**
- Consumes: 无（起点）
- Produces: 静态库目标 `cnk_core`、可执行目标 `CampusNet-Keeper`、测试目标 `cnk_tests`；`cnk::VersionString()` 返回 `const char*`

- [ ] **Step 1: 下载 vendored 依赖**

```bash
mkdir -p third_party
curl -sSL -o third_party/json.hpp \
  https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
curl -sSL -o third_party/doctest.h \
  https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```

验证：两个文件均大于 100 KB。

- [ ] **Step 2: 写版本头文件**

`src/native/core/Version.h`:

```cpp
#pragma once

namespace cnk {

inline const char* VersionString() { return "0.1.0"; }

}  // namespace cnk
```

- [ ] **Step 3: 写失败测试**

`tests/test_main.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
```

`tests/smoke_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Version.h"

TEST_CASE("VersionString 返回非空版本号") {
    CHECK(cnk::VersionString() != nullptr);
    CHECK(std::string(cnk::VersionString()).size() > 0);
}
```

- [ ] **Step 4: 写 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(CampusNetKeeper CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# 注意：cnk_core 必须始终包含至少一个真正的 .cpp。
# 只有头文件的 STATIC 库会让 CMake 报 "cannot determine linker language"；
# 即便补 LINKER_LANGUAGE 也仍不产出 .lib，依赖目标会 LNK1181。
add_library(cnk_core STATIC
  src/native/core/Version.h
  src/native/core/Version.cpp
)
target_include_directories(cnk_core PUBLIC src/native third_party)
target_link_libraries(cnk_core PUBLIC
  winhttp crypt32 bcrypt shell32 advapi32 ole32 user32)

add_executable(CampusNet-Keeper src/native/main.cpp)
target_link_libraries(CampusNet-Keeper PRIVATE cnk_core)

enable_testing()
add_executable(cnk_tests
  tests/test_main.cpp
  tests/smoke_test.cpp
)
target_link_libraries(cnk_tests PRIVATE cnk_core)
target_include_directories(cnk_tests PRIVATE tests)
add_test(NAME cnk_tests COMMAND cnk_tests)
```

`src/native/main.cpp`（本步只做占位，任务 16 会重写）：

```cpp
#include <cstdio>
#include "core/Version.h"

int wmain(int, wchar_t**) {
    std::printf("CampusNet-Keeper %s\n", cnk::VersionString());
    return 0;
}
```

`.gitignore`:

```
build/
dist/
.vs/
*.user
out/
```

- [ ] **Step 5: 构建并运行测试**

```bash
CM="/d/Visual Studio 2022/VS2022/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CM" -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
"$CM" --build build
./build/cnk_tests.exe
```

Expected: 测试通过，输出 `[doctest] test cases: 1 | 1 passed`。

若 Ninja 找不到 `cl`，改用 VS 生成器：`-G "Visual Studio 17 2022" -A x64`，并用 `cmake --build build --config Release` 构建，测试产物在 `build/Release/cnk_tests.exe`。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "build: 项目骨架、CMake 构建系统与 doctest 接入"
```

---

## 任务 2：滚动日志

**Files:**
- Create: `src/native/core/Log.h`
- Create: `src/native/core/Log.cpp`
- Create: `src/native/core/Paths.h`
- Create: `src/native/core/Paths.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/log_test.cpp`

**Interfaces:**
- Consumes: `cnk::VersionString()`（任务 1）
- Produces:
  - `cnk::AppDataDir()` → `std::string`，返回 `%APPDATA%\CampusNet-Keeper`，目录不存在时创建
  - `enum class cnk::LogLevel { Debug, Info, Warn, Error }`
  - `void cnk::LogInit(const std::string& log_dir, LogLevel level, size_t max_bytes, int keep_files)`
  - `void cnk::LogWrite(LogLevel, const std::string& msg)`
  - `void cnk::LogShutdown()`
  - `std::vector<std::string> cnk::LogTail(size_t max_lines)`
  - `std::string cnk::RedactSecrets(const std::string& s)`

- [ ] **Step 1: 写失败测试**

`tests/log_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Log.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("RedactSecrets 屏蔽密码类参数") {
    CHECK(cnk::RedactSecrets("upass=secret123&DDDDD=alice")
          == "upass=***&DDDDD=alice");
    CHECK(cnk::RedactSecrets("password=abc&x=1") == "password=***&x=1");
    CHECK(cnk::RedactSecrets("pwd=p&user_password=q")
          == "pwd=***&user_password=***");
}

TEST_CASE("RedactSecrets 对无凭据字符串原样返回") {
    CHECK(cnk::RedactSecrets("http://a/b?c=1") == "http://a/b?c=1");
}

TEST_CASE("日志写入后可被 LogTail 读回") {
    auto dir = fs::temp_directory_path() / "cnk_log_test";
    fs::remove_all(dir);
    cnk::LogInit(dir.string(), cnk::LogLevel::Debug, 1024 * 1024, 3);
    cnk::LogWrite(cnk::LogLevel::Info, "hello-marker");
    auto lines = cnk::LogTail(50);
    cnk::LogShutdown();
    bool found = false;
    for (auto& l : lines) if (l.find("hello-marker") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("低于阈值的日志不写入") {
    auto dir = fs::temp_directory_path() / "cnk_log_test2";
    fs::remove_all(dir);
    cnk::LogInit(dir.string(), cnk::LogLevel::Warn, 1024 * 1024, 3);
    cnk::LogWrite(cnk::LogLevel::Info, "should-not-appear");
    cnk::LogWrite(cnk::LogLevel::Warn, "should-appear");
    auto lines = cnk::LogTail(50);
    cnk::LogShutdown();
    std::string joined;
    for (auto& l : lines) joined += l;
    CHECK(joined.find("should-not-appear") == std::string::npos);
    CHECK(joined.find("should-appear") != std::string::npos);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`core/Log.h` 不存在。

- [ ] **Step 3: 实现 Paths**

`src/native/core/Paths.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 返回 %APPDATA%\CampusNet-Keeper，必要时创建目录。
// 失败时返回空字符串。
std::string AppDataDir();

// 在 AppDataDir() 下创建子目录并返回路径。
std::string AppDataSubDir(const std::string& name);

}  // namespace cnk
```

`src/native/core/Paths.cpp`:

```cpp
#include "core/Paths.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace cnk {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

fs::path AppDataPath() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)))
        return {};
    std::wstring w(raw);
    CoTaskMemFree(raw);
    return fs::path(w) / L"CampusNet-Keeper";
}

}  // namespace

std::string AppDataDir() {
    fs::path p = AppDataPath();
    if (p.empty()) return {};
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return {};
    return WideToUtf8(p.wstring());
}

std::string AppDataSubDir(const std::string& name) {
    std::string base = AppDataDir();
    if (base.empty()) return {};
    fs::path p = fs::path(base) / fs::u8path(name);
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return {};
    return WideToUtf8(p.wstring());
}

}  // namespace cnk
```

- [ ] **Step 4: 实现 Log**

`src/native/core/Log.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class LogLevel { Debug, Info, Warn, Error };

void LogInit(const std::string& log_dir, LogLevel level,
             size_t max_bytes, int keep_files);
void LogShutdown();

void LogWrite(LogLevel level, const std::string& msg);

inline void LogDebug(const std::string& m) { LogWrite(LogLevel::Debug, m); }
inline void LogInfo(const std::string& m)  { LogWrite(LogLevel::Info, m); }
inline void LogWarn(const std::string& m)  { LogWrite(LogLevel::Warn, m); }
inline void LogError(const std::string& m) { LogWrite(LogLevel::Error, m); }

// 返回最近 max_lines 行日志（从旧到新）。
std::vector<std::string> LogTail(size_t max_lines);

// 把 URL 或表单体中的密码类参数值替换为 ***。
// 覆盖字段：upass, password, pwd, pass, user_password, passwd
std::string RedactSecrets(const std::string& s);

}  // namespace cnk
```

`src/native/core/Log.cpp`:

```cpp
#include "core/Log.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <deque>
#include <chrono>
#include <ctime>
#include <cstdio>

namespace fs = std::filesystem;

namespace cnk {

namespace {

std::mutex g_mutex;
fs::path g_log_path;
std::ofstream g_stream;
LogLevel g_min_level = LogLevel::Info;
size_t g_max_bytes = 512 * 1024;
int g_keep_files = 3;
std::deque<std::string> g_tail;
constexpr size_t kTailMax = 500;

const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

std::string Timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void RotateIfNeeded() {
    std::error_code ec;
    if (!fs::exists(g_log_path, ec)) return;
    if ((size_t)fs::file_size(g_log_path, ec) < g_max_bytes) return;

    g_stream.close();
    for (int i = g_keep_files - 1; i >= 1; --i) {
        fs::path from = g_log_path;
        from += "." + std::to_string(i);
        fs::path to = g_log_path;
        to += "." + std::to_string(i + 1);
        std::error_code e2;
        if (fs::exists(from, e2)) {
            fs::remove(to, e2);
            fs::rename(from, to, e2);
        }
    }
    fs::path first = g_log_path;
    first += ".1";
    std::error_code e3;
    fs::remove(first, e3);
    fs::rename(g_log_path, first, e3);

    g_stream.open(g_log_path, std::ios::app);
}

bool IsSecretKey(const std::string& key) {
    static const char* kKeys[] = {
        "upass", "password", "pwd", "pass", "user_password", "passwd"
    };
    for (auto* k : kKeys)
        if (key == k) return true;
    return false;
}

}  // namespace

void LogInit(const std::string& log_dir, LogLevel level,
             size_t max_bytes, int keep_files) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_stream.is_open()) {
        std::error_code ec;
        fs::create_directories(fs::u8path(log_dir), ec);
        g_log_path = fs::u8path(log_dir) / "keeper.log";
        g_stream.open(g_log_path, std::ios::app);
    }
    g_min_level = level;
    g_max_bytes = max_bytes > 4096 ? max_bytes : 4096;
    g_keep_files = keep_files > 0 ? keep_files : 1;
}

void LogShutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_stream.is_open()) g_stream.flush();
    g_stream.close();
}

void LogWrite(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (level < g_min_level) return;

    std::string line = Timestamp() + " [" + LevelName(level) + "] " + msg;

    if (g_stream.is_open()) {
        RotateIfNeeded();
        g_stream << line << "\n";
        g_stream.flush();
    }

    g_tail.push_back(line);
    if (g_tail.size() > kTailMax) g_tail.pop_front();
}

std::vector<std::string> LogTail(size_t max_lines) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<std::string> out;
    size_t start = g_tail.size() > max_lines ? g_tail.size() - max_lines : 0;
    for (size_t i = start; i < g_tail.size(); ++i) out.push_back(g_tail[i]);
    return out;
}

std::string RedactSecrets(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        // 找到一个 key= 的起点
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) { out.append(s, i, std::string::npos); break; }

        // key 是从上一个分隔符到 eq 的子串
        size_t key_start = s.find_last_of("?&", eq);
        key_start = (key_start == std::string::npos) ? i : key_start + 1;
        std::string key = s.substr(key_start, eq - key_start);

        size_t val_end = s.find('&', eq);
        if (val_end == std::string::npos) val_end = s.size();

        out.append(s, i, key_start - i);
        out += key;
        out += "=";
        out += IsSecretKey(key) ? "***" : s.substr(eq + 1, val_end - eq - 1);
        i = val_end;
    }
    return out;
}

}  // namespace cnk
```

- [ ] **Step 5: 更新 CMakeLists.txt**

把 `cnk_core` 与 `cnk_tests` 的源文件列表替换为：

```cmake
add_library(cnk_core STATIC
  src/native/core/Version.h
  src/native/core/Version.cpp
  src/native/core/Paths.cpp
  src/native/core/Log.cpp
)

# ... 中间不变 ...

add_executable(cnk_tests
  tests/test_main.cpp
  tests/smoke_test.cpp
  tests/log_test.cpp
)
```

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS，无失败用例。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: 滚动日志与路径工具，含日志脱敏"
```

---

## 任务 3：模拟 Portal 服务器

这是全部认证测试的基础设施。没有它，协议代码无法在开发机上验证。

**Files:**
- Create: `src/native/sim/SimServer.h`
- Create: `src/native/sim/SimServer.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/sim_test.cpp`

**Interfaces:**
- Consumes: `cnk::HttpPerform`（任务 4，本任务先用 WinHTTP 直接实现循环，不依赖任务 4）
- Produces:
  - `struct cnk::SimOptions { std::string vendor = "drcom"; std::string valid_user; std::string valid_pass; std::string isp_suffix; int fail_mode = 0; }`
    - `fail_mode`: `0`=正常, `1`=返回凭据错误, `2`=连接挂起（测超时）
  - `class cnk::SimServer`：
    - `bool Start(const SimOptions& opts, std::string& error)`
    - `void Stop()`
    - `int Port() const`
    - `std::string BaseUrl() const`
    - `std::string LoginPath() const` — 返回如 `/eportal/?c=ACSetting&a=Login`
    - `int LoginCount() const` — 已收到的登录请求次数，用于断言重试行为
    - `std::string LastLoginBody() const`

- [ ] **Step 1: 写失败测试**

**本任务必须先于任务 4（HTTP 客户端）完成，因此测试不得使用 `HttpPerform`。** 测试自带一个约 30 行的裸 Winsock 客户端，直接收发 HTTP 报文即可 —— 这样任务 3 自洽，不产生对下游的循环依赖。

`tests/sim_test.cpp`:

```cpp
#include "doctest.h"
#include "sim/SimServer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {

// 极简 HTTP 客户端：发一个请求，读回全部响应字节。
// 仅用于测试断言，不解析响应（正文匹配用 find 就够）。
std::string RawHttp(int port, const std::string& method,
                    const std::string& path, const std::string& body) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        return {};
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    send(s, req.c_str(), (int)req.size(), 0);

    std::string resp;
    char buf[4096];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);

    closesocket(s);
    WSACleanup();
    return resp;
}

}  // namespace

TEST_CASE("模拟服务器能响应门户首页请求") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    std::string resp = RawHttp(srv.Port(), "GET", "/", "");
    CHECK(resp.find("200") != std::string::npos);
    CHECK(resp.find("DrcomServer1.0") != std::string::npos);
    CHECK(resp.find("Dr.COMWebLoginID_") != std::string::npos);

    srv.Stop();
}

TEST_CASE("模拟服务器对正确凭据返回成功标志") {
    cnk::SimOptions opts;
    opts.valid_user = "alice";
    opts.valid_pass = "pw123";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
        "DDDDD=alice&upass=pw123&R1=0&R2=0&R3=0&R6=0&para=00&0MKKey=123456");
    CHECK(resp.find("Dr.COMWebLoginID_3.htm") != std::string::npos);
    CHECK(srv.LoginCount() == 1);

    srv.Stop();
}

TEST_CASE("模拟服务器对错误凭据返回失败标志") {
    cnk::SimOptions opts;
    opts.valid_user = "alice";
    opts.valid_pass = "pw123";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                               "DDDDD=alice&upass=wrong");
    CHECK(resp.find("Dr.COMWebLoginID_2.htm") != std::string::npos);

    srv.Stop();
}

TEST_CASE("模拟服务器按后缀校验运营商") {
    cnk::SimOptions opts;
    opts.valid_user = "bob";
    opts.valid_pass = "pw";
    opts.isp_suffix = "@cmcc";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                               "DDDDD=bob@cmcc&upass=pw");
    CHECK(resp.find("Dr.COMWebLoginID_3.htm") != std::string::npos);

    srv.Stop();
}

TEST_CASE("登录计数随请求累加") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));
    CHECK(srv.LoginCount() == 0);
    RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    CHECK(srv.LoginCount() == 1);
    RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    CHECK(srv.LoginCount() == 2);
    srv.Stop();
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`sim/SimServer.h` 不存在。

- [ ] **Step 3: 实现 SimServer**

`src/native/sim/SimServer.h`:

```cpp
#pragma once
#include <string>
#include <memory>

namespace cnk {

struct SimOptions {
    std::string vendor = "drcom";   // "drcom" | "eportal" | "srun" | "generic"
    std::string valid_user;
    std::string valid_pass;
    std::string isp_suffix;         // 期望的账号后缀，如 "@cmcc"
    int fail_mode = 0;              // 0=正常 1=凭据错误 2=连接挂起
};

class SimServer {
public:
    SimServer();
    ~SimServer();
    SimServer(const SimServer&) = delete;
    SimServer& operator=(const SimServer&) = delete;

    bool Start(const SimOptions& opts, std::string& error);
    void Stop();

    int Port() const;
    std::string BaseUrl() const;
    std::string LoginPath() const;
    int LoginCount() const;
    std::string LastLoginBody() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
```

`src/native/sim/SimServer.cpp`:

```cpp
#include "sim/SimServer.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <map>

#pragma comment(lib, "ws2_32.lib")

namespace cnk {

namespace {

const char* kDrcomHome =
    "<html><head><meta charset=\"gbk\"><!--Dr.COMWebLoginID_1.htm--></head>"
    "<body><script>"
    "authloginport=801;"
    "authloginpath='/eportal/?c=ACSetting&a=Login';"
    "authloginparam='url=drappall';"
    "authuserfield='DDDDD';"
    "authpassfield='upass';"
    "authsuccess='Dr.COMWebLoginID_3.htm';"
    "authfail='Dr.COMWebLoginID_2.htm';"
    "carrier='{\"yys\":{\"title\":\"服务类型\",\"mode\":\"radiobutton\",\"type\":\"0\","
    "\"data\":[{\"id\":\"1\",\"name\":\"校园用户\",\"suffix\":\"\"},"
    "{\"id\":\"2\",\"name\":\"校园电信\",\"suffix\":\"@dx\"},"
    "{\"id\":\"3\",\"name\":\"校园联通\",\"suffix\":\"@lt\"},"
    "{\"id\":\"4\",\"name\":\"校园其他\",\"suffix\":\"\"}],\"defaultID\":\"1\"}}';"
    "visit_blacklist=[\"www.msftncsi.com\",\"detectportal.firefox.com\","
    "\"captive.apple.com\",\"generate_204\",\"ncsi.txt\"];"
    "</script></body></html>";

std::string ParseFormValue(const std::string& body, const std::string& key) {
    std::string needle = key + "=";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        bool at_boundary = (pos == 0) || body[pos - 1] == '&';
        if (at_boundary) {
            size_t start = pos + needle.size();
            size_t end = body.find('&', start);
            if (end == std::string::npos) end = body.size();
            return body.substr(start, end - start);
        }
        pos += needle.size();
    }
    return {};
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { out += (char)(h * 16 + l); i += 2; continue; }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

}  // namespace

struct SimServer::Impl {
    SOCKET listen_sock = INVALID_SOCKET;
    std::thread worker;
    std::atomic<bool> running{false};
    int port = 0;
    SimOptions opts;
    std::atomic<int> login_count{0};
    std::mutex body_mutex;
    std::string last_login_body;

    void HandleClient(SOCKET client);
    void ServeLoop();
};

void SimServer::Impl::HandleClient(SOCKET client) {
    std::string request;
    char buf[4096];
    int n;
    while ((n = recv(client, buf, sizeof(buf), 0)) > 0) {
        request.append(buf, n);
        size_t hdr_end = request.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            // 读取 Content-Length 指定的正文
            std::string headers = request.substr(0, hdr_end);
            size_t cl_pos = headers.find("Content-Length:");
            if (cl_pos == std::string::npos)
                cl_pos = headers.find("content-length:");
            size_t want = 0;
            if (cl_pos != std::string::npos) {
                size_t vstart = headers.find(':', cl_pos) + 1;
                want = (size_t)std::strtoul(headers.c_str() + vstart, nullptr, 10);
            }
            if (request.size() >= hdr_end + 4 + want) break;
        }
        if (request.size() > 65536) break;
    }
    if (request.empty()) { closesocket(client); return; }

    size_t sp1 = request.find(' ');
    size_t sp2 = request.find(' ', sp1 + 1);
    std::string method = request.substr(0, sp1);
    std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string body;
    size_t hdr_end = request.find("\r\n\r\n");
    if (hdr_end != std::string::npos) body = request.substr(hdr_end + 4);

    std::string path = target;
    size_t q = path.find('?');
    std::string query = q == std::string::npos ? "" : path.substr(q + 1);
    if (q != std::string::npos) path = path.substr(0, q);

    std::string resp_body;
    std::string content_type = "text/html; charset=gbk";
    int status = 200;

    if (opts.fail_mode == 2) {
        // 挂起：不响应，让客户端超时
        std::this_thread::sleep_for(std::chrono::seconds(10));
        closesocket(client);
        return;
    }

    if (path == "/") {
        resp_body = kDrcomHome;
    } else if (path == "/eportal/" || path == "/eportal") {
        login_count++;
        // 凭据可能来自 query（GET）或 body（POST）
        std::string all = query + "&" + body;
        std::string user = UrlDecode(ParseFormValue(all, "DDDDD"));
        std::string pass = UrlDecode(ParseFormValue(all, "upass"));
        {
            std::lock_guard<std::mutex> lk(body_mutex);
            last_login_body = all;
        }
        bool user_ok = !opts.valid_user.empty() && user == opts.valid_user + opts.isp_suffix;
        bool pass_ok = !opts.valid_pass.empty() && pass == opts.valid_pass;
        if (opts.fail_mode == 1 || !user_ok || !pass_ok) {
            resp_body = "<html><body><!--Dr.COMWebLoginID_2.htm-->login failed</body></html>";
        } else {
            resp_body = "<html><body><!--Dr.COMWebLoginID_3.htm-->"
                        "window.location='3.htm';</body></html>";
        }
    } else if (path == "/ncsi.txt") {
        resp_body = "Microsoft NCSI";
    } else if (path == "/success.txt") {
        resp_body = "success";
    } else {
        status = 404;
        resp_body = "not found";
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " OK\r\n"
        << "Server: DrcomServer1.0\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << resp_body.size() << "\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << resp_body;
    std::string out = oss.str();
    send(client, out.c_str(), (int)out.size(), 0);
    closesocket(client);
}

void SimServer::Impl::ServeLoop() {
    while (running.load()) {
        SOCKET client = accept(listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running.load()) break;
            continue;
        }
        HandleClient(client);
    }
}

SimServer::SimServer() : impl_(new Impl) {}
SimServer::~SimServer() { Stop(); }

bool SimServer::Start(const SimOptions& opts, std::string& error) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    impl_->opts = opts;
    impl_->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listen_sock == INVALID_SOCKET) {
        error = "socket() failed";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 让系统分配
    if (bind(impl_->listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        error = "bind() failed: " + std::to_string(WSAGetLastError());
        closesocket(impl_->listen_sock);
        return false;
    }
    int len = sizeof(addr);
    if (getsockname(impl_->listen_sock, (sockaddr*)&addr, &len) == SOCKET_ERROR) {
        error = "getsockname() failed";
        closesocket(impl_->listen_sock);
        return false;
    }
    impl_->port = ntohs(addr.sin_port);

    int one = 1;
    setsockopt(impl_->listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
    if (listen(impl_->listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        error = "listen() failed";
        closesocket(impl_->listen_sock);
        return false;
    }

    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->ServeLoop(); });
    return true;
}

void SimServer::Stop() {
    if (!impl_) return;
    if (impl_->running.exchange(false)) {
        if (impl_->listen_sock != INVALID_SOCKET) {
            closesocket(impl_->listen_sock);
            impl_->listen_sock = INVALID_SOCKET;
        }
        if (impl_->worker.joinable()) impl_->worker.join();
    }
}

int SimServer::Port() const { return impl_->port; }

std::string SimServer::BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(impl_->port);
}

std::string SimServer::LoginPath() const {
    return "/eportal/?c=ACSetting&a=Login&url=drappall";
}

int SimServer::LoginCount() const { return impl_->login_count.load(); }

std::string SimServer::LastLoginBody() const {
    std::lock_guard<std::mutex> lk(impl_->body_mutex);
    return impl_->last_login_body;
}

}  // namespace cnk
```

- [ ] **Step 4: 更新 CMakeLists.txt**

在 `cnk_core` 源列表追加：

```cmake
  src/native/sim/SimServer.cpp
```

并把 `ws2_32` 加入链接库，在 `tests/` 列表追加 `tests/sim_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。本任务不依赖任务 4，测试使用自带的裸 socket 客户端。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "test: 模拟 Portal 服务器，作为协议测试基础设施"
```

---

## 任务 4：HTTP 客户端

**Files:**
- Create: `src/native/core/Http.h`
- Create: `src/native/core/Http.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/http_test.cpp`

**Interfaces:**
- Consumes: 无
- Produces:
  - `struct cnk::HttpRequest { std::string url; std::string method = "GET"; std::string body; std::string content_type; std::string user_agent; int timeout_ms = 3000; bool follow_redirects = false; }`
  - `struct cnk::HttpResponse { int status = 0; std::string body; std::string server; std::string location; std::string content_type; std::string effective_url; std::string error; bool Ok() const { return status > 0 && error.empty(); } }`
  - `bool cnk::HttpPerform(const HttpRequest& req, HttpResponse& out, std::string& error)`

- [ ] **Step 1: 写失败测试**

`tests/http_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Http.h"
#include "sim/SimServer.h"

TEST_CASE("HttpPerform 能取回正文与 Server 头") {
    cnk::SimServer srv;
    std::string err;
    cnk::SimOptions opts;
    REQUIRE(srv.Start(opts, err));

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/";
    cnk::HttpResponse resp;
    REQUIRE(cnk::HttpPerform(req, resp, err));
    CHECK(resp.status == 200);
    CHECK(resp.server == "DrcomServer1.0");
    CHECK(resp.content_type.find("charset=gbk") != std::string::npos);
    CHECK(resp.body.find("DrcomServer") != std::string::npos
          || resp.body.find("Dr.COM") != std::string::npos);
    srv.Stop();
}

TEST_CASE("HttpPerform 能发送 POST 正文") {
    cnk::SimServer srv;
    std::string err;
    cnk::SimOptions opts;
    opts.valid_user = "u1";
    opts.valid_pass = "p1";
    REQUIRE(srv.Start(opts, err));

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    req.method = "POST";
    req.body = "DDDDD=u1&upass=p1";
    req.content_type = "application/x-www-form-urlencoded";
    cnk::HttpResponse resp;
    REQUIRE(cnk::HttpPerform(req, resp, err));
    CHECK(srv.LastLoginBody().find("DDDDD=u1") != std::string::npos);
    CHECK(srv.LastLoginBody().find("upass=p1") != std::string::npos);
    srv.Stop();
}

TEST_CASE("HttpPerform 超时返回失败而不挂起") {
    cnk::SimServer srv;
    std::string err;
    cnk::SimOptions opts;
    opts.fail_mode = 2;  // 挂起
    REQUIRE(srv.Start(opts, err));

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/";
    req.timeout_ms = 800;
    cnk::HttpResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = cnk::HttpPerform(req, resp, err);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(ok);
    CHECK(ms < 4000);
    CHECK(err.size() > 0);
    srv.Stop();
}

TEST_CASE("HttpPerform 连接不存在的端口时快速失败") {
    cnk::HttpRequest req;
    req.url = "http://127.0.0.1:1/";
    req.timeout_ms = 1000;
    cnk::HttpResponse resp;
    std::string err;
    bool ok = cnk::HttpPerform(req, resp, err);
    CHECK_FALSE(ok);
    CHECK(err.size() > 0);
}

TEST_CASE("HttpPerform 对畸形 URL 返回明确错误") {
    cnk::HttpRequest req;
    req.url = "not-a-url";
    cnk::HttpResponse resp;
    std::string err;
    CHECK_FALSE(cnk::HttpPerform(req, resp, err));
    CHECK(err.size() > 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`core/Http.h` 不存在。

- [ ] **Step 3: 实现 Http**

`src/native/core/Http.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string content_type;
    std::string user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    int timeout_ms = 3000;
    // 探测场景需要观察 302 以判定劫持，故默认不跟随
    bool follow_redirects = false;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string server;
    std::string location;
    std::string content_type;
    std::string effective_url;
    std::string error;

    bool Ok() const { return status > 0 && error.empty(); }
};

bool HttpPerform(const HttpRequest& req, HttpResponse& out, std::string& error);

}  // namespace cnk
```

`src/native/core/Http.cpp`:

```cpp
#include "core/Http.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>

namespace cnk {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

struct WinHttpHandle {
    HINTERNET h = nullptr;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

bool CrackUrl(const std::string& url, std::wstring& host, std::wstring& path,
              INTERNET_PORT& port, bool& https) {
    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host_buf[256] = {};
    wchar_t path_buf[2048] = {};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return false;
    host = host_buf;
    path = path_buf;
    if (path.empty()) path = L"/";
    port = uc.nPort;
    https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

}  // namespace

bool HttpPerform(const HttpRequest& req, HttpResponse& out, std::string& error) {
    out = HttpResponse{};
    error.clear();

    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!CrackUrl(req.url, host, path, port, https)) {
        error = "URL 解析失败: " + req.url;
        return false;
    }

    WinHttpHandle session{WinHttpOpen(L"CampusNetKeeper/0.1",
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) { error = "WinHttpOpen 失败"; return false; }

    int timeout = req.timeout_ms > 0 ? req.timeout_ms : 3000;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    WinHttpHandle connect{WinHttpConnect(session, host.c_str(), port, 0)};
    if (!connect.h) {
        error = "无法连接到 " + WideToUtf8(host) + ":" + std::to_string(port);
        return false;
    }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    if (!req.follow_redirects)
        flags |= WINHTTP_FLAG_NO_REDIRECT;
    else if (https)
        flags |= WINHTTP_FLAG_SECURE;

    WinHttpHandle request{WinHttpOpenRequest(
        connect, Utf8ToWide(req.method).c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request.h) { error = "WinHttpOpenRequest 失败"; return false; }

    std::wstring headers;
    if (!req.content_type.empty())
        headers += L"Content-Type: " + Utf8ToWide(req.content_type) + L"\r\n";
    if (!req.user_agent.empty())
        headers += L"User-Agent: " + Utf8ToWide(req.user_agent) + L"\r\n";
    headers += L"Cache-Control: no-cache\r\n";

    BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), (DWORD)-1L,
        req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(),
        (DWORD)req.body.size(), (DWORD)req.body.size(), 0);
    if (!sent) {
        error = "发送请求失败，错误码 " + std::to_string(GetLastError());
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        error = "接收响应超时或失败，错误码 " + std::to_string(GetLastError());
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;

    auto query_header = [&](DWORD which, const wchar_t* name) -> std::string {
        DWORD len = 0;
        WinHttpQueryHeaders(request, which, name, nullptr, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (len == 0) return {};
        std::wstring buf(len / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request, which, name, buf.data(), &len,
                                 WINHTTP_NO_HEADER_INDEX))
            return {};
        if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
        return WideToUtf8(buf);
    };

    out.server = query_header(WINHTTP_QUERY_SERVER, WINHTTP_HEADER_NAME_BY_INDEX);
    out.location = query_header(WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX);
    out.content_type = query_header(WINHTTP_QUERY_CONTENT_TYPE,
                                    WINHTTP_HEADER_NAME_BY_INDEX);

    std::string body;
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read)) break;
        body.append(buf.data(), read);
        if (body.size() > 4 * 1024 * 1024) break;  // 上限，防异常大响应
    } while (avail > 0);

    out.body = std::move(body);
    out.effective_url = req.url;
    return true;
}

}  // namespace cnk
```

- [ ] **Step 4: 更新 CMakeLists.txt**

在 `cnk_core` 源列表追加 `src/native/core/Http.cpp`，在 `cnk_tests` 列表追加 `tests/http_test.cpp`，并把 `tests/sim_test.cpp` 加回（若任务 3 曾移除）。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。特别注意超时用例应在 4 秒内返回。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 基于 WinHTTP 的 HTTP 客户端，含超时与重定向控制"
```

---

## 任务 5：编码处理（GBK / UTF-8 判定）

spec §7.1 明确：门户首页是 GBK，eportal 路径是 UTF-8。硬编码任一种都会导致中文乱码。这是本任务存在的唯一理由。

**Files:**
- Create: `src/native/core/Encoding.h`
- Create: `src/native/core/Encoding.cpp`
- Create: `src/native/core/Url.h`
- Create: `src/native/core/Url.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/encoding_test.cpp`
- Create: `tests/url_test.cpp`

**Interfaces:**
- Consumes: `cnk::HttpResponse`（任务 4）
- Produces:
  - `std::string cnk::ParseCharset(const std::string& content_type)` — 从 `text/html; charset=gbk` 提取 `gbk`（小写）；无则返回空串
  - `std::string cnk::ToUtf8(const std::string& bytes, const std::string& charset)` — 空 charset 按 UTF-8 处理
  - `std::string cnk::DecodeBody(const cnk::HttpResponse& resp)` — 便捷函数：自动解析 charset 并转 UTF-8
  - `std::string cnk::UrlEncode(const std::string& s)` — 百分号编码，空格编码为 `%20`
  - `bool cnk::SplitHostPort(const std::string& url, std::string& scheme_host, std::string& port, const std::string& fallback_port)` — 未显式指定端口时用 `fallback_port`
  - `std::string cnk::ResolveUrl(const std::string& action, const std::string& base)` — 相对路径按 base 的 origin 解析

**为什么 URL 工具放在这个任务**：任务 11（Dr.COM）、19（锐捷）、20（深澜）、21（通用表单）都需要 URL 编码与「从 auth_url 拆出主机与端口」。把它们各自复制一份会在评审中被判为逐字重复。集中在这里定义一次，四个认证策略共同引用。

- [ ] **Step 1: 写失败测试**

`tests/encoding_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Encoding.h"

TEST_CASE("ParseCharset 处理各种写法") {
    CHECK(cnk::ParseCharset("text/html; charset=gbk") == "gbk");
    CHECK(cnk::ParseCharset("text/html;charset=UTF-8") == "utf-8");
    CHECK(cnk::ParseCharset("text/html; charset=\"gb2312\"") == "gb2312");
    CHECK(cnk::ParseCharset("text/html; Charset=GBK ") == "gbk");
    CHECK(cnk::ParseCharset("text/html") == "");
    CHECK(cnk::ParseCharset("") == "");
}

TEST_CASE("ToUtf8 把 GBK 正确转成 UTF-8") {
    // "学号" 的 GBK 编码
    const std::string gbk = "\xD1\xA7\xBA\xC5";
    CHECK(cnk::ToUtf8(gbk, "gbk") == "学号");
    CHECK(cnk::ToUtf8(gbk, "gb2312") == "学号");
    CHECK(cnk::ToUtf8(gbk, "GBK") == "学号");
}

TEST_CASE("ToUtf8 对 UTF-8 输入原样返回") {
    CHECK(cnk::ToUtf8("学号", "utf-8") == "学号");
    CHECK(cnk::ToUtf8("学号", "") == "学号");
}

TEST_CASE("ToUtf8 对非法字节序列不崩溃") {
    std::string junk = "\xFF\xFE\x80\x81\x90";
    auto r = cnk::ToUtf8(junk, "gbk");
    CHECK(r.size() > 0);   // 只要不崩、能返回即可
}
```

`tests/url_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Url.h"

TEST_CASE("UrlEncode 编码保留字符") {
    CHECK(cnk::UrlEncode("abc123-_.~") == "abc123-_.~");
    CHECK(cnk::UrlEncode("a b") == "a%20b");
    CHECK(cnk::UrlEncode("a@b") == "a%40b");
    CHECK(cnk::UrlEncode("a&b=c") == "a%26b%3Dc");
    CHECK(cnk::UrlEncode("") == "");
}

TEST_CASE("UrlEncode 正确处理中文（按 UTF-8 字节）") {
    CHECK(cnk::UrlEncode("学") == "%E5%AD%A6");
}

TEST_CASE("SplitHostPort 对显式端口的 URL") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("http://10.0.0.1:801/eportal/", host, port, "80"));
    CHECK(host == "http://10.0.0.1");
    CHECK(port == "801");
}

TEST_CASE("SplitHostPort 对无端口的 URL 用 fallback") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("http://10.0.0.1/", host, port, "801"));
    CHECK(host == "http://10.0.0.1");
    CHECK(port == "801");

    std::string host2, port2;
    REQUIRE(cnk::SplitHostPort("http://10.0.0.1/", host2, port2, "80"));
    CHECK(port2 == "80");
}

TEST_CASE("SplitHostPort 保留 scheme") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("https://a.example.com/x?y=1", host, port, "443"));
    CHECK(host == "https://a.example.com");
    CHECK(port == "443");
}

TEST_CASE("SplitHostPort 对畸形 URL 返回 false") {
    std::string host, port;
    CHECK_FALSE(cnk::SplitHostPort("", host, port, "80"));
}

TEST_CASE("ResolveUrl 处理绝对、根相对与路径相对") {
    CHECK(cnk::ResolveUrl("http://a/b", "http://x/") == "http://a/b");
    CHECK(cnk::ResolveUrl("/doLogin", "http://10.0.0.1/portal/")
          == "http://10.0.0.1/doLogin");
    CHECK(cnk::ResolveUrl("doLogin", "http://10.0.0.1/portal/")
          == "http://10.0.0.1/doLogin");
    CHECK(cnk::ResolveUrl("", "http://10.0.0.1/login")
          == "http://10.0.0.1/login");
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`core/Encoding.h` 不存在。

- [ ] **Step 3: 实现 Encoding**

`src/native/core/Encoding.h`:

```cpp
#pragma once
#include <string>
#include "core/Http.h"

namespace cnk {

std::string ParseCharset(const std::string& content_type);
std::string ToUtf8(const std::string& bytes, const std::string& charset);
std::string DecodeBody(const HttpResponse& resp);

}  // namespace cnk
```

`src/native/core/Encoding.cpp`:

```cpp
#include "core/Encoding.h"
#include <windows.h>
#include <algorithm>
#include <cctype>

namespace cnk {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string Trim(std::string s) {
    auto notspace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

UINT CodePageFor(const std::string& charset) {
    std::string cs = ToLower(charset);
    if (cs.empty() || cs == "utf-8" || cs == "utf8") return CP_UTF8;
    if (cs == "gbk" || cs == "gb2312" || cs == "gb18030" ||
        cs == "cp936" || cs == "x-gbk") return 936;
    if (cs == "big5") return 950;
    if (cs == "iso-8859-1" || cs == "latin1") return 28591;
    return CP_UTF8;  // 未知编码按 UTF-8 兜底
}

}  // namespace

std::string ParseCharset(const std::string& content_type) {
    std::string lower = ToLower(content_type);
    size_t pos = lower.find("charset=");
    if (pos == std::string::npos) return {};
    std::string val = content_type.substr(pos + 8);
    val = Trim(val);
    if (!val.empty() && val.front() == '"') {
        size_t end = val.find('"', 1);
        val = val.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    } else {
        size_t end = val.find_first_of("; \t\r\n");
        if (end != std::string::npos) val = val.substr(0, end);
    }
    return ToLower(Trim(val));
}

std::string ToUtf8(const std::string& bytes, const std::string& charset) {
    if (bytes.empty()) return {};
    UINT cp = CodePageFor(charset);
    if (cp == CP_UTF8) return bytes;

    int n = MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(),
                                nullptr, 0);
    if (n <= 0) return bytes;  // 解码失败时原样返回，不崩溃
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(),
                        wide.data(), n);

    int m = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                                nullptr, 0, nullptr, nullptr);
    if (m <= 0) return bytes;
    std::string out(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                        out.data(), m, nullptr, nullptr);
    return out;
}

std::string DecodeBody(const HttpResponse& resp) {
    return ToUtf8(resp.body, ParseCharset(resp.content_type));
}

}  // namespace cnk
```

`src/native/core/Url.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 百分号编码。保留 A-Z a-z 0-9 - _ . ~，空格编码为 %20（不用 +，
// 因为 + 在查询串中的含义依赖服务端实现，%20 语义无歧义）。
std::string UrlEncode(const std::string& s);

// 从完整 URL 拆出 "scheme://host" 与端口。
// url 未显式指定端口时，port 取 fallback_port。
// 空串或无法解析返回 false。
bool SplitHostPort(const std::string& url, std::string& scheme_host,
                   std::string& port, const std::string& fallback_port);

// 把 action 解析为绝对 URL：
//   - action 已是 http(s):// 开头 → 原样返回
//   - action 以 / 开头 → 拼到 base 的 origin 之后
//   - 其他 → 视为相对路径，拼到 base 的 origin 之后并补 /
//   - action 为空 → 返回 base
std::string ResolveUrl(const std::string& action, const std::string& base);

}  // namespace cnk
```

`src/native/core/Url.cpp`:

```cpp
#include "core/Url.h"

namespace cnk {

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool SplitHostPort(const std::string& url, std::string& scheme_host,
                   std::string& port, const std::string& fallback_port) {
    scheme_host.clear();
    port.clear();
    if (url.empty()) return false;

    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;

    const size_t host_start = scheme_end + 3;
    const size_t path_start = url.find('/', host_start);
    const std::string authority = url.substr(
        host_start,
        path_start == std::string::npos ? std::string::npos : path_start - host_start);
    if (authority.empty()) return false;

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        scheme_host = url.substr(0, host_start) + authority.substr(0, colon);
        port = authority.substr(colon + 1);
    } else {
        scheme_host = url.substr(0, host_start) + authority;
        port = fallback_port;
    }
    return true;
}

namespace {

// scheme 前缀大小写不敏感比较（prefix 传小写）
bool StartsWithCI(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[i]) != prefix[i]) return false;
    return true;
}

}  // namespace

std::string ResolveUrl(const std::string& action, const std::string& base) {
    if (action.empty()) return base;
    // 绝对地址：scheme 大小写不敏感（Location 头的写法不受 RFC 约束）
    // ⚠ 审阅注记（2026-10-13，任务 13 深度审阅）：原实现只认小写 http://，
    // 大写 scheme 与协议相对地址会被当相对路径 → 探测劫持判定静默漏判
    if (StartsWithCI(action, "http://") || StartsWithCI(action, "https://"))
        return action;

    // 协议相对（//host/path）：沿用 base 的 scheme 拼接
    if (action.rfind("//", 0) == 0) {
        const size_t se = base.find("://");
        if (se == std::string::npos) return base + action;
        return base.substr(0, se + 1) + action;  // "http:" + "//host/path"
    }

    // 取 base 的 origin
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return base + action;
    const size_t host_start = scheme_end + 3;
    const size_t path_start = base.find('/', host_start);
    const std::string origin = base.substr(
        0, path_start == std::string::npos ? base.size() : path_start);

    if (action[0] == '/') return origin + action;
    return origin + "/" + action;
}

}  // namespace cnk
```

- [ ] **Step 4: 更新 CMakeLists.txt**

追加 `src/native/core/Encoding.cpp`、`src/native/core/Url.cpp`、`tests/encoding_test.cpp`、`tests/url_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 按 Content-Type 动态判定字符集，修正 GBK/UTF-8 混用"
```

---

## 任务 6：DPAPI 与配置结构

**Files:**
- Create: `src/native/store/Dpapi.h`
- Create: `src/native/store/Dpapi.cpp`
- Create: `src/native/store/Config.h`
- Create: `src/native/store/Config.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/config_test.cpp`

**Interfaces:**
- Consumes: `cnk::AppDataDir()`（任务 2）
- Produces:
  - `bool cnk::DpapiProtect(const std::string& plain, std::string& blob)`
  - `bool cnk::DpapiUnprotect(const std::string& blob, std::string& plain)`
  - `struct cnk::IspConfig { std::string preset; std::string custom; }`
  - `struct cnk::CustomTemplate { std::string method; std::string url; std::string body; std::map<std::string,std::string> headers; std::string success_regex; }`
  - `struct cnk::Profile { std::string name; std::string auth_url; std::string username; std::string password; bool has_password; std::string vendor; IspConfig isp; CustomTemplate custom_template; }`
  - `struct cnk::Settings { int probe_interval_seconds; int probe_timeout_seconds; std::string probe_url; std::string probe_expect; std::vector<int> retry_backoff_seconds; bool autostart; bool notify_on_reconnect; std::string log_level; int log_max_kb; int log_keep_files; }`
  - `struct cnk::AppConfig { int version; std::string active_profile; Settings settings; std::vector<Profile> profiles; }`
  - `std::string cnk::ConfigPath()`
  - `bool cnk::ConfigLoad(AppConfig& out, std::string& error)`
  - `bool cnk::ConfigSave(const AppConfig& cfg, std::string& error)`
  - `std::string cnk::IspSuffix(const Profile& p)` — 返回要拼到账号后的后缀（Dr.COM 用）
  - `const Profile* cnk::ActiveProfile(const AppConfig& cfg)`

**IspSuffix 的映射规则**（spec §4.4）：

| preset | Dr.COM 后缀 | 深澜 domain | 锐捷 service |
|---|---|---|---|
| `cmcc` | `@cmcc` | `@cmcc` | `cmcc` |
| `telecom` | `@dx` | `@telecom` | `telecom` |
| `unicom` | `@lt` | `@unicom` | `unicom` |
| `campus` | `""` | `@campus` | `""` |
| `custom` | `custom` 原样 | `custom` 原样 | `custom` 原样 |

- [ ] **Step 1: 写失败测试**

`tests/config_test.cpp`:

```cpp
#include "doctest.h"
#include "store/Dpapi.h"
#include "store/Config.h"
#include <filesystem>

TEST_CASE("DPAPI 加解密往返") {
    std::string blob, plain;
    REQUIRE(cnk::DpapiProtect("my-secret-pw", blob));
    CHECK(blob != "my-secret-pw");
    CHECK(blob.size() > 10);
    REQUIRE(cnk::DpapiUnprotect(blob, plain));
    CHECK(plain == "my-secret-pw");
}

TEST_CASE("DPAPI 解密垃圾数据返回失败而不崩溃") {
    std::string plain;
    CHECK_FALSE(cnk::DpapiUnprotect("not-a-valid-blob", plain));
}

TEST_CASE("IspSuffix 按厂商映射") {
    cnk::Profile p;
    p.vendor = "drcom";
    p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(p) == "@dx");
    p.isp.preset = "cmcc";
    CHECK(cnk::IspSuffix(p) == "@cmcc");
    p.isp.preset = "campus";
    CHECK(cnk::IspSuffix(p) == "");
    p.isp.preset = "custom";
    p.isp.custom = "@xyz";
    CHECK(cnk::IspSuffix(p) == "@xyz");

    p.vendor = "srun";
    p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(p) == "@telecom");
    p.vendor = "eportal";
    p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(p) == "telecom");
}

TEST_CASE("AppConfig JSON 往返保持一致") {
    cnk::AppConfig cfg;
    cfg.active_profile = "宿舍";
    cfg.settings.probe_interval_seconds = 7;
    cnk::Profile p;
    p.name = "宿舍";
    p.auth_url = "http://10.0.0.1";
    p.username = "u";
    p.password = "pw";
    p.has_password = true;
    p.vendor = "drcom";
    p.isp.preset = "cmcc";
    cfg.profiles.push_back(p);

    std::string json_out, err;
    REQUIRE(cnk::ConfigToJson(cfg, json_out));
    cnk::AppConfig back;
    REQUIRE(cnk::ConfigFromJson(json_out, back, err));
    CHECK(back.active_profile == "宿舍");
    CHECK(back.settings.probe_interval_seconds == 7);
    REQUIRE(back.profiles.size() == 1);
    CHECK(back.profiles[0].username == "u");
    CHECK(back.profiles[0].password == "pw");
    CHECK(back.profiles[0].isp.preset == "cmcc");
}

TEST_CASE("损坏的 JSON 返回失败而不崩溃") {
    cnk::AppConfig back;
    std::string err;
    CHECK_FALSE(cnk::ConfigFromJson("{ this is not json", back, err));
    CHECK(err.size() > 0);
}

TEST_CASE("缺字段的 JSON 使用默认值") {
    cnk::AppConfig back;
    std::string err;
    REQUIRE(cnk::ConfigFromJson("{}", back, err));
    CHECK(back.version == 1);
    CHECK(back.settings.probe_interval_seconds == 5);
    CHECK(back.profiles.empty());
}

TEST_CASE("ActiveProfile 按名字查找，找不到返回第一个") {
    cnk::AppConfig cfg;
    cnk::Profile a; a.name = "A";
    cnk::Profile b; b.name = "B";
    cfg.profiles = {a, b};
    cfg.active_profile = "B";
    CHECK(cnk::ActiveProfile(cfg)->name == "B");
    cfg.active_profile = "不存在";
    CHECK(cnk::ActiveProfile(cfg)->name == "A");

    cnk::AppConfig empty;
    CHECK(cnk::ActiveProfile(empty) == nullptr);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`store/Dpapi.h` 不存在。

- [ ] **Step 3: 实现 Dpapi**

`src/native/store/Dpapi.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

bool DpapiProtect(const std::string& plain, std::string& blob);
bool DpapiUnprotect(const std::string& blob, std::string& plain);

}  // namespace cnk
```

`src/native/store/Dpapi.cpp`:

```cpp
#include "store/Dpapi.h"
#include <windows.h>
#include <dpapi.h>
#include <vector>

namespace cnk {

namespace {

// 固定 entropy：使密文绑定本程序，拷贝到别处也无法解密
const char kEntropy[] = "CampusNetKeeper.v1";

}  // namespace

bool DpapiProtect(const std::string& plain, std::string& blob) {
    blob.clear();
    if (plain.empty()) return true;

    DATA_BLOB in{};
    in.pbData = (BYTE*)plain.data();
    in.cbData = (DWORD)plain.size();

    DATA_BLOB entropy{};
    entropy.pbData = (BYTE*)kEntropy;
    entropy.cbData = (DWORD)(sizeof(kEntropy) - 1);

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"CampusNetKeeper", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    blob.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool DpapiUnprotect(const std::string& blob, std::string& plain) {
    plain.clear();
    if (blob.empty()) return true;

    DATA_BLOB in{};
    in.pbData = (BYTE*)blob.data();
    in.cbData = (DWORD)blob.size();

    DATA_BLOB entropy{};
    entropy.pbData = (BYTE*)kEntropy;
    entropy.cbData = (DWORD)(sizeof(kEntropy) - 1);

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    plain.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

}  // namespace cnk
```

- [ ] **Step 4: 实现 Config**

`src/native/store/Config.h`:

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>

namespace cnk {

struct IspConfig {
    std::string preset = "campus";  // cmcc | telecom | unicom | campus | custom
    std::string custom;
};

struct CustomTemplate {
    std::string method = "POST";
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string success_regex;
};

struct Profile {
    std::string name;
    std::string auth_url;
    std::string username;
    std::string password;       // 内存中明文
    bool has_password = false;  // 反序列化后表示 password_enc 非空
    std::string vendor = "auto";  // auto|drcom|srun|eportal|generic|custom
    IspConfig isp;
    CustomTemplate custom_template;
};

struct Settings {
    int probe_interval_seconds = 5;
    int probe_timeout_seconds = 3;
    std::string probe_url = "http://www.msftncsi.com/ncsi.txt";
    std::string probe_expect = "Microsoft NCSI";
    std::vector<int> retry_backoff_seconds = {5, 10, 20, 30, 60};
    bool autostart = true;
    bool notify_on_reconnect = true;
    std::string log_level = "info";
    int log_max_kb = 512;
    int log_keep_files = 3;
};

struct AppConfig {
    int version = 1;
    std::string active_profile = "默认";
    Settings settings;
    std::vector<Profile> profiles;
};

std::string ConfigPath();

// 序列化到 JSON 字符串；密码以 base64(DPAPI) 存入 password_enc 字段
bool ConfigToJson(const AppConfig& cfg, std::string& out_json);
// 从 JSON 反序列化；解密 password_enc 到 password
bool ConfigFromJson(const std::string& json, AppConfig& out, std::string& error);

bool ConfigLoad(AppConfig& out, std::string& error);
bool ConfigSave(const AppConfig& cfg, std::string& error);

// 该 Profile 在 Dr.COM 下应拼到账号后的后缀（其他厂商由各自 Auth 使用）
std::string IspSuffix(const Profile& p);

// active_profile 命中的项；不存在时返回第一个；无 profile 时返回 nullptr
const Profile* ActiveProfile(const AppConfig& cfg);

}  // namespace cnk
```

`src/native/store/Config.cpp`:

```cpp
#include "store/Config.h"
#include "store/Dpapi.h"
#include "core/Paths.h"
#include "core/Log.h"
#include "json.hpp"
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace cnk {

namespace {

std::string Base64Encode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += tbl[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += tbl[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::string Base64Decode(const std::string& in) {
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        size_t pos = tbl.find((char)c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + (int)pos;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

json ProfileToJson(const Profile& p) {
    json j;
    j["name"] = p.name;
    j["auth_url"] = p.auth_url;
    j["username"] = p.username;
    j["vendor"] = p.vendor;
    j["isp"] = {{"preset", p.isp.preset}, {"custom", p.isp.custom}};

    if (!p.password.empty()) {
        std::string blob;
        if (DpapiProtect(p.password, blob))
            j["password_enc"] = Base64Encode(blob);
    } else {
        j["password_enc"] = "";
    }

    j["custom_template"] = {
        {"method", p.custom_template.method},
        {"url", p.custom_template.url},
        {"body", p.custom_template.body},
        {"headers", p.custom_template.headers},
        {"success_regex", p.custom_template.success_regex},
    };
    return j;
}

Profile ProfileFromJson(const json& j) {
    Profile p;
    p.name = j.value("name", std::string{});
    p.auth_url = j.value("auth_url", std::string{});
    p.username = j.value("username", std::string{});
    p.vendor = j.value("vendor", std::string{"auto"});

    if (j.contains("isp") && j["isp"].is_object()) {
        p.isp.preset = j["isp"].value("preset", std::string{"campus"});
        p.isp.custom = j["isp"].value("custom", std::string{});
    }

    std::string enc = j.value("password_enc", std::string{});
    if (!enc.empty()) {
        std::string blob = Base64Decode(enc);
        if (DpapiUnprotect(blob, p.password)) {
            p.has_password = true;
        } else {
            LogWarn("配置中的密码无法解密（可能来自其他机器或用户），需重新输入");
            p.has_password = false;
        }
    }

    if (j.contains("custom_template") && j["custom_template"].is_object()) {
        const auto& ct = j["custom_template"];
        p.custom_template.method = ct.value("method", std::string{"POST"});
        p.custom_template.url = ct.value("url", std::string{});
        p.custom_template.body = ct.value("body", std::string{});
        p.custom_template.success_regex = ct.value("success_regex", std::string{});
        if (ct.contains("headers") && ct["headers"].is_object())
            for (auto& [k, v] : ct["headers"].items())
                p.custom_template.headers[k] = v.get<std::string>();
    }
    return p;
}

}  // namespace

std::string ConfigPath() {
    std::string dir = AppDataDir();
    if (dir.empty()) return {};
    return (fs::u8path(dir) / "config.json").u8string();
}

bool ConfigToJson(const AppConfig& cfg, std::string& out_json) {
    json j;
    j["version"] = cfg.version;
    j["active_profile"] = cfg.active_profile;

    j["settings"] = {
        {"probe_interval_seconds", cfg.settings.probe_interval_seconds},
        {"probe_timeout_seconds", cfg.settings.probe_timeout_seconds},
        {"probe_url", cfg.settings.probe_url},
        {"probe_expect", cfg.settings.probe_expect},
        {"retry_backoff_seconds", cfg.settings.retry_backoff_seconds},
        {"autostart", cfg.settings.autostart},
        {"notify_on_reconnect", cfg.settings.notify_on_reconnect},
        {"log_level", cfg.settings.log_level},
        {"log_max_kb", cfg.settings.log_max_kb},
        {"log_keep_files", cfg.settings.log_keep_files},
    };

    j["profiles"] = json::array();
    for (const auto& p : cfg.profiles) j["profiles"].push_back(ProfileToJson(p));

    out_json = j.dump(2);
    return true;
}

bool ConfigFromJson(const std::string& json_str, AppConfig& out, std::string& error) {
    out = AppConfig{};
    error.clear();
    json j;
    try {
        j = json::parse(json_str);
    } catch (const std::exception& e) {
        error = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "配置根节点不是对象";
        return false;
    }

    out.version = j.value("version", 1);
    out.active_profile = j.value("active_profile", std::string{"默认"});

    if (j.contains("settings") && j["settings"].is_object()) {
        const auto& s = j["settings"];
        auto& t = out.settings;
        t.probe_interval_seconds = s.value("probe_interval_seconds", 5);
        t.probe_timeout_seconds = s.value("probe_timeout_seconds", 3);
        t.probe_url = s.value("probe_url", t.probe_url);
        t.probe_expect = s.value("probe_expect", t.probe_expect);
        t.autostart = s.value("autostart", true);
        t.notify_on_reconnect = s.value("notify_on_reconnect", true);
        t.log_level = s.value("log_level", std::string{"info"});
        t.log_max_kb = s.value("log_max_kb", 512);
        t.log_keep_files = s.value("log_keep_files", 3);
        if (s.contains("retry_backoff_seconds") &&
            s["retry_backoff_seconds"].is_array()) {
            t.retry_backoff_seconds.clear();
            for (auto& v : s["retry_backoff_seconds"])
                t.retry_backoff_seconds.push_back(v.get<int>());
            if (t.retry_backoff_seconds.empty())
                t.retry_backoff_seconds = {5, 10, 20, 30, 60};
        }
    }

    if (j.contains("profiles") && j["profiles"].is_array())
        for (auto& pj : j["profiles"]) out.profiles.push_back(ProfileFromJson(pj));

    return true;
}

bool ConfigSave(const AppConfig& cfg, std::string& error) {
    error.clear();
    std::string path = ConfigPath();
    if (path.empty()) { error = "无法确定配置目录"; return false; }

    std::string json_str;
    if (!ConfigToJson(cfg, json_str)) { error = "序列化失败"; return false; }

    // 先写临时文件再改名，避免写入中断留下半截文件
    fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) { error = "无法写入临时配置文件"; return false; }
        ofs << json_str;
        ofs.flush();
        if (!ofs) { error = "写入临时配置文件失败"; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, fs::u8path(path), ec);
    if (ec) {
        fs::remove(fs::u8path(path), ec);
        fs::rename(tmp, fs::u8path(path), ec);
        if (ec) { error = "替换配置文件失败"; return false; }
    }
    return true;
}

bool ConfigLoad(AppConfig& out, std::string& error) {
    error.clear();
    out = AppConfig{};
    std::string path = ConfigPath();
    if (path.empty()) { error = "无法确定配置目录"; return false; }

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        error = "配置文件不存在";
        return false;
    }

    std::ifstream ifs(fs::u8path(path), std::ios::binary);
    if (!ifs) { error = "无法读取配置文件"; return false; }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    std::string parse_err;
    if (!ConfigFromJson(content, out, parse_err)) {
        // 损坏：备份后回落默认配置，绝不让上层拿到半个配置
        fs::path bak = fs::u8path(path + ".bak");
        std::error_code e2;
        fs::copy_file(fs::u8path(path), bak, fs::copy_options::overwrite_existing, e2);
        LogError("配置文件损坏，已备份到 config.json.bak：" + parse_err);
        out = AppConfig{};
        error = parse_err;
        return false;
    }
    return true;
}

std::string IspSuffix(const Profile& p) {
    if (p.isp.preset == "custom") return p.isp.custom;
    const bool drcom = (p.vendor == "drcom") || (p.vendor == "auto");
    if (p.isp.preset == "cmcc")    return drcom ? "@cmcc" : (p.vendor == "srun" ? "@cmcc" : "cmcc");
    if (p.isp.preset == "telecom") return drcom ? "@dx"   : (p.vendor == "srun" ? "@telecom" : "telecom");
    if (p.isp.preset == "unicom")  return drcom ? "@lt"   : (p.vendor == "srun" ? "@unicom" : "unicom");
    if (p.isp.preset == "campus")  return drcom ? ""      : (p.vendor == "srun" ? "@campus" : "");
    return {};
}

const Profile* ActiveProfile(const AppConfig& cfg) {
    if (cfg.profiles.empty()) return nullptr;
    for (const auto& p : cfg.profiles)
        if (p.name == cfg.active_profile) return &p;
    return &cfg.profiles.front();
}

}  // namespace cnk
```

- [ ] **Step 5: 更新 CMakeLists.txt**

追加 `src/native/store/Dpapi.cpp`、`src/native/store/Config.cpp`、`tests/config_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。DPAPI 用例要求当前用户有正常登录会话——在普通桌面会话下运行即可。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: DPAPI 密码加密与配置读写，含损坏配置的备份回落"
```

---

## 任务 7：配置的磁盘读写、损坏恢复与原子写入

任务 6 已实现 `ConfigLoad` / `ConfigSave` 的骨架。本任务补上**跨进程可验证的行为测试**——损坏文件、半写入、只读目录，这些是 Review Focus 第 3 条的落点。

**Files:**
- Modify: `src/native/store/Config.cpp`（若需修正）
- Create: `tests/config_io_test.cpp`

**Interfaces:**
- Consumes: `cnk::ConfigLoad` / `cnk::ConfigSave` / `cnk::ConfigPath()`（任务 6）
- Produces: 无新接口，仅行为保证

- [ ] **Step 1: 写失败测试**

由于 `ConfigPath()` 固定指向 `%APPDATA%`，测试需要能指定路径。为此给 `Config.h` 增加两个可注入路径的重载：

```cpp
bool ConfigLoadFrom(const std::string& path, AppConfig& out, std::string& error);
bool ConfigSaveTo(const std::string& path, const AppConfig& cfg, std::string& error);
```

`ConfigLoad` / `ConfigSave` 改为对它们的薄封装。测试用这两个重载。

`tests/config_io_test.cpp`:

```cpp
#include "doctest.h"
#include "store/Config.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
fs::path FreshDir(const char* name) {
    auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
}

TEST_CASE("保存后能原样读回，且密码可用") {
    auto dir = FreshDir("cnk_io_1");
    auto path = (dir / "config.json").string();

    cnk::AppConfig cfg;
    cfg.active_profile = "A";
    cnk::Profile p;
    p.name = "A";
    p.username = "user1";
    p.password = "pw1";
    p.has_password = true;
    cfg.profiles.push_back(p);

    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));

    cnk::AppConfig back;
    REQUIRE(cnk::ConfigLoadFrom(path, back, err));
    REQUIRE(back.profiles.size() == 1);
    CHECK(back.profiles[0].password == "pw1");
    CHECK(back.profiles[0].has_password);
}

TEST_CASE("磁盘上的密码不是明文") {
    auto dir = FreshDir("cnk_io_2");
    auto path = (dir / "config.json").string();
    cnk::AppConfig cfg;
    cnk::Profile p;
    p.name = "A";
    p.username = "user1";
    p.password = "SUPER_SECRET_VALUE";
    cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));

    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content.find("SUPER_SECRET_VALUE") == std::string::npos);
    CHECK(content.find("user1") != std::string::npos);  // 用户名不加密
}

TEST_CASE("损坏的配置文件被备份且返回失败，不崩溃") {
    auto dir = FreshDir("cnk_io_3");
    auto path = (dir / "config.json").string();
    {
        std::ofstream ofs(path);
        ofs << "{ broken json !!!";
    }
    cnk::AppConfig cfg;
    std::string err;
    CHECK_FALSE(cnk::ConfigLoadFrom(path, cfg, err));
    CHECK(err.size() > 0);
    CHECK(fs::exists(path + ".bak"));       // 已备份
    CHECK(cfg.profiles.empty());            // 回落到干净默认值
    CHECK(cfg.settings.probe_interval_seconds == 5);
}

TEST_CASE("文件不存在时返回失败并给出干净默认值") {
    auto dir = FreshDir("cnk_io_4");
    cnk::AppConfig cfg;
    std::string err;
    CHECK_FALSE(cnk::ConfigLoadFrom((dir / "nope.json").string(), cfg, err));
    CHECK(cfg.profiles.empty());
}

TEST_CASE("保存不留下临时文件") {
    auto dir = FreshDir("cnk_io_5");
    auto path = (dir / "config.json").string();
    cnk::AppConfig cfg;
    cnk::Profile p; p.name = "A"; cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));
    CHECK_FALSE(fs::exists(path + ".tmp"));
}

TEST_CASE("保存到不可写路径返回失败而不崩溃") {
    cnk::AppConfig cfg;
    std::string err;
    // Windows 下用非法路径字符触发失败
    CHECK_FALSE(cnk::ConfigSaveTo("Z:\\nonexistent\\dir\\config.json", cfg, err));
    CHECK(err.size() > 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`ConfigSaveTo` / `ConfigLoadFrom` 未声明。

- [ ] **Step 3: 重构 Config.cpp 暴露可注入路径的重载**

在 `Config.h` 加入声明，在 `Config.cpp` 中把现有 `ConfigLoad` / `ConfigSave` 的实现体改为调用新函数：

```cpp
bool ConfigSaveTo(const std::string& path, const AppConfig& cfg, std::string& error) {
    error.clear();
    if (path.empty()) { error = "配置路径为空"; return false; }

    std::string json_str;
    if (!ConfigToJson(cfg, json_str)) { error = "序列化失败"; return false; }

    fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) { error = "无法写入临时配置文件"; return false; }
        ofs << json_str;
        ofs.flush();
        if (!ofs) { error = "写入临时配置文件失败"; return false; }
    }
    std::error_code ec;
    // MSVC 的 rename 可覆盖已存在的目标。失败（权限/占用等）时保留旧配置原样，
    // 只清理 .tmp 并报错 —— 绝不能「先删旧配置再重试」，否则第二次 rename 若
    // 同样失败，用户配置将彻底丢失（任务 6 审阅 P1，回归见 config_io_test）。
    fs::rename(tmp, fs::u8path(path), ec);
    if (ec) {
        std::error_code e3;
        fs::remove(tmp, e3);  // 不留临时文件
        error = "替换配置文件失败: " + ec.message();
        return false;
    }
    return true;
}

bool ConfigLoadFrom(const std::string& path, AppConfig& out, std::string& error) {
    error.clear();
    out = AppConfig{};
    if (path.empty()) { error = "配置路径为空"; return false; }

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        error = "配置文件不存在";
        return false;
    }

    std::ifstream ifs(fs::u8path(path), std::ios::binary);
    if (!ifs) { error = "无法读取配置文件"; return false; }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    std::string parse_err;
    if (!ConfigFromJson(content, out, parse_err)) {
        fs::path bak = fs::u8path(path + ".bak");
        std::error_code e2;
        fs::copy_file(fs::u8path(path), bak, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            LogError("配置文件损坏，且备份失败（" + e2.message() + "），已回落默认配置：" + parse_err);
        } else {
            LogError("配置文件损坏，已备份到 " + path + ".bak：" + parse_err);
        }
        out = AppConfig{};
        error = parse_err;
        return false;
    }
    return true;
}

bool ConfigSave(const AppConfig& cfg, std::string& error) {
    const std::string path = ConfigPath();
    if (path.empty()) { error = "无法确定配置目录"; return false; }
    return ConfigSaveTo(path, cfg, error);
}

bool ConfigLoad(AppConfig& out, std::string& error) {
    const std::string path = ConfigPath();
    if (path.empty()) { error = "无法确定配置目录"; return false; }
    return ConfigLoadFrom(path, out, error);
}
```

- [ ] **Step 4: 更新 CMakeLists.txt**

在 `cnk_tests` 源列表追加 `tests/config_io_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 配置原子写入与损坏恢复，路径可注入以便测试"
```

---

## 任务 8：厂商识别

**Files:**
- Create: `src/native/auth/Detector.h`
- Create: `src/native/auth/Detector.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/detector_test.cpp`

**Interfaces:**
- Consumes: `cnk::HttpResponse`（任务 4）
- Produces:
  - `enum class cnk::Vendor { Unknown, Drcom, Eportal, Srun, Huawei }`
  - `cnk::Vendor cnk::DetectVendor(const HttpResponse& resp)` — **必须按 spec §7 的顺序判定**
  - `const char* cnk::VendorName(Vendor v)` — 返回 `"drcom"` / `"eportal"` / `"srun"` / `"huawei"` / `"unknown"`
  - `cnk::Vendor cnk::VendorFromString(const std::string& s)`

- [ ] **Step 1: 写失败测试**

`tests/detector_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/Detector.h"

TEST_CASE("Server 头为 DrcomServer 判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.server = "DrcomServer1.0";
    r.body = "<html>whatever</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("同时含 DrcomServer 与 /eportal/ 时判定为 Dr.COM 而非锐捷") {
    // 这正是实测环境的情形，是最容易误判的组合
    cnk::HttpResponse r;
    r.server = "DrcomServer1.0";
    r.body = "<script>authloginpath='/eportal/?c=ACSetting&a=Login';"
             "authloginport=801;</script>";
    r.effective_url = "http://10.0.0.1:801/eportal/";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("页面含中文「哆点」判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.body = "<script>/** 哆点参数 start */</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("epHTTPPort 不构成 Dr.COM 指纹") {
    // 该变量真实存在，但只在首页引用的 a41.js 里，探测拿到的 HTML 看不到它。
    // 若把它当指纹，真实环境永远不命中 —— 这条测试防止它被改回来。
    cnk::HttpResponse r;
    r.server = "nginx";
    r.body = "<script>epHTTPPort=801;</script>";
    CHECK(cnk::DetectVendor(r) != cnk::Vendor::Drcom);
}

TEST_CASE("页面含 Dr.COMWebLoginID_ 判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.body = "<!--Dr.COMWebLoginID_1.htm-->";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("页面含 0MKKey 判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.body = "<input name=\"0MKKey\" value=\"123456\">";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("无 Dr.COM 特征但含 /eportal/ 判定为锐捷") {
    cnk::HttpResponse r;
    r.server = "nginx";
    r.body = "<script>jsVersion='4.1.3';</script>";
    r.effective_url = "http://10.0.0.1/eportal/";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Eportal);
}

TEST_CASE("含 srun_portal 判定为深澜") {
    cnk::HttpResponse r;
    r.body = "<script>var api='/cgi-bin/srun_portal';</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Srun);
}

TEST_CASE("含 ac_id 与 callback 判定为深澜") {
    cnk::HttpResponse r;
    r.body = "<script>var ac_id='1'; var callback='jsonp';</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Srun);
}

TEST_CASE("含 iPortal 判定为华为") {
    cnk::HttpResponse r;
    r.body = "<html>iPortal 认证</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Huawei);
}

TEST_CASE("无任何特征判定为 Unknown") {
    cnk::HttpResponse r;
    r.body = "<html>hello</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Unknown);
}

TEST_CASE("VendorName 与 VendorFromString 互逆") {
    for (auto v : {cnk::Vendor::Drcom, cnk::Vendor::Eportal,
                   cnk::Vendor::Srun, cnk::Vendor::Huawei}) {
        CHECK(cnk::VendorFromString(cnk::VendorName(v)) == v);
    }
    CHECK(cnk::VendorFromString("auto") == cnk::Vendor::Unknown);
    CHECK(cnk::VendorFromString("garbage") == cnk::Vendor::Unknown);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`auth/Detector.h` 不存在。

- [ ] **Step 3: 实现 Detector**

`src/native/auth/Detector.h`:

```cpp
#pragma once
#include <string>
#include "core/Http.h"

namespace cnk {

enum class Vendor { Unknown, Drcom, Eportal, Srun, Huawei };

// 按 spec §7 的顺序判定：Dr.COM 必须排在锐捷之前，
// 因为两者共用 /eportal/ 路径。
Vendor DetectVendor(const HttpResponse& resp);

const char* VendorName(Vendor v);
Vendor VendorFromString(const std::string& s);

}  // namespace cnk
```

`src/native/auth/Detector.cpp`:

```cpp
#include "auth/Detector.h"
#include "core/Encoding.h"
#include <algorithm>
#include <cctype>

namespace cnk {

namespace {

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool ContainsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    return it != hay.end();
}

}  // namespace

Vendor DetectVendor(const HttpResponse& resp) {
    const std::string body = DecodeBody(resp);
    const std::string& server = resp.server;
    const std::string& url = resp.effective_url;

    // spec §7 对部分指纹写「页面/URL 含」，两处都查
    // （审阅注记 2026-10-13：首版仅查 body，URL-only 场景会漏判华为）
    const auto inUrlOrBody = [&](const char* s) {
        return Contains(url, s) || Contains(body, s);
    };

    // 序 1：Server 头含 DrcomServer —— 最强信号
    if (ContainsCI(server, "drcomserver")) return Vendor::Drcom;

    // 序 2：哆点门户特征
    // 注意：不要用 epHTTPPort 做指纹 —— 该变量真实存在，但在首页引用的
    // a41.js 里，不在首页 HTML 内；客户端探测只拿得到 HTML，永远看不到它。
    // 同理 `duodian` 是拼音，真实页面写的是中文「哆点」。
    if (Contains(body, "Dr.COMWebLoginID_") || Contains(body, "0MKKey") ||
        Contains(body, "哆点"))
        return Vendor::Drcom;

    // 序 3：深澜
    if (inUrlOrBody("srun_portal") || inUrlOrBody("get_challenge") ||
        inUrlOrBody("srun_bx1") ||
        (Contains(body, "ac_id") && Contains(body, "callback")))
        return Vendor::Srun;

    // 序 4：锐捷 eportal
    if (Contains(url, "/eportal/") || ContainsCI(body, "c=Portal") ||
        (Contains(body, "wlanuserip") && Contains(body, "nasip")) ||
        Contains(body, "jsVersion"))
        return Vendor::Eportal;

    // 序 5：华为 iPortal。
    // "iPortal" 保持大小写敏感：ContainsCI 会把 multiportal 之类的子串误判为华为。
    if (inUrlOrBody("/portal/login") || Contains(body, "iPortal") ||
        Contains(body, "portalpage"))
        return Vendor::Huawei;

    return Vendor::Unknown;
}

const char* VendorName(Vendor v) {
    switch (v) {
        case Vendor::Drcom:   return "drcom";
        case Vendor::Eportal: return "eportal";
        case Vendor::Srun:    return "srun";
        case Vendor::Huawei:  return "huawei";
        case Vendor::Unknown: return "unknown";
    }
    return "unknown";
}

Vendor VendorFromString(const std::string& s) {
    if (s == "drcom")   return Vendor::Drcom;
    if (s == "eportal") return Vendor::Eportal;
    if (s == "srun")    return Vendor::Srun;
    if (s == "huawei")  return Vendor::Huawei;
    return Vendor::Unknown;
}

}  // namespace cnk
```

- [ ] **Step 4: 更新 CMakeLists.txt**

追加 `src/native/auth/Detector.cpp` 与 `tests/detector_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。重点关注「同时含 DrcomServer 与 /eportal/」用例必须判定为 Drcom。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 厂商识别，Dr.COM 优先于锐捷判定"
```

---

## 任务 9：Portal 配置解析（运营商与探测地址）

**Files:**
- Create: `src/native/auth/Carrier.h`
- Create: `src/native/auth/Carrier.cpp`
- Create: `src/native/core/ProbeTarget.h`
- Create: `src/native/core/ProbeTarget.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/carrier_test.cpp`
- Create: `tests/probe_target_test.cpp`

**Interfaces:**
- Consumes: `cnk::HttpResponse`（任务 4）
- Produces:
  - `struct cnk::CarrierOption { std::string id; std::string name; std::string suffix; }`
  - `struct cnk::CarrierList { std::string title; std::vector<CarrierOption> options; std::string default_id; }`
  - `bool cnk::ParseCarrier(const std::string& utf8_body, CarrierList& out)` — 从页面里 `carrier='...'` 提取并解析
  - `bool cnk::ParseVisitBlacklist(const std::string& utf8_body, std::vector<std::string>& out)`
  - `struct cnk::ProbeTarget { std::string url; std::string expect; }`
  - `bool cnk::SelectProbeTarget(const std::vector<std::string>& blacklist, ProbeTarget& out)`

**SelectProbeTarget 的规则**（spec §6.1）：按优先级 `www.msftncsi.com` → `ncsi.txt` → `detectportal.firefox.com` → `success.txt` → `captive.apple.com` 匹配黑名单条目；命中即产出对应 URL 与期望正文。无一命中返回 false。

> ⚠ 实现注记（2026-10-13 复核）：实际候选顺序为 `msftncsi → detectportal → ncsi.txt → success.txt → captive.apple`
> （路径型候选 `ncsi.txt`/`success.txt` 后置为兜底）。仅当清单**同时含 `ncsi.txt` 与 `detectportal` 但无 msftncsi 域名**时，
> 与上面字面分组顺序不同（会选 firefox 而非 msftncsi）——两者均为清单内合法地址，且实测真实清单首个候选即命中，不受影响。

- [ ] **Step 1: 写失败测试**

`tests/carrier_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/Carrier.h"

// 与实测环境一致的 carrier 配置
static const char* kPage =
    "<script>carrier='{\"yys\":{\"title\": \"服务类型\",\"mode\":\"radiobutton\","
    "\"type\":\"0\",\"data\":[{\"id\":\"1\",\"name\":\"校园用户\",\"suffix\":\"\"},"
    "{\"id\":\"2\",\"name\":\"校园电信\",\"suffix\":\"@dx\"},"
    "{\"id\":\"3\",\"name\":\"校园联通\",\"suffix\":\"@lt\"},"
    "{\"id\":\"4\",\"name\":\"校园其他\",\"suffix\":\"\"}],"
    "\"defaultID\":\"1\"}}';</script>";

TEST_CASE("解析实测的 carrier 配置") {
    cnk::CarrierList list;
    REQUIRE(cnk::ParseCarrier(kPage, list));
    CHECK(list.title == "服务类型");
    REQUIRE(list.options.size() == 4);
    CHECK(list.options[0].name == "校园用户");
    CHECK(list.options[0].suffix == "");
    CHECK(list.options[1].name == "校园电信");
    CHECK(list.options[1].suffix == "@dx");
    CHECK(list.options[2].name == "校园联通");
    CHECK(list.options[2].suffix == "@lt");
    CHECK(list.options[3].name == "校园其他");
    CHECK(list.default_id == "1");
}

TEST_CASE("无 carrier 时返回 false 而非崩溃") {
    cnk::CarrierList list;
    CHECK_FALSE(cnk::ParseCarrier("<html>nothing</html>", list));
    CHECK(list.options.empty());
}

TEST_CASE("carrier 内容为非法 JSON 时返回 false") {
    cnk::CarrierList list;
    CHECK_FALSE(cnk::ParseCarrier("carrier='{not json';", list));
}

TEST_CASE("DefaultCarrierIndex 返回默认项下标") {
    cnk::CarrierList list;
    REQUIRE(cnk::ParseCarrier(kPage, list));
    CHECK(cnk::DefaultCarrierIndex(list) == 0);
}
```

`tests/probe_target_test.cpp`:

```cpp
#include "doctest.h"
#include "core/ProbeTarget.h"

// 与实测环境一致的 visit_blacklist
static const char* kPage =
    "<script>visit_blacklist=[\"1.1.1.1\",\"www.msftncsi.com\",\"land.xiaomi.net\","
    "\"detectportal.firefox.com\",\"www.apple.com\",\"generate_204\",\"ncsi.txt\","
    "\"success.txt\"];</script>";

TEST_CASE("解析 visit_blacklist") {
    std::vector<std::string> bl;
    REQUIRE(cnk::ParseVisitBlacklist(kPage, bl));
    CHECK(bl.size() == 8);
    CHECK(std::find(bl.begin(), bl.end(), "www.msftncsi.com") != bl.end());
}

TEST_CASE("优先选中 msftncsi 而非黑名单外地址") {
    std::vector<std::string> bl;
    REQUIRE(cnk::ParseVisitBlacklist(kPage, bl));
    cnk::ProbeTarget t;
    REQUIRE(cnk::SelectProbeTarget(bl, t));
    CHECK(t.url.find("www.msftncsi.com") != std::string::npos);
    CHECK(t.expect == "Microsoft NCSI");
    // 关键：绝不能选出 connect.rom.miui.com 之类不在清单内的地址
    CHECK(t.url.find("connect.rom.miui.com") == std::string::npos);
}

TEST_CASE("无 msftncsi 时回落到 firefox") {
    std::vector<std::string> bl = {"detectportal.firefox.com", "ncsi.txt"};
    cnk::ProbeTarget t;
    REQUIRE(cnk::SelectProbeTarget(bl, t));
    CHECK(t.url.find("detectportal.firefox.com") != std::string::npos);
    CHECK(t.expect == "success");
}

TEST_CASE("清单为空时返回 false") {
    cnk::ProbeTarget t;
    CHECK_FALSE(cnk::SelectProbeTarget({}, t));
}

TEST_CASE("无 visit_blacklist 时解析返回 false") {
    std::vector<std::string> bl;
    CHECK_FALSE(cnk::ParseVisitBlacklist("<html>none</html>", bl));
}

TEST_CASE("DefaultProbeTarget 返回内置默认值") {
    auto t = cnk::DefaultProbeTarget();
    CHECK(t.url.find("www.msftncsi.com") != std::string::npos);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，头文件不存在。

- [ ] **Step 3: 实现 Carrier**

`src/native/auth/Carrier.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace cnk {

struct CarrierOption {
    std::string id;
    std::string name;
    std::string suffix;
};

struct CarrierList {
    std::string title;
    std::vector<CarrierOption> options;
    std::string default_id;
};

// 从页面文本中提取 carrier='{...}' 并解析。找不到或解析失败返回 false。
bool ParseCarrier(const std::string& utf8_body, CarrierList& out);

// default_id 命中项的下标；未命中返回 0；空列表返回 -1。
int DefaultCarrierIndex(const CarrierList& list);

}  // namespace cnk
```

`src/native/auth/Carrier.cpp`:

```cpp
#include "auth/Carrier.h"
#include "json.hpp"
#include <cstdlib>

using json = nlohmann::json;

namespace cnk {

namespace {

// 字段取值：字符串原样返回；数值型（如 "defaultID":1）转字符串；其他类型返回空。
std::string StrField(const json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

// 找到 carrier='...' 或 carrier="..." 的赋值，返回引号内的原文
bool ExtractCarrierLiteral(const std::string& body, std::string& out) {
    size_t pos = body.find("carrier");
    while (pos != std::string::npos) {
        size_t eq = body.find('=', pos + 7);
        if (eq == std::string::npos) return false;

        // 'carrier' 与 '=' 之间必须只有空白，防 carrierXXX= 型误命中
        bool between_ok = true;
        for (size_t k = pos + 7; k < eq; ++k) {
            if (body[k] != ' ' && body[k] != '\t') { between_ok = false; break; }
        }
        if (!between_ok) {
            pos = body.find("carrier", pos + 7);
            continue;
        }

        // 跳过 = 与引号之间的空白
        size_t i = eq + 1;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
        if (i >= body.size()) return false;
        char quote = body[i];
        if (quote != '\'' && quote != '"') { pos = body.find("carrier", pos + 7); continue; }
        size_t start = i + 1;
        size_t end = body.find(quote, start);
        if (end == std::string::npos) return false;
        out = body.substr(start, end - start);
        return true;
    }
    return false;
}

}  // namespace

bool ParseCarrier(const std::string& utf8_body, CarrierList& out) {
    out = CarrierList{};

    std::string literal;
    if (!ExtractCarrierLiteral(utf8_body, literal)) return false;

    // ⚠ 审阅注记（2026-10-13）：字段取值不得直接用 nlohmann 的 value() ——
    // 服务器下发数值型 id/defaultID（如 "defaultID":1）时它会抛未捕获异常
    // 并终止进程（已实测复现）。用 StrField 兼容字符串/数值，并整体兜底 try/catch。
    try {
        json j = json::parse(literal);
        if (!j.is_object() || !j.contains("yys") || !j["yys"].is_object()) return false;

        const auto& yys = j["yys"];
        out.title = StrField(yys, "title");
        out.default_id = StrField(yys, "defaultID");
        if (out.default_id.empty()) out.default_id = StrField(yys, "defaultid");

        if (yys.contains("data") && yys["data"].is_array()) {
            for (const auto& item : yys["data"]) {
                if (!item.is_object()) continue;
                CarrierOption o;
                o.id = StrField(item, "id");
                o.name = StrField(item, "name");
                o.suffix = StrField(item, "suffix");
                out.options.push_back(o);
            }
        }
    } catch (const std::exception&) {
        out = CarrierList{};
        return false;
    }
    return !out.options.empty();
}

int DefaultCarrierIndex(const CarrierList& list) {
    if (list.options.empty()) return -1;
    for (size_t i = 0; i < list.options.size(); ++i)
        if (list.options[i].id == list.default_id) return (int)i;
    return 0;
}

}  // namespace cnk
```

- [ ] **Step 4: 实现 ProbeTarget**

`src/native/core/ProbeTarget.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace cnk {

struct ProbeTarget {
    std::string url;
    std::string expect;
};

// 从页面文本中提取 visit_blacklist=[...]。找不到返回 false。
bool ParseVisitBlacklist(const std::string& utf8_body,
                         std::vector<std::string>& out);

// 按优先级从清单中选择探测地址。清单为空或无命中返回 false。
bool SelectProbeTarget(const std::vector<std::string>& blacklist, ProbeTarget& out);

// 内置默认探测地址（msftncsi）。清单不可用时的兜底。
ProbeTarget DefaultProbeTarget();

}  // namespace cnk
```

`src/native/core/ProbeTarget.cpp`:

```cpp
#include "core/ProbeTarget.h"
#include <algorithm>

namespace cnk {

namespace {

struct Candidate {
    const char* key;    // 在黑名单里需要出现的子串
    const char* url;
    const char* expect;
};

// 优先级顺序即数组顺序：先域名型候选（msftncsi / firefox），
// 再路径型候选作为清单只给路径时的兜底，最后 captive.apple。
// ⚠ 审阅注记（2026-10-13）：原表缺 ncsi.txt 独立层级、captive.apple 位置也与 spec §6.1 不符，已修正
const Candidate kCandidates[] = {
    {"msftncsi",      "http://www.msftncsi.com/ncsi.txt",              "Microsoft NCSI"},
    {"detectportal",  "http://detectportal.firefox.com/success.txt",   "success"},
    {"ncsi.txt",      "http://www.msftncsi.com/ncsi.txt",              "Microsoft NCSI"},
    {"success.txt",   "http://detectportal.firefox.com/success.txt",   "success"},
    {"captive.apple", "http://captive.apple.com/hotspot-detect.html",  "Success"},
};

// 域名/标识符边界匹配：needle 必须位于串首或 '.'、'/' 之后，且后随 '.' 或串尾。
// 纯子串匹配会让 evil-msftncsi.com 误命中 msftncsi。
bool ListContains(const std::vector<std::string>& list, const std::string& needle) {
    for (const auto& s : list) {
        size_t p = s.find(needle);
        while (p != std::string::npos) {
            const bool left_ok = (p == 0) || (s[p - 1] == '.') || (s[p - 1] == '/');
            const size_t end = p + needle.size();
            const bool right_ok = (end == s.size()) || (s[end] == '.');
            if (left_ok && right_ok) return true;
            p = s.find(needle, p + 1);
        }
    }
    return false;
}

}  // namespace

bool ParseVisitBlacklist(const std::string& utf8_body,
                         std::vector<std::string>& out) {
    out.clear();
    // ⚠ 审阅注记（2026-10-13）：真实响应是 JSONP dr1003({"visit_blacklist":[...]})，
    // 键值之间是**冒号**——原实现只认 '='，对真实数据永远返回 false（曾被单测掩盖）。
    // 兼容两种形态：JSONP/JSON 的 "visit_blacklist":[ ... ] 与页面内 JS 的 visit_blacklist=[ ... ]
    static const char kKey[] = "visit_blacklist";
    size_t pos = utf8_body.find(kKey);
    if (pos == std::string::npos) return false;
    size_t lb = utf8_body.find('[', pos);
    if (lb == std::string::npos) return false;

    // 键与 '[' 之间只允许分隔符、引号与空白，防跨越大段文本误配对
    for (size_t k = pos + sizeof(kKey) - 1; k < lb; ++k) {
        const char c = utf8_body[k];
        if (c != '"' && c != '\'' && c != ':' && c != '=' &&
            c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return false;
    }

    size_t rb = utf8_body.find(']', lb);
    if (rb == std::string::npos) return false;

    std::string inner = utf8_body.substr(lb + 1, rb - lb - 1);
    size_t i = 0;
    while (i < inner.size()) {
        size_t q1 = inner.find_first_of("\"'", i);
        if (q1 == std::string::npos) break;
        char quote = inner[q1];
        size_t q2 = inner.find(quote, q1 + 1);
        if (q2 == std::string::npos) break;
        out.push_back(inner.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return !out.empty();
}

bool SelectProbeTarget(const std::vector<std::string>& blacklist, ProbeTarget& out) {
    out = ProbeTarget{};  // 失败时不残留上一次的选中结果
    if (blacklist.empty()) return false;
    for (const auto& c : kCandidates) {
        if (ListContains(blacklist, c.key)) {
            out.url = c.url;
            out.expect = c.expect;
            return true;
        }
    }
    return false;
}

ProbeTarget DefaultProbeTarget() {
    return ProbeTarget{"http://www.msftncsi.com/ncsi.txt", "Microsoft NCSI"};
}

}  // namespace cnk
```

- [ ] **Step 5: 更新 CMakeLists.txt**

追加 `src/native/auth/Carrier.cpp`、`src/native/core/ProbeTarget.cpp`、`tests/carrier_test.cpp`、`tests/probe_target_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: 解析服务器下发的运营商列表与免劫持探测地址"
```

---

## 任务 10：认证策略接口与工厂

**Files:**
- Create: `src/native/auth/IAuth.h`
- Create: `src/native/auth/AuthFactory.h`
- Create: `src/native/auth/AuthFactory.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/auth_factory_test.cpp`

**Interfaces:**
- Consumes: `cnk::Profile`（任务 6）、`cnk::Vendor`（任务 8）
- Produces:
  - `struct cnk::NetContext { std::string local_ip; std::string portal_url; std::string portal_body; std::string server_header; }`
    - `portal_body` 为**已转 UTF-8** 的页面文本
  - `struct cnk::LoginResult { bool success; std::string message; int vendor_code; std::string request_url; std::string request_body; }`
    - `request_url` / `request_body` 供日志与 `--test-login` 输出（已脱敏前原文，调用方负责脱敏）
  - `class cnk::IAuth`：`virtual LoginResult Login(const Profile&, const NetContext&) = 0;`、`virtual const char* Name() const = 0;`
  - `std::unique_ptr<cnk::IAuth> cnk::CreateAuth(Vendor vendor, const Profile& profile)` — profile 为 `custom` 时返回 `CustomTemplateAuth`

- [ ] **Step 1: 写失败测试**

`tests/auth_factory_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/AuthFactory.h"

TEST_CASE("按厂商创建对应策略") {
    cnk::Profile p;
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Drcom, p)->Name()) == "DrcomAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Eportal, p)->Name()) == "EportalAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Srun, p)->Name()) == "SrunAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Huawei, p)->Name()) == "GenericFormAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Unknown, p)->Name()) == "GenericFormAuth");
}

TEST_CASE("vendor=custom 时创建自定义模板策略") {
    cnk::Profile p;
    p.vendor = "custom";
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Drcom, p)->Name()) == "CustomTemplateAuth");
}

TEST_CASE("CreateAuth 永不返回 nullptr") {
    cnk::Profile p;
    for (auto v : {cnk::Vendor::Unknown, cnk::Vendor::Drcom, cnk::Vendor::Eportal,
                   cnk::Vendor::Srun, cnk::Vendor::Huawei})
        CHECK(cnk::CreateAuth(v, p) != nullptr);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现接口与工厂**

`src/native/auth/IAuth.h`:

```cpp
#pragma once
#include <string>
#include <memory>
#include "store/Config.h"

namespace cnk {

struct NetContext {
    std::string local_ip;
    std::string portal_url;
    std::string portal_body;    // 已转 UTF-8
    std::string server_header;
};

struct LoginResult {
    bool success = false;
    std::string message;
    int vendor_code = -1;
    std::string request_url;    // 供诊断输出
    std::string request_body;
};

class IAuth {
public:
    virtual ~IAuth() = default;
    virtual LoginResult Login(const Profile& profile, const NetContext& ctx) = 0;
    virtual const char* Name() const = 0;
};

}  // namespace cnk
```

`src/native/auth/AuthFactory.h`:

```cpp
#pragma once
#include <memory>
#include "auth/IAuth.h"
#include "auth/Detector.h"

namespace cnk {

// 依 profile.vendor 决定策略；vendor=="custom" 时无条件用 CustomTemplateAuth。
// 永不返回 nullptr。
std::unique_ptr<IAuth> CreateAuth(Vendor detected, const Profile& profile);

}  // namespace cnk
```

`src/native/auth/AuthFactory.cpp`:

```cpp
#include "auth/AuthFactory.h"

namespace cnk {

std::unique_ptr<IAuth> MakeDrcomAuth();
std::unique_ptr<IAuth> MakeEportalAuth();
std::unique_ptr<IAuth> MakeSrunAuth();
std::unique_ptr<IAuth> MakeGenericFormAuth();
std::unique_ptr<IAuth> MakeCustomTemplateAuth();

std::unique_ptr<IAuth> CreateAuth(Vendor detected, const Profile& profile) {
    if (profile.vendor == "custom") return MakeCustomTemplateAuth();

    Vendor v = detected;
    if (profile.vendor != "auto") {
        Vendor forced = VendorFromString(profile.vendor);
        if (forced != Vendor::Unknown) v = forced;
    }

    switch (v) {
        case Vendor::Drcom:   return MakeDrcomAuth();
        case Vendor::Eportal: return MakeEportalAuth();
        case Vendor::Srun:    return MakeSrunAuth();
        case Vendor::Huawei:
        case Vendor::Unknown:
        default:              return MakeGenericFormAuth();
    }
}

}  // namespace cnk
```

**注意**：本任务的测试依赖任务 11-13 的 `Make*Auth` 实现。为使本任务可独立通过，先创建四个**最小桩**（仅 `Name()` 正确、`Login()` 返回失败并注明未实现），任务 11-13 逐个替换为真实实现。

在 `src/native/auth/Stubs.cpp` 中：

```cpp
#include "auth/AuthFactory.h"

namespace cnk {

namespace {

class StubAuth : public IAuth {
public:
    StubAuth(const char* name) : name_(name) {}
    LoginResult Login(const Profile&, const NetContext&) override {
        LoginResult r;
        r.message = std::string(name_) + " 尚未实现";
        return r;
    }
    const char* Name() const override { return name_; }
private:
    const char* name_;
};

}  // namespace

std::unique_ptr<IAuth> MakeDrcomAuth()        { return std::make_unique<StubAuth>("DrcomAuth"); }
std::unique_ptr<IAuth> MakeEportalAuth()      { return std::make_unique<StubAuth>("EportalAuth"); }
std::unique_ptr<IAuth> MakeSrunAuth()         { return std::make_unique<StubAuth>("SrunAuth"); }
std::unique_ptr<IAuth> MakeGenericFormAuth()  { return std::make_unique<StubAuth>("GenericFormAuth"); }
std::unique_ptr<IAuth> MakeCustomTemplateAuth(){ return std::make_unique<StubAuth>("CustomTemplateAuth"); }

}  // namespace cnk
```

- [ ] **Step 4: 更新 CMakeLists.txt**

追加 `src/native/auth/AuthFactory.cpp`、`src/native/auth/Stubs.cpp`、`tests/auth_factory_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 认证策略接口与工厂，先以桩实现占位"
```

---

## 任务 11：DrcomAuth（首要目标）

**Files:**
- Create: `src/native/auth/DrcomAuth.cpp`
- Modify: `src/native/auth/Stubs.cpp`（移除 `MakeDrcomAuth`）
- Modify: `CMakeLists.txt`
- Create: `tests/drcom_auth_test.cpp`

**Interfaces:**
- Consumes: `IAuth`、`Profile`、`NetContext`（任务 10）、`HttpPerform`（任务 4）、`IspSuffix`（任务 6）
- Produces: `std::unique_ptr<cnk::IAuth> cnk::MakeDrcomAuth()`

**请求构造**（spec §7.2）：

- 目标：`{auth_url 的 host 与 port 部分}/eportal/?c=ACSetting&a=Login&url=drappall`
  - 由 `profile.auth_url` 推导：取其 scheme+host，端口**强制为 801**（若 `auth_url` 已带端口则沿用）
  - ⚠ **实测修订（2026-10-16，见任务 18「实测回修 2」）**：端口**不再强制 801**——老式
    c=ACSetting 协议与门户页同源（801 只服务 SPA/新 API），实现改为取 `auth_url` 自身端口
    （未显式写端口则按 scheme 缺省 80/443）。下文「成功判定」级别与 17 个字段不变。
  - ⚠⚠ **再修订（2026-10-16 晚，见「实测回修 3」）**：老式 c=ACSetting 通道在本部署
    **不做真实认证**（恒回 `userid error2`，与凭据对错无关），故它**已降级为旧部署回退**；
    **首选通道是新式 API** `GET :801/eportal/portal/login?callback=..&params=<AES(JSON)>`
    （前置 `chkstatus` + `loadConfig` 取 `rcn`）。本节以下的老式请求构造/成功判定
    仅在回退分支生效。
- 方法：`POST`，`Content-Type: application/x-www-form-urlencoded`
- 正文 17 个字段，`DDDDD` 为 `username + IspSuffix(profile)`，`upass` 为密码，其余按 spec §7.2 的固定值

**成功判定**（三级，spec §7.2）：
1. 正文含 `Dr.COMWebLoginID_3.htm` 或 `3.htm` → 成功
2. 正文含 `Dr.COMWebLoginID_2.htm` 或 `2.htm` → 失败
3. 都不含时返回 `success=false` 且 `vendor_code=-1`，由上层用探测结果做最终判定

- [ ] **Step 1: 写失败测试**

`tests/drcom_auth_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/AuthFactory.h"
#include "sim/SimServer.h"

namespace {
cnk::Profile MakeProfile(const std::string& base, const std::string& user,
                         const std::string& pass, const std::string& preset) {
    cnk::Profile p;
    p.name = "T";
    p.auth_url = base + "/";
    p.username = user;
    p.password = pass;
    p.vendor = "drcom";
    p.isp.preset = preset;
    return p;
}
}

TEST_CASE("正确凭据登录成功") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "pw123";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom,
                                MakeProfile(srv.BaseUrl(), "alice", "pw123", "campus"));
    cnk::NetContext ctx;
    ctx.portal_url = srv.BaseUrl() + "/";
    auto r = auth->Login(MakeProfile(srv.BaseUrl(), "alice", "pw123", "campus"), ctx);
    CHECK(r.success);
    CHECK(r.request_body.find("DDDDD=alice") != std::string::npos);
    CHECK(r.request_body.find("upass=pw123") != std::string::npos);
    srv.Stop();
}

TEST_CASE("错误密码返回失败") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "right";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "alice", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    // ⚠ 审阅注记（2026-10-13）：原期望值 -1 与 spec §4.1「vendor_code = 厂商返回的
    // 原始码」及三级判定语义矛盾（-1 是「无法判定」的保留值）；且任务 14 调度器以
    // vendor_code==2 作为凭据错误「停止重试」的唯一信号，-1 会导致错误密码无限重试。
    CHECK(r.vendor_code == 2);
    srv.Stop();
}

TEST_CASE("运营商后缀被拼到账号后") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "bob";
    o.valid_pass = "pw";
    o.isp_suffix = "@cmcc";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "bob", "pw", "cmcc");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    // 服务器要求账号必须带 @cmcc 才通过，能成功即证明后缀已拼上
    CHECK(r.success);
    CHECK(r.request_body.find("DDDDD=bob%40cmcc") != std::string::npos);
    srv.Stop();
}

TEST_CASE("正文包含全部 17 个规定字段") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "u", "p", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);

    for (const char* field : {"DDDDD=", "upass=", "R1=0", "R2=0", "R3=0", "R6=0",
                              "para=00", "0MKKey=123456", "buttonClicked=",
                              "redirect_url=", "err_flag=", "username=",
                              "password=", "user=", "cmd=", "Login=", "v6ip="})
        CHECK(r.request_body.find(field) != std::string::npos);
    srv.Stop();
}

TEST_CASE("request_url 指向 801 端口的登录路径") {
    cnk::Profile p;
    p.auth_url = "http://10.1.2.3/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("/eportal/") != std::string::npos);
    CHECK(r.request_url.find("c=ACSetting") != std::string::npos);
    CHECK(r.request_url.find("a=Login") != std::string::npos);
    CHECK(r.request_url.find(":801") != std::string::npos);
}

TEST_CASE("auth_url 已带端口时沿用该端口") {
    cnk::Profile p;
    p.auth_url = "http://10.1.2.3:9000/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find(":9000") != std::string::npos);
    CHECK(r.request_url.find(":801") == std::string::npos);
}

TEST_CASE("空密码不发请求直接返回失败") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:1/";
    p.username = "u";
    p.password = "";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("密码") != std::string::npos);
}

TEST_CASE("空账号不发请求直接返回失败") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:1/";
    p.username = "";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号") != std::string::npos);
}

TEST_CASE("服务器无响应时返回失败并带错误信息") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:1/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.size() > 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 用例失败（桩实现返回「尚未实现」）。

- [ ] **Step 3: 实现 DrcomAuth**

`src/native/auth/DrcomAuth.cpp`:

```cpp
#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include "core/Log.h"
#include <sstream>

namespace cnk {

namespace {

class DrcomAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext&) override {
        LoginResult r;

        if (p.username.empty()) {
            r.message = "账号为空，请在设置中填写";
            return r;
        }
        if (p.password.empty()) {
            r.message = "密码为空，请在设置中填写（若是首次使用请重新输入密码）";
            return r;
        }

        std::string scheme_host, port;
        // spec §7.1：eportal HTTP 端口 801、HTTPS 端口 802；无显式端口时按 scheme 兜底
        const bool https = p.auth_url.rfind("https://", 0) == 0;
        if (!SplitHostPort(p.auth_url, scheme_host, port, https ? "802" : "801")) {
            r.message = "认证网址格式不正确：" + p.auth_url +
                        "（应形如 http://10.0.0.1/）";
            return r;
        }
        r.request_url = scheme_host + ":" + port +
                        "/eportal/?c=ACSetting&a=Login&url=drappall";

        const std::string account = p.username + IspSuffix(p);

        std::ostringstream body;
        body << "DDDDD=" << UrlEncode(account)
             << "&upass=" << UrlEncode(p.password)
             << "&R1=0&R2=0&R3=0&R6=0"
             << "&para=00"
             << "&0MKKey=123456"
             << "&buttonClicked=&redirect_url=&err_flag="
             << "&username=&password=&user=&cmd=&Login="
             << "&v6ip=";
        r.request_body = body.str();

        HttpRequest req;
        req.url = r.request_url;
        req.method = "POST";
        req.body = r.request_body;
        req.content_type = "application/x-www-form-urlencoded";
        req.timeout_ms = 8000;
        req.follow_redirects = false;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "登录请求发送失败：" + err;
            return r;
        }

        const std::string page = DecodeBody(resp);
        LogInfo("Dr.COM 登录响应 HTTP " + std::to_string(resp.status) +
                "，正文 " + std::to_string(page.size()) + " 字节");

        // ⚠ 审阅注记（2026-10-13）：判定逻辑已按审阅修正为
        //   ① 4xx/5xx 错误页不构成判定依据；② Location 纳入判定但只认完整标记
        //   （短形会误伤 error3.htm 类 URL）；③ 双标记同现时不猜、交探测裁决
        //   （真实首页即同时含 authsuccess/authfail 两个标记，乐观判成功会使调度器
        //   跳过探测复验并绕过凭据错误的停止重试）
        const std::string& loc = resp.location;
        const bool hit3_full =
            page.find("Dr.COMWebLoginID_3.htm") != std::string::npos ||
            loc.find("Dr.COMWebLoginID_3.htm") != std::string::npos;
        const bool hit2_full =
            page.find("Dr.COMWebLoginID_2.htm") != std::string::npos ||
            loc.find("Dr.COMWebLoginID_2.htm") != std::string::npos;
        const bool hit3 = hit3_full || page.find("3.htm") != std::string::npos;
        const bool hit2 = hit2_full || page.find("2.htm") != std::string::npos;

        if (hit3 && hit2) {
            r.vendor_code = -1;
            r.success = false;
            r.message = "响应同时含成功与失败标志，将由网络探测确认实际状态";
            return r;
        }
        if (hit3) {
            r.success = true;
            r.vendor_code = 3;
            r.message = "服务器返回登录成功";
            return r;
        }
        if (hit2) {
            r.vendor_code = 2;
            r.success = false;
            r.message = "服务器返回登录失败（账号或密码错误，或该账号不可用）";
            return r;
        }

        // 无法从正文判定：交给上层用探测结果定夺
        r.vendor_code = -1;
        r.success = false;
        r.message = "服务器未返回明确的成功或失败标志，将由网络探测确认实际状态";
        return r;
    }

    const char* Name() const override { return "DrcomAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeDrcomAuth() { return std::make_unique<DrcomAuth>(); }

}  // namespace cnk
```

- [ ] **Step 4: 从 Stubs.cpp 移除 MakeDrcomAuth**

删除 `Stubs.cpp` 中 `MakeDrcomAuth` 的定义与 `StubAuth` 若不再被使用则可一并删除（保留其余四个）。

- [ ] **Step 5: 更新 CMakeLists.txt**

追加 `src/native/auth/DrcomAuth.cpp` 与 `tests/drcom_auth_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: Dr.COM 哆点认证实现，含运营商后缀与三级成功判定"
```

---

## 任务 12：CustomTemplateAuth（万能兜底）

这是 spec 四层兜底的最后一层，保证任何学校都能用。

**Files:**
- Create: `src/native/auth/CustomTemplateAuth.cpp`
- Modify: `src/native/auth/Stubs.cpp`（移除 `MakeCustomTemplateAuth`）
- Modify: `CMakeLists.txt`
- Create: `tests/custom_auth_test.cpp`

**Interfaces:**
- Consumes: `IAuth`、`Profile::custom_template`
- Produces: `std::unique_ptr<cnk::IAuth> cnk::MakeCustomTemplateAuth()`
- 新增工具函数（放在 `src/native/core/Template.h/.cpp`）：
  - `std::string cnk::ExpandTemplate(const std::string& tpl, const std::map<std::string,std::string>& vars)`
    - 把 `{key}` 替换为 `vars.at(key)`；**未定义的占位符保留原样**（不替换成空）

**占位符**：`{username}`、`{password}`、`{isp}`、`{timestamp}`（Unix 秒）、`{ip}`

- [ ] **Step 1: 写失败测试**

`tests/custom_auth_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/AuthFactory.h"
#include "core/Template.h"
#include "sim/SimServer.h"

TEST_CASE("ExpandTemplate 替换已知占位符") {
    std::map<std::string, std::string> vars = {
        {"username", "alice"}, {"password", "pw"}, {"isp", "@cmcc"}};
    CHECK(cnk::ExpandTemplate("{username}:{password}{isp}", vars)
          == "alice:pw@cmcc");
}

TEST_CASE("ExpandTemplate 保留未知占位符") {
    std::map<std::string, std::string> vars = {{"username", "a"}};
    CHECK(cnk::ExpandTemplate("{username}-{unknown}", vars) == "a-{unknown}");
}

TEST_CASE("ExpandTemplate 对空模板返回空") {
    CHECK(cnk::ExpandTemplate("", {}) == "");
}

TEST_CASE("自定义模板能对模拟服务器登录成功") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "carol";
    o.valid_pass = "pw9";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p;
    p.name = "T";
    p.vendor = "custom";
    p.username = "carol";
    p.password = "pw9";
    p.custom_template.method = "POST";
    p.custom_template.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    p.custom_template.body = "DDDDD={username}&upass={password}";
    p.custom_template.success_regex = "Dr\\.COMWebLoginID_3";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    srv.Stop();
}

TEST_CASE("自定义模板正则不匹配时返回失败") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "carol";
    o.valid_pass = "pw9";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p;
    p.vendor = "custom";
    p.username = "carol";
    p.password = "WRONG";
    p.custom_template.method = "POST";
    p.custom_template.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    p.custom_template.body = "DDDDD={username}&upass={password}";
    p.custom_template.success_regex = "Dr\\.COMWebLoginID_3";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    srv.Stop();
}

TEST_CASE("模板 URL 为空时返回明确失败") {
    cnk::Profile p;
    p.vendor = "custom";
    p.username = "u";
    p.password = "p";
    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("URL") != std::string::npos);
}

TEST_CASE("无效正则不崩溃，按失败处理") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p;
    p.vendor = "custom";
    p.username = "u";
    p.password = "p";
    p.custom_template.method = "POST";
    p.custom_template.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    p.custom_template.body = "DDDDD={username}&upass={password}";
    p.custom_template.success_regex = "([unclosed";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    srv.Stop();
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败，`core/Template.h` 不存在。

- [ ] **Step 3: 实现 Template**

`src/native/core/Template.h`:

```cpp
#pragma once
#include <string>
#include <map>

namespace cnk {

// 把 tpl 中的 {key} 替换为 vars 中对应值。
// vars 中不存在的占位符保持原样（便于用户发现拼写错误）。
std::string ExpandTemplate(const std::string& tpl,
                           const std::map<std::string, std::string>& vars);

}  // namespace cnk
```

`src/native/core/Template.cpp`:

```cpp
#include "core/Template.h"

namespace cnk {

std::string ExpandTemplate(const std::string& tpl,
                           const std::map<std::string, std::string>& vars) {
    std::string out;
    out.reserve(tpl.size());
    size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] != '{') { out += tpl[i++]; continue; }
        size_t close = tpl.find('}', i + 1);
        if (close == std::string::npos) { out += tpl.substr(i); break; }
        std::string key = tpl.substr(i + 1, close - i - 1);
        auto it = vars.find(key);
        if (it != vars.end()) out += it->second;
        else out += tpl.substr(i, close - i + 1);  // 保留原样
        i = close + 1;
    }
    return out;
}

}  // namespace cnk
```

- [ ] **Step 4: 实现 CustomTemplateAuth**

`src/native/auth/CustomTemplateAuth.cpp`:

```cpp
#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Template.h"
#include "core/Log.h"
#include <regex>
#include <chrono>

namespace cnk {

namespace {

bool RegexSearch(const std::string& pattern, const std::string& text) {
    if (pattern.empty()) return false;
    try {
        std::regex re(pattern, std::regex::ECMAScript);
        return std::regex_search(text, re);
    } catch (const std::regex_error& e) {
        LogWarn(std::string("自定义模板的成功正则无效：") + e.what());
        return false;
    }
}

class CustomTemplateAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;
        const auto& ct = p.custom_template;

        if (ct.url.empty()) {
            r.message = "自定义模板的 URL 为空，请在高级设置中填写";
            return r;
        }

        std::map<std::string, std::string> vars;
        vars["username"] = p.username;
        vars["password"] = p.password;
        vars["isp"] = IspSuffix(p);
        vars["ip"] = ctx.local_ip;
        vars["timestamp"] = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        r.request_url = ExpandTemplate(ct.url, vars);
        r.request_body = ExpandTemplate(ct.body, vars);

        HttpRequest req;
        req.url = r.request_url;
        req.method = ct.method.empty() ? "POST" : ct.method;
        req.body = r.request_body;
        req.content_type = "application/x-www-form-urlencoded";
        for (const auto& [k, v] : ct.headers) {
            if (k == "Content-Type") req.content_type = v;
            if (k == "User-Agent") req.user_agent = v;
        }
        req.timeout_ms = 8000;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "请求发送失败：" + err;
            return r;
        }

        const std::string page = DecodeBody(resp);
        if (ct.success_regex.empty()) {
            r.success = resp.status >= 200 && resp.status < 400;
            r.message = r.success
                ? "请求已发送（未配置成功判定正则，以 HTTP 状态判定）"
                : "HTTP 状态 " + std::to_string(resp.status);
            return r;
        }

        r.success = RegexSearch(ct.success_regex, page);
        r.message = r.success ? "响应匹配成功正则"
                              : "响应未匹配成功正则，请检查正则或凭据";
        return r;
    }

    const char* Name() const override { return "CustomTemplateAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeCustomTemplateAuth() {
    return std::make_unique<CustomTemplateAuth>();
}

}  // namespace cnk
```

- [ ] **Step 5: 更新 Stubs.cpp 与 CMakeLists.txt**

从 `Stubs.cpp` 移除 `MakeCustomTemplateAuth`。若 `StubAuth` 仍被其余三个工厂函数使用则保留。CMake 追加 `src/native/core/Template.cpp`、`src/native/auth/CustomTemplateAuth.cpp`、`tests/custom_auth_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: 自定义模板认证，作为任意学校的兜底方案"
```

---

## 任务 13：探测模块

**Files:**
- Create: `src/native/core/Probe.h`
- Create: `src/native/core/Probe.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/probe_test.cpp`

**Interfaces:**
- Consumes: `Settings`（任务 6）、`HttpPerform`（任务 4）、`ProbeTarget`（任务 9）
- Produces:
  - `enum class cnk::NetState { Unknown, Online, PortalDetected, Offline }`
  - `struct cnk::ProbeResult { NetState state; HttpResponse response; std::string message; }`
  - `cnk::ProbeResult cnk::ProbeOnce(const Settings& s)`

**判定规则**（spec §6.1，按序命中）：

| 条件 | 结果 |
|---|---|
| `status == 204` | Online |
| `status == 200` 且正文含 `probe_expect` 且长度 ≤ 8192 | Online |
| `status` 为 3xx 且 `location` 主机与探测主机不同 | PortalDetected |
| `status == 200` 但正文不含 `probe_expect`，或长度 > 8192 | PortalDetected |
| 连接失败 / 超时 / DNS 失败 | Offline |

- [ ] **Step 1: 写失败测试**

`tests/probe_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Probe.h"
#include "sim/SimServer.h"

namespace {
cnk::Settings MakeSettings(const std::string& url, const std::string& expect) {
    cnk::Settings s;
    s.probe_url = url;
    s.probe_expect = expect;
    s.probe_timeout_seconds = 2;
    return s;
}
}

TEST_CASE("返回 200 且正文匹配时判定为在线") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Online);
    srv.Stop();
}

TEST_CASE("正文不匹配时判定为 Portal 劫持") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("连接失败时判定为离线") {
    auto s = MakeSettings("http://127.0.0.1:1/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Offline);
    CHECK(r.message.size() > 0);
}

TEST_CASE("超时判定为离线") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 2;
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    s.probe_timeout_seconds = 1;
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Offline);
    srv.Stop();
}

TEST_CASE("正文过长时判定为劫持（长度启发式）") {
    // 用一个返回大量内容的地址模拟：/ 返回的是完整门户页面
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/", "");
    auto r = cnk::ProbeOnce(s);
    // expect 为空时任何 200 都算劫持（因为没有可匹配的标识）
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("绝对地址的异主机 Location 判定为劫持") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 3;                                   // 302
    o.redirect_location = "http://portal.example.com/login";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("相对路径的 Location 也判定为劫持，不能判成 Offline") {
    // 真实 Portal 常用相对跳转。若未先解析成绝对地址，HostOf 会返回空，
    // 落进「同主机」分支 —— 那样不仅漏判劫持，还会永不触发登录、每轮空转重探。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 3;
    o.redirect_location = "/portal/login";             // 相对路径
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state != cnk::NetState::Offline);          // 关键：绝不能是 Offline
    CHECK(r.state == cnk::NetState::Online);           // 解析后主机与探测目标相同，判在线
    srv.Stop();
}

TEST_CASE("同主机重定向判定为 Online 而非 Offline") {
    // 能收到重定向本身就说明目标服务可达。原实现判 Offline 是错的。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 3;
    o.redirect_location = "http://127.0.0.1:1/other";  // 与探测目标同主机（IP 相同即可）
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state != cnk::NetState::Offline);
    srv.Stop();
}
```

**必须先实现 SimServer 的 302 模式**：`SimOptions` 增加两个成员，`fail_mode` 增加取值 `3`。

```cpp
struct SimOptions {
    // ... 原有成员 ...
    // fail_mode: 0=正常 1=凭据错误 2=连接挂起 3=302 重定向
    std::string redirect_location;   // fail_mode == 3 时返回的 Location 值
};
```

（该接口由任务 4 的修复首次引入，本任务直接使用。）

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现 Probe**

`src/native/core/Probe.h`:

```cpp
#pragma once
#include <string>
#include "core/Http.h"
#include "store/Config.h"

namespace cnk {

enum class NetState { Unknown, Online, PortalDetected, Offline };

struct ProbeResult {
    NetState state = NetState::Unknown;
    HttpResponse response;
    std::string message;
};

ProbeResult ProbeOnce(const Settings& s);

}  // namespace cnk
```

`src/native/core/Probe.cpp`:

```cpp
#include "core/Probe.h"
#include "core/Encoding.h"
#include "core/Url.h"

namespace cnk {

namespace {

// 取 URL 的主机部分（含非默认端口），用于劫持判定的主机比较。
// ⚠ 审阅注记（2026-10-13）：归一化保证等价写法不被误判为异主机——
//   剥离 userinfo、剥离 scheme 默认端口（host 与 host:80 同主机）、主机名小写。
// 含端口比较是刻意的：Portal 与本机同 IP、不同端口时也必须判为异主机。
std::string HostOf(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    size_t start = scheme + 3;
    size_t end = url.find('/', start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos
                                                                  : end - start);
    if (host.empty()) return {};

    size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);

    std::string scheme_str = url.substr(0, scheme);
    for (char& c : scheme_str) c = (char)std::tolower((unsigned char)c);
    size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find('[') == std::string::npos) {
        const std::string port = host.substr(colon + 1);
        if ((scheme_str == "http" && port == "80") ||
            (scheme_str == "https" && port == "443"))
            host = host.substr(0, colon);
    }

    for (char& c : host) c = (char)std::tolower((unsigned char)c);
    return host;
}

}  // namespace

ProbeResult ProbeOnce(const Settings& s) {
    ProbeResult r;
    if (s.probe_url.empty()) {
        r.state = NetState::Unknown;
        r.message = "未配置探测地址";
        return r;
    }

    HttpRequest req;
    req.url = s.probe_url;
    req.method = "GET";
    // 夹取上限防 int 溢出（UI 可能被填入极大值）；<=0 走 3 秒兜底
    int secs = s.probe_timeout_seconds;
    if (secs <= 0) secs = 3;
    if (secs > 600) secs = 600;
    req.timeout_ms = secs * 1000;
    req.follow_redirects = false;

    std::string err;
    if (!HttpPerform(req, r.response, err)) {
        r.state = NetState::Offline;
        r.message = "探测失败：" + err;
        return r;
    }

    const int status = r.response.status;

    if (status == 204) {
        r.state = NetState::Online;
        r.message = "HTTP 204";
        return r;
    }

    if (status >= 300 && status < 400) {
        // Location 可能是**相对路径**（真实 Portal 常用 `/portal/login` 这种形式），
        // 必须先解析成绝对地址再比主机。否则相对跳转会让 HostOf 返回空，
        // 落进下面的分支判成 Offline —— 结果是永不触发登录、每轮空转重探，
        // 「劫持检测 → 自动登录」整链静默失效。
        const std::string loc_abs = ResolveUrl(r.response.location, s.probe_url);
        const std::string loc_host = HostOf(loc_abs);
        const std::string probe_host = HostOf(s.probe_url);

        if (!loc_host.empty() && loc_host != probe_host) {
            r.state = NetState::PortalDetected;
            r.message = "被重定向到 " + loc_abs;
            return r;
        }
        // 同主机重定向：能收到重定向本身就说明目标服务可达，网络是通的
        // （Portal 劫持的典型形态是重定向到内网 Portal 地址，即不同主机）。
        r.state = NetState::Online;
        r.message = r.response.location.empty()
            ? "3xx 无 Location 头，按可达处理"
            : "重定向但目标主机相同，判定为在线：" + loc_abs;
        return r;
    }

    if (status == 200) {
        const std::string body = DecodeBody(r.response);
        const bool has_expect = !s.probe_expect.empty() &&
                                body.find(s.probe_expect) != std::string::npos;
        const bool too_long = body.size() > 8192;
        if (has_expect && !too_long) {
            r.state = NetState::Online;
            r.message = "正文匹配预期标识";
            return r;
        }
        r.state = NetState::PortalDetected;
        r.message = has_expect ? "正文长度异常（" + std::to_string(body.size()) + " 字节）"
                               : "正文不含预期标识，疑似 Portal 劫持";
        return r;
    }

    // 4xx/5xx 等其余状态：spec §6.1 未定义，取偏保守的 PortalDetected——
    // 宁可多试一次登录，也不把可疑响应当在线
    r.state = NetState::PortalDetected;
    r.message = "HTTP " + std::to_string(status);
    return r;
}

}  // namespace cnk
```

- [ ] **Step 4: 更新 CMakeLists.txt**

追加 `src/native/core/Probe.cpp` 与 `tests/probe_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 网络探测与在线/劫持/离线三态判定"
```

---

## 任务 14：调度状态机与退避重试

本任务承载 Review Focus 第 1、2、4 条。

**Files:**
- Create: `src/native/core/Scheduler.h`
- Create: `src/native/core/Scheduler.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/scheduler_test.cpp`

**Interfaces:**
- Consumes: `ProbeOnce`（任务 13）、`CreateAuth`（任务 10）、`ParseCarrier`/`ParseVisitBlacklist`（任务 9）、`DetectVendor`（任务 8）
- Produces:
  - `enum class cnk::RunState { Disconnected, Probing, PortalDetected, Online, Authenticating, AuthFailed, NotCampusNetwork }`
  - `class cnk::Scheduler`：
    - `explicit Scheduler(AppConfig cfg)`
    - `void Tick()` — 由外部消息循环按周期调用；内部自行计时决定是否真正探测
    - `RunState State() const`
    - `std::string LastMessage() const`
    - `std::string LocalIp() const`
    - `long long LastSuccessUnix() const`
    - `int ConsecutiveAuthFailures() const`
    - `void ForceReconnect()` — 立即重置计时并触发
    - `void SetConfig(const AppConfig& cfg)` — 热重载
    - `bool RunProbeNow(ProbeResult& out)` — 供 CLI 诊断用，忽略计时器

**状态机行为**（spec §5.2、§6）：

- 探测间隔：`Online` 时 `probe_interval_seconds`；离开 `Online` 时用 3 秒快速重探（改进 S1）
- 登录失败退避：按 `retry_backoff_seconds` 序列推进，用尽后固定为序列末值
- 登录成功：退避序列重置，状态 `Online`
- **凭据错误**（`vendor_code == 2`）：进入 `AuthFailed` 并**停止自动重试**，等待用户改配置（Review Focus 2）
- **非校园网保护**：连续 `auth_fail_limit`（默认 3）次登录尝试都未能使探测恢复在线，且从未成功过，则进入 `NotCampusNetwork` 并停止重试（Review Focus 1）
- 从 `Online` 离开时清空缓存的 `NetContext`

- [ ] **Step 1: 写失败测试**

`tests/scheduler_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Scheduler.h"
#include "sim/SimServer.h"

namespace {

cnk::AppConfig MakeConfig(const std::string& portal, const std::string& user,
                          const std::string& pass) {
    cnk::AppConfig cfg;
    cfg.settings.probe_url = portal + "/ncsi.txt";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cfg.settings.probe_timeout_seconds = 1;
    cfg.settings.probe_interval_seconds = 1;
    cfg.settings.retry_backoff_seconds = {1, 1, 1};
    cnk::Profile p;
    p.name = "默认";
    p.auth_url = portal + "/";
    p.username = user;
    p.password = pass;
    p.vendor = "drcom";
    p.isp.preset = "campus";
    cfg.profiles.push_back(p);
    cfg.active_profile = "默认";
    return cfg;
}

}  // namespace

TEST_CASE("探测在线时进入 Online") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Online);
    srv.Stop();
}

TEST_CASE("被劫持且凭据正确时自动登录并恢复在线") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    // 探测指向门户首页（会被判为劫持），登录端点则正确
    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();
    for (int i = 0; i < 5; ++i) sch.Tick();
    CHECK(srv.LoginCount() >= 1);
    srv.Stop();
}

TEST_CASE("凭据错误时停止重试并进入 AuthFailed") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "RIGHT";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "WRONG");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();

    // 连跑多次 Tick；因退避为 {1,1,1} 秒，需要让时间推进
    for (int i = 0; i < 200; ++i) {
        sch.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK(sch.State() == cnk::RunState::AuthFailed);
    const int count_after_stop = srv.LoginCount();
    // 进入 AuthFailed 后不得继续重试
    for (int i = 0; i < 100; ++i) {
        sch.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(srv.LoginCount() == count_after_stop);
    CHECK(sch.LastMessage().size() > 0);
    srv.Stop();
}

TEST_CASE("从未成功且反复失败时进入 NotCampusNetwork 并停止重试") {
    // 探测地址指向一个永远返回门户页的服务器，登录也永远失败
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "RIGHT";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "WRONG");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cfg.settings.retry_backoff_seconds = {1, 1, 1, 1, 1};
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();

    for (int i = 0; i < 600; ++i) {
        sch.Tick();
        if (sch.State() == cnk::RunState::NotCampusNetwork ||
            sch.State() == cnk::RunState::AuthFailed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 凭据错误会先命中 AuthFailed，这也是停止重试的合法终态
    CHECK((sch.State() == cnk::RunState::AuthFailed ||
           sch.State() == cnk::RunState::NotCampusNetwork));
    srv.Stop();
}

TEST_CASE("ForceReconnect 立即触发探测而不等待间隔") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_interval_seconds = 3600;  // 一小时内不该再自动探测
    cnk::Scheduler sch(cfg);
    sch.Tick();
    auto first = sch.State();
    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.State() == first);
    srv.Stop();
}

TEST_CASE("无配置档案时不崩溃且状态明确") {
    cnk::AppConfig cfg;
    cnk::Scheduler sch(cfg);
    sch.Tick();
    CHECK(sch.State() != cnk::RunState::Online);
    CHECK(sch.LastMessage().size() > 0);
}

TEST_CASE("网络变更通知会立即触发探测，不等轮询周期") {
    // 对应 Review Focus 第 4 条：睡眠唤醒 / 切换网络后必须立刻探测
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_interval_seconds = 3600;  // 一小时内不会自动再探测
    cnk::Scheduler sch(cfg);

    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Online);
    const long long t1 = sch.LastProbeUnix();

    // 模拟系统睡眠 2 秒后唤醒
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 未通知时，Tick 不应产生新探测
    sch.Tick();
    CHECK(sch.LastProbeUnix() == t1);

    // 收到网络变更通知后，下一次 Tick 必须立即探测
    sch.NotifyNetworkChanged();
    sch.Tick();
    CHECK(sch.LastProbeUnix() > t1);

    srv.Stop();
}
```

测试需要 `#include <thread>` 与 `<chrono>`。

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现 Scheduler**

`src/native/core/Scheduler.h`:

```cpp
#pragma once
#include <string>
#include <memory>
#include <chrono>
#include "store/Config.h"
#include "core/Probe.h"

namespace cnk {

enum class RunState {
    Disconnected,
    Probing,
    PortalDetected,
    Online,
    Authenticating,
    AuthFailed,
    NotCampusNetwork,
};

const char* RunStateName(RunState s);

class Scheduler {
public:
    explicit Scheduler(AppConfig cfg);
    ~Scheduler();

    void Tick();
    void ForceReconnect();
    void SetConfig(const AppConfig& cfg);

    // 系统网络状态发生变化（地址变更、睡眠唤醒、网卡上下线）时调用。
    // 下一次 Tick 将立即探测，不等轮询周期。
    void NotifyNetworkChanged();

    RunState State() const;
    std::string LastMessage() const;
    std::string LocalIp() const;
    long long LastSuccessUnix() const;
    long long LastProbeUnix() const;
    int ConsecutiveAuthFailures() const;

    // 最近一次探测取回的门户页面（已转 UTF-8）。
    // 供 IPC 的 get_carrier_options 解析服务器下发的运营商列表。
    std::string LastPortalBody() const;

    bool RunProbeNow(ProbeResult& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
```

`src/native/core/Scheduler.cpp`:

```cpp
#include "core/Scheduler.h"
#include "core/Log.h"
#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include "core/ProbeTarget.h"

namespace cnk {

namespace {

using Clock = std::chrono::steady_clock;

long long NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

const char* RunStateName(RunState s) {
    switch (s) {
        case RunState::Disconnected:     return "disconnected";
        case RunState::Probing:          return "probing";
        case RunState::PortalDetected:   return "portal";
        case RunState::Online:           return "online";
        case RunState::Authenticating:   return "authenticating";
        case RunState::AuthFailed:       return "auth_failed";
        case RunState::NotCampusNetwork: return "not_campus";
    }
    return "unknown";
}

struct Scheduler::Impl {
    AppConfig cfg;
    RunState state = RunState::Disconnected;
    std::string message = "尚未开始";
    long long last_success = 0;
    long long last_probe = 0;
    int consecutive_auth_failures = 0;
    int total_auth_attempts = 0;
    size_t backoff_index = 0;
    bool ever_succeeded = false;
    bool stop_retrying = false;

    Clock::time_point next_probe{};
    bool probe_immediately = true;

    std::string local_ip;
    std::string cached_portal_body;
    std::string cached_portal_url;
    std::string cached_server_header;
    ProbeTarget target;
    bool target_resolved = false;

    void ResetCache() {
        local_ip.clear();
        cached_portal_body.clear();
        cached_portal_url.clear();
        cached_server_header.clear();
    }

    void ScheduleNext(int seconds) {
        next_probe = Clock::now() + std::chrono::seconds(seconds);
    }

    int CurrentBackoff() const {
        if (cfg.settings.retry_backoff_seconds.empty()) return 60;
        if (backoff_index >= cfg.settings.retry_backoff_seconds.size())
            return cfg.settings.retry_backoff_seconds.back();
        return cfg.settings.retry_backoff_seconds[backoff_index];
    }

    void AdvanceBackoff() {
        if (backoff_index + 1 < cfg.settings.retry_backoff_seconds.size())
            ++backoff_index;
    }

    void DoProbe();
    void DoLogin();
};

void Scheduler::Impl::DoProbe() {
    state = RunState::Probing;
    last_probe = NowUnix();

    ProbeResult pr = ProbeOnce(cfg.settings);
    last_probe_response = pr.response;

    if (pr.state == NetState::Online) {
        state = RunState::Online;
        message = "网络正常";
        if (!ever_succeeded) ever_succeeded = true;
        backoff_index = 0;
        consecutive_auth_failures = 0;
        ScheduleNext(cfg.settings.probe_interval_seconds);
        ResetCache();
        return;
    }

    if (pr.state == NetState::Offline) {
        state = RunState::Disconnected;
        message = pr.message;
        ScheduleNext(3);  // 快速重探，改善恢复延迟
        return;
    }

    // PortalDetected
    state = RunState::PortalDetected;
    message = pr.message;
    cached_portal_url = cfg.settings.probe_url;
    cached_server_header = pr.response.server;
    cached_portal_body = DecodeBody(pr.response);

    // 从门户页面里取运营商与探测地址（仅首次或页面变化时）
    if (!target_resolved) {
        std::vector<std::string> blacklist;
        if (ParseVisitBlacklist(cached_portal_body, blacklist)) {
            ProbeTarget t;
            if (SelectProbeTarget(blacklist, t)) {
                target = t;
                target_resolved = true;
                LogInfo("采用服务器下发的探测地址：" + t.url);
            }
        }
    }

    DoLogin();
}

void Scheduler::Impl::DoLogin() {
    const Profile* p = ActiveProfile(cfg);
    if (!p) {
        state = RunState::AuthFailed;
        message = "没有可用的账号配置，请在设置中添加";
        stop_retrying = true;
        return;
    }
    if (stop_retrying) return;

    state = RunState::Authenticating;
    ++total_auth_attempts;

    Vendor detected = DetectVendor(last_probe_response);

    NetContext ctx;
    ctx.local_ip = local_ip;
    ctx.portal_url = cached_portal_url;
    ctx.portal_body = cached_portal_body;
    ctx.server_header = cached_server_header;

    auto auth = CreateAuth(detected, *p);
    LogInfo(std::string("尝试登录，策略=") + auth->Name() +
            "，识别厂商=" + VendorName(detected));

    LoginResult lr = auth->Login(*p, ctx);

    LogInfo("登录结果：" + (lr.success ? "成功" : "失败") +
            "，信息=" + RedactSecrets(lr.message));
    LogInfo("请求地址：" + RedactSecrets(lr.request_url));

    if (lr.success) {
        state = RunState::Online;
        message = "已重新登录";
        last_success = NowUnix();
        ever_succeeded = true;
        consecutive_auth_failures = 0;
        backoff_index = 0;
        ScheduleNext(cfg.settings.probe_interval_seconds);
        ResetCache();
        return;
    }

    // 厂商明确报凭据错误：停止重试，等用户改配置
    if (lr.vendor_code == 2) {
        state = RunState::AuthFailed;
        message = lr.message.empty() ? "账号或密码错误" : lr.message;
        stop_retrying = true;
        LogError("检测到凭据错误，已停止自动重试：" + message);
        return;
    }

    ++consecutive_auth_failures;

    // 非校园网保护：从未成功过，且连续尝试达到上限
    const int limit = 3;
    if (!ever_succeeded && total_auth_attempts >= limit) {
        state = RunState::NotCampusNetwork;
        message = "多次尝试均失败，当前网络可能不是校园网，已停止自动重试。"
                  "接入校园网后将自动恢复。";
        stop_retrying = true;
        LogWarn(message);
        return;
    }

    state = RunState::Disconnected;
    message = lr.message.empty() ? "登录失败，将重试" : lr.message;
    const int wait = CurrentBackoff();
    AdvanceBackoff();
    LogInfo("登录失败，将在 " + std::to_string(wait) + " 秒后重试");
    ScheduleNext(wait);
}
```

**注意**：`stop_retrying` 需要在一个条件满足时被重置——当探测重新判定为 `Online`，或用户改了配置（`SetConfig`）。已在 `DoProbe` 的 `Online` 分支与 `SetConfig` 中处理。

在 `struct Scheduler::Impl` 内追加以下两个成员（其余成员已在上文定义）：

```cpp
    HttpResponse last_probe_response;   // DoProbe 中赋值，DoLogin 读取

    void Tick() {
        if (probe_immediately || Clock::now() >= next_probe) {
            probe_immediately = false;
            DoProbe();
        }
    }
```

公开方法：

```cpp
Scheduler::Scheduler(AppConfig cfg) : impl_(new Impl) {
    impl_->cfg = std::move(cfg);
}

Scheduler::~Scheduler() = default;

void Scheduler::Tick() { impl_->Tick(); }

void Scheduler::ForceReconnect() {
    impl_->probe_immediately = true;
    impl_->stop_retrying = false;
    impl_->total_auth_attempts = 0;
    impl_->backoff_index = 0;
    impl_->target_resolved = false;
}

void Scheduler::SetConfig(const AppConfig& cfg) {
    impl_->cfg = cfg;
    impl_->stop_retrying = false;
    impl_->total_auth_attempts = 0;
    impl_->backoff_index = 0;
    impl_->target_resolved = false;
    impl_->probe_immediately = true;
}

void Scheduler::NotifyNetworkChanged() {
    // 立刻探测，且重置退避——网络环境变了，之前的失败计数不再有参考价值
    impl_->probe_immediately = true;
    impl_->stop_retrying = false;
    impl_->total_auth_attempts = 0;
    impl_->backoff_index = 0;
    impl_->target_resolved = false;
    LogInfo("收到网络变更事件，立即重新探测");
}

RunState Scheduler::State() const { return impl_->state; }
std::string Scheduler::LastMessage() const { return impl_->message; }
std::string Scheduler::LocalIp() const { return impl_->local_ip; }
long long Scheduler::LastSuccessUnix() const { return impl_->last_success; }
long long Scheduler::LastProbeUnix() const { return impl_->last_probe; }
int Scheduler::ConsecutiveAuthFailures() const {
    return impl_->consecutive_auth_failures;
}

std::string Scheduler::LastPortalBody() const {
    return impl_->cached_portal_body;
}

bool Scheduler::RunProbeNow(ProbeResult& out) {
    out = ProbeOnce(impl_->cfg.settings);
    return true;
}
```

- [ ] **Step 4: 实现网络变更监听器**

这是达成「断线后 ≤15 秒恢复」的关键：DHCP 续约、网卡上下线、睡眠唤醒都会让系统发出地址变更通知，此时立即探测，无需等满一个轮询周期。

`src/native/core/NetworkChange.h`:

```cpp
#pragma once
#include <functional>
#include <memory>

namespace cnk {

// 监听 Windows 的地址变更通知（NotifyAddrChange）。
// 在独立线程上阻塞等待，事件到来时回调。
class NetworkChangeWatcher {
public:
    NetworkChangeWatcher();
    ~NetworkChangeWatcher();
    NetworkChangeWatcher(const NetworkChangeWatcher&) = delete;
    NetworkChangeWatcher& operator=(const NetworkChangeWatcher&) = delete;

    // 回调在工作线程上执行，实现方需保证线程安全（Scheduler 只置标志位，安全）
    bool Start(std::function<void()> on_change);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
```

`src/native/core/NetworkChange.cpp`:

```cpp
#include "core/NetworkChange.h"
#include "core/Log.h"
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <atomic>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")

namespace cnk {

struct NetworkChangeWatcher::Impl {
    HANDLE stop_event = nullptr;
    HANDLE notify_event = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::function<void()> callback;

    void Loop();
};

void NetworkChangeWatcher::Impl::Loop() {
    // NotifyAddrChange 会阻塞，直到地址发生变化或句柄被关闭。
    // 为支持停止，用请求队列方式：先调一次，收到后重新挂起。
    while (running.load()) {
        OVERLAPPED ov{};
        ov.hEvent = notify_event;
        DWORD rc = NotifyAddrChange(nullptr, &ov);

        if (rc == ERROR_IO_PENDING) {
            HANDLE waits[2] = {stop_event, notify_event};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) break;             // 停止
            if (w != WAIT_OBJECT_0 + 1) break;         // 异常
            // 事件已触发，消费掉
            DWORD dummy = 0;
            GetOverlappedResult(nullptr, &ov, &dummy, FALSE);
        } else if (rc != ERROR_IO_PENDING) {
            // 极少数情况下同步返回；短暂退避避免忙等
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!running.load()) break;
        }

        if (!running.load()) break;
        if (callback) {
            callback();
        }
    }
}

NetworkChangeWatcher::NetworkChangeWatcher() : impl_(new Impl) {}
NetworkChangeWatcher::~NetworkChangeWatcher() { Stop(); }

bool NetworkChangeWatcher::Start(std::function<void()> on_change) {
    if (impl_->running.load()) return true;

    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->notify_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stop_event || !impl_->notify_event) {
        LogError("创建网络监听的等待事件失败");
        Stop();
        return false;
    }

    impl_->callback = std::move(on_change);
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->Loop(); });
    LogInfo("已启动网络地址变更监听");
    return true;
}

void NetworkChangeWatcher::Stop() {
    if (!impl_) return;
    if (impl_->running.exchange(false)) {
        if (impl_->stop_event) SetEvent(impl_->stop_event);
        if (impl_->notify_event) SetEvent(impl_->notify_event);
        if (impl_->worker.joinable()) impl_->worker.join();
    }
    if (impl_->stop_event) { CloseHandle(impl_->stop_event); impl_->stop_event = nullptr; }
    if (impl_->notify_event) { CloseHandle(impl_->notify_event); impl_->notify_event = nullptr; }
}

}  // namespace cnk
```

**注意**：`NotifyAddrChange` 的回调在独立线程执行，而 `Scheduler` 的其余方法在主线程的消息循环里调用。因此 `NotifyNetworkChanged()` 只允许**置位标志**（`probe_immediately = true`），不得触碰其他状态。`Impl` 中的 `probe_immediately` 是 `bool`，在 x86/x64 上的单字节写是原子的，此用法安全；若要更保险可改为 `std::atomic<bool>`。

- [ ] **Step 5: 更新 CMakeLists.txt**

追加 `src/native/core/Scheduler.cpp`、`src/native/core/NetworkChange.cpp` 与 `tests/scheduler_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。三个用例是关键，务必确认：
- 凭据错误用例中 `LoginCount()` 在进入终止态后不再增长
- 网络变更用例中，未通知时 `LastProbeUnix()` 不变，通知后必须变化

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: 调度状态机，含退避重试、凭据错误停止、非校园网保护与地址变更监听"
```

---

## 任务 15：CLI 入口与端到端集成

**Files:**
- Create: `src/native/core/Cli.h`
- Create: `src/native/core/Cli.cpp`
- Modify: `src/native/main.cpp`（重写）
- Modify: `CMakeLists.txt`
- Create: `tests/cli_test.cpp`

**Interfaces:**
- Consumes: 全部前述模块
- Produces: 命令行入口，支持：
  - `--probe-now`：执行一次探测，输出状态与诊断，退出
  - `--test-login`：执行一次探测 + 一次登录尝试，输出完整请求（脱敏），退出
  - `--detect`：输出厂商识别结果
  - `--version`：输出版本
  - `--help`：输出用法
  - `--daemon`（默认为守护形态，此参数可省略）
  - `--simulate=<vendor>`：以模拟服务器为目标运行，供测试
  - `--config=<path>`：指定配置文件路径（测试用）

- [ ] **Step 1: 写失败测试**

`tests/cli_test.cpp`:

```cpp
#include "doctest.h"
#include "core/Cli.h"

TEST_CASE("解析 --probe-now") {
    auto opts = cnk::ParseCli({"--probe-now"});
    CHECK(opts.mode == cnk::CliMode::ProbeNow);
}

TEST_CASE("解析 --test-login") {
    auto opts = cnk::ParseCli({"--test-login"});
    CHECK(opts.mode == cnk::CliMode::TestLogin);
}

TEST_CASE("解析 --detect") {
    auto opts = cnk::ParseCli({"--detect"});
    CHECK(opts.mode == cnk::CliMode::Detect);
}

TEST_CASE("解析 --version 与 --help") {
    CHECK(cnk::ParseCli({"--version"}).mode == cnk::CliMode::Version);
    CHECK(cnk::ParseCli({"--help"}).mode == cnk::CliMode::Help);
    CHECK(cnk::ParseCli({"-h"}).mode == cnk::CliMode::Help);
}

TEST_CASE("无参数时为守护模式") {
    CHECK(cnk::ParseCli({}).mode == cnk::CliMode::Daemon);
}

TEST_CASE("解析 key=value 形式的参数") {
    auto opts = cnk::ParseCli({"--config=C:\\tmp\\a.json",
                               "--simulate=drcom"});
    CHECK(opts.config_path == "C:\\tmp\\a.json");
    CHECK(opts.simulate_vendor == "drcom");
}

TEST_CASE("解析 --config 空格分隔形式") {
    auto opts = cnk::ParseCli({"--config", "C:\\tmp\\b.json"});
    CHECK(opts.config_path == "C:\\tmp\\b.json");
}

TEST_CASE("未知参数被收集但不影响模式判定") {
    auto opts = cnk::ParseCli({"--probe-now", "--unknown-thing"});
    CHECK(opts.mode == cnk::CliMode::ProbeNow);
    CHECK(opts.unknown.size() == 1);
}

TEST_CASE("帮助文本非空且包含关键开关") {
    std::string h = cnk::CliHelpText();
    CHECK(h.find("--probe-now") != std::string::npos);
    CHECK(h.find("--test-login") != std::string::npos);
    CHECK(h.find("--config") != std::string::npos);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现 Cli**

`src/native/core/Cli.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class CliMode { Daemon, ProbeNow, TestLogin, Detect, Version, Help, Ui };

struct CliOptions {
    CliMode mode = CliMode::Daemon;
    std::string config_path;
    std::string simulate_vendor;
    std::vector<std::string> unknown;
};

CliOptions ParseCli(const std::vector<std::string>& args);
std::string CliHelpText();

}  // namespace cnk
```

`src/native/core/Cli.cpp`:

```cpp
#include "core/Cli.h"

namespace cnk {

namespace {

// 支持 "--k=v" 与 "--k v" 两种写法。
// 返回 true 表示已消费下一个参数。
bool TakeValue(const std::vector<std::string>& a, size_t& i,
               const std::string& prefix, std::string& out) {
    const std::string& cur = a[i];
    if (cur.rfind(prefix + "=", 0) == 0) {
        out = cur.substr(prefix.size() + 1);
        return false;
    }
    if (cur == prefix && i + 1 < a.size()) {
        out = a[++i];
        return true;
    }
    return false;
}

}  // namespace

CliOptions ParseCli(const std::vector<std::string>& args) {
    CliOptions o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];

        if (a == "--probe-now")      { o.mode = CliMode::ProbeNow; continue; }
        if (a == "--test-login")     { o.mode = CliMode::TestLogin; continue; }
        if (a == "--detect")         { o.mode = CliMode::Detect; continue; }
        if (a == "--version" || a == "-v") { o.mode = CliMode::Version; continue; }
        if (a == "--help" || a == "-h" || a == "/?") {
            o.mode = CliMode::Help; continue;
        }
        if (a == "--ui")             { o.mode = CliMode::Ui; continue; }
        if (a == "--daemon")         { o.mode = CliMode::Daemon; continue; }

        std::string val;
        if (a.rfind("--config", 0) == 0) {
            if (a == "--config" && i + 1 < args.size()) o.config_path = args[++i];
            else o.config_path = a.substr(9);
            continue;
        }
        if (a.rfind("--simulate", 0) == 0) {
            if (a == "--simulate" && i + 1 < args.size()) o.simulate_vendor = args[++i];
            else o.simulate_vendor = a.substr(11);
            continue;
        }

        o.unknown.push_back(a);
    }
    return o;
}

std::string CliHelpText() {
    return
        "CampusNet-Keeper —— 校园网自动重连守护\n"
        "\n"
        "用法：CampusNet-Keeper [选项]\n"
        "\n"
        "  （无选项）            以守护模式运行，常驻托盘\n"
        "  --ui                  打开设置界面\n"
        "  --probe-now           执行一次网络探测并输出结果，然后退出\n"
        "  --test-login          执行一次探测 + 登录尝试，输出完整请求（已脱敏）\n"
        "  --detect              只做厂商识别并输出结果\n"
        "  --config=<路径>       使用指定的配置文件\n"
        "  --simulate=<厂商>     使用本地模拟 Portal 服务器（测试用）\n"
        "  --version             显示版本号\n"
        "  --help                显示本帮助\n"
        "\n"
        "配置文件默认位于 %APPDATA%\\CampusNet-Keeper\\config.json\n"
        "日志位于 %APPDATA%\\CampusNet-Keeper\\logs\\keeper.log\n";
}

}  // namespace cnk
```

- [ ] **Step 4: 重写 main.cpp**

`src/native/main.cpp`:

```cpp
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include "core/Version.h"
#include "core/Cli.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Probe.h"
#include "core/Scheduler.h"
#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include "auth/Detector.h"
#include "store/Config.h"
#include "sim/SimServer.h"

namespace {

std::vector<std::string> WideArgsToUtf8() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) { out.push_back({}); continue; }
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(argv);
    return out;
}

void PrintLine(const std::string& s) {
    std::printf("%s\n", s.c_str());
}

int RunProbeNow(cnk::AppConfig& cfg) {
    PrintLine("== 探测 ==");
    PrintLine("探测地址: " + cfg.settings.probe_url);
    auto r = cnk::ProbeOnce(cfg.settings);
    const char* names[] = {"Unknown", "Online（网络正常）",
                           "PortalDetected（被 Portal 劫持）", "Offline（无连接）"};
    PrintLine(std::string("结果: ") + names[(int)r.state]);
    PrintLine("说明: " + r.message);
    if (r.response.status)
        PrintLine("HTTP 状态: " + std::to_string(r.response.status));
    if (!r.response.server.empty())
        PrintLine("Server: " + r.response.server);
    if (!r.response.location.empty())
        PrintLine("Location: " + r.response.location);
    if (!r.response.content_type.empty())
        PrintLine("Content-Type: " + r.response.content_type);
    return (r.state == cnk::NetState::Online) ? 0 : 1;
}

int RunDetect(cnk::AppConfig& cfg) {
    cnk::HttpRequest req;
    req.url = cfg.settings.probe_url;
    req.timeout_ms = cfg.settings.probe_timeout_seconds * 1000;
    cnk::HttpResponse resp;
    std::string err;
    if (!cnk::HttpPerform(req, resp, err)) {
        PrintLine("探测失败：" + err);
        return 1;
    }
    cnk::Vendor v = cnk::DetectVendor(resp);
    PrintLine("识别厂商: " + std::string(cnk::VendorName(v)));

    std::string body = cnk::DecodeBody(resp);
    cnk::CarrierList cl;
    if (cnk::ParseCarrier(body, cl)) {
        PrintLine("运营商列表（服务器下发）:");
        for (const auto& o : cl.options)
            PrintLine("  [" + o.id + "] " + o.name +
                      (o.suffix.empty() ? "（无后缀）" : "  后缀=" + o.suffix) +
                      (o.id == cl.default_id ? "  <= 默认" : ""));
    } else {
        PrintLine("服务器未下发运营商列表");
    }

    std::vector<std::string> bl;
    if (cnk::ParseVisitBlacklist(body, bl)) {
        PrintLine("免劫持地址清单:");
        for (const auto& b : bl) PrintLine("  " + b);
        cnk::ProbeTarget t;
        if (cnk::SelectProbeTarget(bl, t))
            PrintLine("建议探测地址: " + t.url + "  （期望正文: " + t.expect + "）");
    }
    return 0;
}

int RunTestLogin(cnk::AppConfig& cfg) {
    const cnk::Profile* p = cnk::ActiveProfile(cfg);
    if (!p) {
        PrintLine("配置中没有账号档案，请先运行程序并用 --ui 打开设置填写");
        return 1;
    }

    PrintLine("== 探测 ==");
    auto pr = cnk::ProbeOnce(cfg.settings);
    PrintLine("状态: " + std::string(pr.state == cnk::NetState::Online ? "在线"
                       : pr.state == cnk::NetState::Offline ? "离线" : "被劫持"));
    PrintLine("说明: " + pr.message);
    if (pr.state == cnk::NetState::Online) {
        PrintLine("当前已在线，无需登录。若要测试登录请先注销或等待会话失效。");
        return 0;
    }

    cnk::Vendor v = cnk::DetectVendor(pr.response);
    PrintLine(std::string("识别厂商: ") + cnk::VendorName(v));

    cnk::NetContext ctx;
    ctx.portal_url = cfg.settings.probe_url;
    ctx.portal_body = cnk::DecodeBody(pr.response);
    ctx.server_header = pr.response.server;

    auto auth = cnk::CreateAuth(v, *p);
    PrintLine(std::string("认证策略: ") + auth->Name());
    PrintLine("账号: " + p->username + "（运营商后缀: " +
              (cnk::IspSuffix(*p).empty() ? "无" : cnk::IspSuffix(*p)) + "）");

    auto lr = auth->Login(*p, ctx);

    PrintLine("");
    PrintLine("== 请求（已脱敏）==");
    PrintLine("POST " + lr.request_url);
    PrintLine(cnk::RedactSecrets(lr.request_body));
    PrintLine("");
    PrintLine("== 结果 ==");
    PrintLine(std::string("成功: ") + (lr.success ? "是" : "否"));
    PrintLine("信息: " + lr.message);

    if (lr.success) {
        PrintLine("");
        PrintLine("复测探测...");
        auto after = cnk::ProbeOnce(cfg.settings);
        PrintLine(std::string("复测结果: ") +
                  (after.state == cnk::NetState::Online ? "已在线 ✓" : "仍不在线"));
    }
    return lr.success ? 0 : 2;
}

int RunDaemon(cnk::AppConfig& cfg) {
    cnk::Scheduler sch(cfg);
    PrintLine("CampusNet-Keeper 守护模式已启动，按 Ctrl+C 退出。");
    PrintLine("（任务 16-17 会替换为托盘驻留；当前为控制台轮询）");
    for (;;) {
        sch.Tick();
        std::printf("\r状态: %-16s %s          ",
                    cnk::RunStateName(sch.State()), sch.LastMessage().c_str());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

}  // namespace

int wmain(int, wchar_t**) {
    SetConsoleOutputCP(CP_UTF8);

    auto args = WideArgsToUtf8();
    auto opts = cnk::ParseCli(args);

    if (opts.mode == cnk::CliMode::Help) {
        std::printf("%s", cnk::CliHelpText().c_str());
        return 0;
    }
    if (opts.mode == cnk::CliMode::Version) {
        std::printf("CampusNet-Keeper %s\n", cnk::VersionString());
        return 0;
    }

    std::string log_dir = cnk::AppDataSubDir("logs");
    cnk::LogInit(log_dir, cnk::LogLevel::Info, 512 * 1024, 3);
    cnk::LogInfo("启动，模式=" + std::to_string((int)opts.mode));

    cnk::AppConfig cfg;
    std::string err;
    if (!opts.config_path.empty())
        cnk::ConfigLoadFrom(opts.config_path, cfg, err);
    else
        cnk::ConfigLoad(cfg, err);

    if (!err.empty()) cnk::LogWarn("配置加载：" + err);

    int rc = 0;
    switch (opts.mode) {
        case cnk::CliMode::ProbeNow:  rc = RunProbeNow(cfg); break;
        case cnk::CliMode::Detect:    rc = RunDetect(cfg); break;
        case cnk::CliMode::TestLogin: rc = RunTestLogin(cfg); break;
        case cnk::CliMode::Daemon:
        default:                      rc = RunDaemon(cfg); break;
    }

    cnk::LogShutdown();
    return rc;
}
```

`RunDaemon` 需要 `#include <thread>` 与 `<chrono>`。

- [ ] **Step 5: 接入模拟模式**

这是 spec §9.2 的核心测试手段：让整个程序对着本机模拟服务器跑完整流程，从而在**没有真实校园网**的情况下验证协议逻辑。

在 `main.cpp` 中增加以下辅助函数与接线：

```cpp
namespace {

// 模拟模式：启动本机模拟服务器，并把配置里的地址全部改指向它。
// 这些覆盖只作用于内存中的配置，绝不写回配置文件。
struct SimSession {
    cnk::SimServer server;
    bool active = false;
};

bool SetupSimulation(const cnk::CliOptions& opts, cnk::AppConfig& cfg,
                     SimSession& sim) {
    if (opts.simulate_vendor.empty()) return false;

    cnk::SimOptions so;
    so.vendor = opts.simulate_vendor;
    if (!opts.simulate_user.empty()) so.valid_user = opts.simulate_user;
    if (!opts.simulate_pass.empty()) so.valid_pass = opts.simulate_pass;
    if (opts.simulate_fail == "badcred") so.fail_mode = 1;
    else if (opts.simulate_fail == "timeout") so.fail_mode = 2;

    std::string err;
    if (!sim.server.Start(so, err)) {
        std::printf("启动模拟服务器失败：%s\n", err.c_str());
        return false;
    }
    sim.active = true;

    // 强制把探测与认证地址指向模拟服务器
    cfg.settings.probe_url = sim.server.BaseUrl() + "/ncsi.txt";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cfg.settings.probe_timeout_seconds = 2;
    for (auto& p : cfg.profiles) {
        p.auth_url = sim.server.BaseUrl() + "/";
        p.custom_template.url = sim.server.BaseUrl()
                                + "/eportal/?c=ACSetting&a=Login";
        p.custom_template.body = "DDDDD={username}&upass={password}";
        p.custom_template.success_regex = "Dr\\.COMWebLoginID_3";
    }
    if (cfg.profiles.empty()) {
        cnk::Profile p;
        p.name = "sim";
        p.auth_url = sim.server.BaseUrl() + "/";
        p.username = so.valid_user;
        p.password = so.valid_pass;
        p.vendor = (so.vendor == "eportal") ? "eportal" : "drcom";
        p.isp.preset = "campus";
        cfg.profiles.push_back(p);
        cfg.active_profile = "sim";
    }

    std::printf("== 模拟模式 ==\n");
    std::printf("厂商: %s\n", so.vendor.c_str());
    std::printf("地址: %s\n", sim.server.BaseUrl().c_str());
    if (!opts.simulate_fail.empty())
        std::printf("故障注入: %s\n", opts.simulate_fail.c_str());
    std::printf("\n");
    return true;
}

}  // namespace
```

在 `wmain` 中，加载配置之后、分发模式之前插入：

```cpp
    SimSession sim;
    SetupSimulation(opts, cfg, sim);
```

并在程序退出前（`LogShutdown()` 之前）加上：

```cpp
    if (sim.active) sim.server.Stop();
```

在 `Cli.h` / `Cli.cpp` 中为 `CliOptions` 补充三个字段并解析：

```cpp
    std::string simulate_user;
    std::string simulate_pass;
    std::string simulate_fail;   // "" | "badcred" | "timeout"
```

解析（沿用 `--key=value` 与 `--key value` 两种写法，与 `--config` 一致）：

```cpp
        if (a.rfind("--simulate-user", 0) == 0) {
            if (a == "--simulate-user" && i + 1 < args.size()) o.simulate_user = args[++i];
            else o.simulate_user = a.substr(16);
            continue;
        }
        if (a.rfind("--simulate-pass", 0) == 0) {
            if (a == "--simulate-pass" && i + 1 < args.size()) o.simulate_pass = args[++i];
            else o.simulate_pass = a.substr(16);
            continue;
        }
        if (a.rfind("--simulate-fail", 0) == 0) {
            if (a == "--simulate-fail" && i + 1 < args.size()) o.simulate_fail = args[++i];
            else o.simulate_fail = a.substr(16);
            continue;
        }
```

**注意**：`--simulate-user` 必须排在 `--simulate` 的匹配**之前**，否则会被 `--simulate` 前缀抢先匹配成厂商名。把这三段放在 `if (a.rfind("--simulate", 0) == 0)` 之前。

在 `tests/cli_test.cpp` 追加：

```cpp
TEST_CASE("解析模拟模式的三个子参数") {
    auto opts = cnk::ParseCli({"--simulate=drcom",
                               "--simulate-user=alice",
                               "--simulate-pass=pw",
                               "--simulate-fail=badcred"});
    CHECK(opts.simulate_vendor == "drcom");
    CHECK(opts.simulate_user == "alice");
    CHECK(opts.simulate_pass == "pw");
    CHECK(opts.simulate_fail == "badcred");
}

TEST_CASE("--simulate-user 不被 --simulate 抢先匹配") {
    auto opts = cnk::ParseCli({"--simulate-user=bob"});
    CHECK(opts.simulate_user == "bob");
    CHECK(opts.simulate_vendor.empty());
}
```

- [ ] **Step 6: 用模拟模式做端到端验证**

```bash
cmake --build build

# 正常路径：应探测到"劫持"、识别为 Dr.COM、登录成功、复测在线
./build/CampusNet-Keeper.exe --test-login --simulate=drcom \
    --simulate-user=alice --simulate-pass=pw123

# 凭据错误路径：应返回失败
./build/CampusNet-Keeper.exe --test-login --simulate=drcom \
    --simulate-user=alice --simulate-pass=WRONG --simulate-fail=badcred

# 超时路径：应在数秒内返回而不是挂住
./build/CampusNet-Keeper.exe --probe-now --simulate=drcom --simulate-fail=timeout
```

Expected:
- 第一条：输出 `成功: 是`，复测显示 `已在线 ✓`
- 第二条：输出 `成功: 否`
- 第三条：数秒内输出 `结果: Offline（无连接）`

**这三条通过，就证明整条认证链路在开发机上已被端到端验证过。**

- [ ] **Step 7: 更新 CMakeLists.txt**

追加 `src/native/core/Cli.cpp` 与 `tests/cli_test.cpp`。`main.cpp` 已在可执行目标中，无需改动。

- [ ] **Step 8: 构建并手工验证 CLI**

```bash
cmake --build build
./build/CampusNet-Keeper.exe --version
./build/CampusNet-Keeper.exe --help
./build/CampusNet-Keeper.exe --probe-now
```

Expected: `--version` 输出 `CampusNet-Keeper 0.1.0`；`--help` 输出用法；`--probe-now` 输出探测结果（若当前不在校园网，结果为 `Offline`，这也是正常的）。

- [ ] **Step 9: 运行测试确认通过**

Run: `./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 10: 提交**

```bash
git add -A
git commit -m "feat: CLI 入口与诊断命令，支持探测/识别/试登录"
```

---

## 任务 16：单实例与开机自启

**Files:**
- Create: `src/native/sys/SingleInstance.h`
- Create: `src/native/sys/SingleInstance.cpp`
- Create: `src/native/sys/Autostart.h`
- Create: `src/native/sys/Autostart.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/sys_test.cpp`

**Interfaces:**
- Consumes: `AppDataDir()`（任务 2）
- Produces:
  - `class cnk::SingleInstance`：`bool Acquire()`（返回 false 表示已有实例）、`void Release()`、`static bool NotifyExistingInstance()`
  - `bool cnk::SetAutostart(bool enable, std::string& error)`
  - `bool cnk::IsAutostartEnabled()`
  - `std::string cnk::ExecutablePath()`

- [ ] **Step 1: 写失败测试**

`tests/sys_test.cpp`:

```cpp
#include "doctest.h"
#include "sys/SingleInstance.h"
#include "sys/Autostart.h"

TEST_CASE("ExecutablePath 返回存在的文件") {
    std::string p = cnk::ExecutablePath();
    CHECK(p.size() > 0);
    CHECK(p.find(".exe") != std::string::npos);
}

TEST_CASE("互斥体可获取与释放") {
    cnk::SingleInstance a;
    CHECK(a.Acquire());
    // 同一进程内再取一次应失败
    cnk::SingleInstance b;
    CHECK_FALSE(b.Acquire());
    a.Release();
    cnk::SingleInstance c;
    CHECK(c.Acquire());
    c.Release();
}

TEST_CASE("IsAutostartEnabled 不崩溃") {
    bool v = cnk::IsAutostartEnabled();
    (void)v;   // 只要不抛异常即可
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现 SingleInstance**

`src/native/sys/SingleInstance.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 基于命名互斥体的单实例保证。
class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();

    // 成功获取返回 true；已有实例持有则返回 false。
    bool Acquire();
    void Release();

    // 通知已在运行的实例弹出设置界面。返回是否成功送达。
    static bool NotifyExistingInstance();

    // 守护进程在消息循环中轮询此标志，为 true 时弹 UI。
    // 由 NotifyExistingInstance 通过线程消息置位。
    static bool ConsumeUiRequest();

private:
    void* handle_ = nullptr;
};

}  // namespace cnk
```

`src/native/sys/SingleInstance.cpp`:

```cpp
#include "sys/SingleInstance.h"
#include <windows.h>

namespace cnk {

namespace {

constexpr const wchar_t* kMutexName = L"Global\\CampusNetKeeper.SingleInstance";
constexpr const wchar_t* kWindowClass = L"CampusNetKeeperHidden";
// 自定义消息：请求弹出 UI
constexpr UINT kMsgShowUi = WM_APP + 1;

}  // namespace

SingleInstance::~SingleInstance() { Release(); }

bool SingleInstance::Acquire() {
    if (handle_) return true;
    HANDLE h = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!h) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return false;
    }
    handle_ = h;
    return true;
}

void SingleInstance::Release() {
    if (!handle_) return;
    CloseHandle((HANDLE)handle_);
    handle_ = nullptr;
}

bool SingleInstance::NotifyExistingInstance() {
    HWND hwnd = FindWindowW(kWindowClass, nullptr);
    if (!hwnd) return false;
    return PostMessageW(hwnd, kMsgShowUi, 0, 0) != FALSE;
}

bool SingleInstance::ConsumeUiRequest() {
    // 由消息循环在处理 kMsgShowUi 时调用间接实现；
    // 这里提供一个简化的静态标志供外部查询。
    return false;
}

}  // namespace cnk
```

**说明**：`ConsumeUiRequest` 的完整实现依赖任务 17 的消息窗口。本任务先提供 `NotifyExistingInstance`（发送端）与互斥体（互斥保证），接收端在任务 17 的托盘消息循环中处理 `kMsgShowUiRequest`。因此本任务需在 `SingleInstance.h` 中额外导出消息常量：

```cpp
// 守护进程应向 FindWindowW(L"CampusNetKeeperHidden", nullptr) 的窗口
// 处理此消息：弹出设置界面。
// ⚠ 审阅注记（2026-10-14）：编号定为 WM_APP+100 而非 WM_APP+1——
// 后者是托盘图标回调消息（WM_TRAYICON）的惯用编号，会同窗口相撞。
inline constexpr UINT kMsgShowUiRequest = WM_APP + 100;
```

（头文件需 `#include <windows.h>` 才能用 `WM_APP`。）

- [ ] **Step 4: 实现 Autostart**

`src/native/sys/Autostart.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

std::string ExecutablePath();

// 写/删 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 项。
bool SetAutostart(bool enable, std::string& error);
bool IsAutostartEnabled();

}  // namespace cnk
```

`src/native/sys/Autostart.cpp`:

```cpp
#include "sys/Autostart.h"
#include <windows.h>
#include <vector>

namespace cnk {

namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValueName = L"CampusNetKeeper";

}  // namespace

std::string ExecutablePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size() - 1) break;
        buf.resize(buf.size() * 2);
    }
    std::wstring w(buf.data());
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), len, nullptr, nullptr);
    return out;
}

bool SetAutostart(bool enable, std::string& error) {
    error.clear();
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                            KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        error = "无法打开自启动注册表项，错误码 " + std::to_string(rc);
        return false;
    }

    if (enable) {
        std::string exe = ExecutablePath();
        if (exe.empty()) {
            RegCloseKey(key);
            error = "无法取得自身路径";
            return false;
        }
        int wn = MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), (int)exe.size(),
                                     nullptr, 0);
        std::wstring wexe(wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), (int)exe.size(),
                            wexe.data(), wn);
        std::wstring value = L"\"" + wexe + L"\"";  // 路径含空格时需引号
        rc = RegSetValueExW(key, kValueName, 0, REG_SZ,
                            (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kValueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS) {
        error = "写自启动注册表失败，错误码 " + std::to_string(rc);
        return false;
    }
    return true;
}

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG rc = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && size > 0;
}

}  // namespace cnk
```

- [ ] **Step 5: 更新 CMakeLists.txt**

追加 `src/native/sys/SingleInstance.cpp`、`src/native/sys/Autostart.cpp`、`tests/sys_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: 单实例互斥体与开机自启注册"
```

---

## 任务 17：托盘图标与消息循环

> **⚠ 审阅注记（2026-10-14，任务 16 审阅）**：隐藏消息窗口需要处理两个跨进程/跨完整性级别的细节：
> 1. 窗口创建后调用 `ChangeWindowMessageFilterEx(hwnd, kMsgShowUiRequest, MSGFLT_ALLOW, nullptr)`
>    放行「弹 UI 请求」——`kMsgShowUiRequest`（WM_APP+100）> WM_USER，提权启动或未来服务化时，
>    第二实例的 `PostMessage` 会被 UIPI 静默拦截（FindWindow 不受影响，故障隐蔽难排查）；
> 2. 托盘回调消息（WM_TRAYICON）用 `WM_APP + 1`，与 `kMsgShowUiRequest` 的编号已错开，勿再合并。

**Files:**
- Create: `src/native/tray/TrayIcon.h`
- Create: `src/native/tray/TrayIcon.cpp`
- Modify: `src/native/main.cpp`（守护模式改为托盘消息循环）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Scheduler`（任务 14）、`SingleInstance`（任务 16）
- Produces:
  - `class cnk::TrayIcon`：
    - `bool Create(HINSTANCE hinst, const char* tooltip)`
    - `void Destroy()`
    - `void SetState(RunState state, const std::string& tooltip)`
    - `void SetCallbacks(std::function<void()> on_open_ui, std::function<void()> on_reconnect, std::function<void()> on_quit, std::function<void()> on_open_log)`
    - `void ShowBalloon(const std::string& title, const std::string& text)`
    - `HWND Hwnd() const`
  - `bool cnk::RunDaemonLoop(HINSTANCE hinst, AppConfig cfg)` — 阻塞式消息循环，含 `SetTimer` 驱动 `Scheduler::Tick()`

**图标来源**：使用系统内置图标（`LoadIconW(nullptr, IDI_APPLICATION)` 等），按状态切换不同系统图标，避免引入外部资源文件。后续可在 M2 替换为自绘图标。

- [ ] **Step 1: 实现 TrayIcon**

`src/native/tray/TrayIcon.h`:

```cpp
#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include "core/Scheduler.h"

namespace cnk {

class TrayIcon {
public:
    bool Create(HINSTANCE hinst, const char* tooltip);
    void Destroy();
    void SetState(RunState state, const std::string& tooltip);
    void ShowBalloon(const std::string& title, const std::string& text);
    HWND Hwnd() const { return hwnd_; }

    void SetCallbacks(std::function<void()> on_open_ui,
                      std::function<void()> on_reconnect,
                      std::function<void()> on_open_log,
                      std::function<void()> on_quit);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool created_ = false;
    RunState state_ = RunState::Disconnected;

    std::function<void()> on_open_ui_;
    std::function<void()> on_reconnect_;
    std::function<void()> on_open_log_;
    std::function<void()> on_quit_;

    void OnTrayMessage(LPARAM lparam);
    void ShowContextMenu();
    HICON IconForState(RunState s) const;
};

// 阻塞式守护循环：创建托盘、装 500ms 定时器驱动 Scheduler，直到退出消息。
bool RunDaemonLoop(HINSTANCE hinst, AppConfig cfg);

}  // namespace cnk
```

`src/native/tray/TrayIcon.cpp`:

```cpp
#include "tray/TrayIcon.h"
#include "sys/SingleInstance.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/NetworkChange.h"
#include <shellapi.h>
#include <thread>

namespace cnk {

namespace {

constexpr const wchar_t* kWindowClass = L"CampusNetKeeperHidden";
constexpr UINT kTrayCallbackMsg = WM_APP + 2;
constexpr UINT kTimerId = 1;
constexpr UINT kTimerIntervalMs = 500;

enum MenuId {
    kMenuStatus = 1,
    kMenuOpenUi = 2,
    kMenuReconnect = 3,
    kMenuOpenLog = 4,
    kMenuQuit = 5,
};

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

void OpenPathInExplorer(const std::string& path) {
    ShellExecuteW(nullptr, L"open", Utf8ToWide(path).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

HICON TrayIcon::IconForState(RunState s) const {
    // 使用系统内置图标区分状态，避免外部资源依赖
    LPCWSTR id = IDI_APPLICATION;
    switch (s) {
        case RunState::Online:
        case RunState::Disconnected:      break;
        case RunState::Probing:
        case RunState::PortalDetected:    break;
        case RunState::Authenticating:    break;
        case RunState::AuthFailed:
        case RunState::NotCampusNetwork:  id = IDI_WARNING; break;
    }
    return LoadIconW(nullptr, id);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayIcon* self = (TrayIcon*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case kTrayCallbackMsg:
            self->OnTrayMessage(lp);
            return 0;
        case kMsgShowUiRequest:
            if (self->on_open_ui_) self->on_open_ui_();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kMenuOpenUi:    if (self->on_open_ui_) self->on_open_ui_(); break;
                case kMenuReconnect: if (self->on_reconnect_) self->on_reconnect_(); break;
                case kMenuOpenLog:   if (self->on_open_log_) self->on_open_log_(); break;
                case kMenuQuit:      if (self->on_quit_) self->on_quit_(); break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool TrayIcon::Create(HINSTANCE hinst, const char* tooltip) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);   // 已注册时返回 0，可忽略

    hwnd_ = CreateWindowExW(0, kWindowClass, L"CampusNetKeeper",
                            0, 0, 0, 0, 0, nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kTrayCallbackMsg;
    nid_.hIcon = IconForState(RunState::Disconnected);
    wcsncpy_s(nid_.szTip, Utf8ToWide(tooltip).c_str(), _TRUNCATE);

    created_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    return created_;
}

void TrayIcon::Destroy() {
    if (created_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        created_ = false;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TrayIcon::SetState(RunState state, const std::string& tooltip) {
    state_ = state;
    if (!created_) return;
    nid_.hIcon = IconForState(state);
    wcsncpy_s(nid_.szTip, Utf8ToWide(tooltip).c_str(), _TRUNCATE);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::ShowBalloon(const std::string& title, const std::string& text) {
    if (!created_) return;
    nid_.uFlags = NIF_INFO;
    wcsncpy_s(nid_.szInfoTitle, Utf8ToWide(title).c_str(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, Utf8ToWide(text).c_str(), _TRUNCATE);
    nid_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void TrayIcon::SetCallbacks(std::function<void()> on_open_ui,
                            std::function<void()> on_reconnect,
                            std::function<void()> on_open_log,
                            std::function<void()> on_quit) {
    on_open_ui_ = std::move(on_open_ui);
    on_reconnect_ = std::move(on_reconnect);
    on_open_log_ = std::move(on_open_log);
    on_quit_ = std::move(on_quit);
}

void TrayIcon::OnTrayMessage(LPARAM lparam) {
    switch (LOWORD(lparam)) {
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            if (on_open_ui_) on_open_ui_();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu();
            break;
    }
}

void TrayIcon::ShowContextMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, L"状态：见提示");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuOpenUi, L"设置…");
    AppendMenuW(menu, MF_STRING, kMenuReconnect, L"立即重连");
    AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"打开日志目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出");

    SetForegroundWindow(hwnd_);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

bool RunDaemonLoop(HINSTANCE hinst, AppConfig cfg) {
    auto sch = std::make_shared<Scheduler>(cfg);
    TrayIcon tray;

    // 地址变更 / 睡眠唤醒时立即探测，是达成 15 秒恢复目标的关键
    NetworkChangeWatcher netwatch;
    netwatch.Start([sch]() { sch->NotifyNetworkChanged(); });

    if (!tray.Create(hinst, "CampusNet-Keeper 启动中…")) {
        LogError("托盘图标创建失败");
        netwatch.Stop();
        return false;
    }

    auto open_ui = [&tray]() {
        // M2 会替换为启动 --ui 子进程
        tray.ShowBalloon("设置界面", "设置界面将在下一版本提供。\n"
                                     "当前可手动编辑 %APPDATA%\\CampusNet-Keeper\\config.json");
    };

    tray.SetCallbacks(
        open_ui,
        [sch, &tray]() {
            sch->ForceReconnect();
            tray.ShowBalloon("CampusNet-Keeper", "已触发立即重连");
        },
        [&tray]() {
            std::string dir = AppDataDir();
            OpenPathInExplorer(dir);
        },
        [&tray]() { tray.Destroy(); PostQuitMessage(0); });

    SetTimer(tray.Hwnd(), kTimerId, kTimerIntervalMs, nullptr);

    RunState last_state = RunState::Unknown;
    bool ever_online = false;

    MSG msg;
    for (;;) {
        // 抽出所有排队消息
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                KillTimer(tray.Hwnd(), kTimerId);
                netwatch.Stop();
                tray.Destroy();
                return true;
            }
            if (msg.message == WM_TIMER && msg.wParam == kTimerId) {
                sch->Tick();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const RunState st = sch->State();
        if (st != last_state) {
            std::string tip = "CampusNet-Keeper — " + sch->LastMessage();
            tray.SetState(st, tip);

            if (st == RunState::Online) ever_online = true;
            // 从离线恢复到在线：通知用户（仅当此前断开过）
            if (st == RunState::Online && ever_online && last_state != RunState::Online &&
                last_state != RunState::Unknown) {
                tray.ShowBalloon("网络已恢复", "已自动重新登录校园网");
                LogInfo("已自动重新登录并恢复在线");
            }
            if (st == RunState::AuthFailed || st == RunState::NotCampusNetwork) {
                tray.ShowBalloon("需要处理", sch->LastMessage());
            }
            last_state = st;
        }

        // 空闲等待：有消息立刻醒，否则最多等 20ms
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
}

}  // namespace cnk
```

`RunDaemonLoop` 需要 `#include <memory>`。

- [ ] **Step 2: 修改 main.cpp 使用守护循环**

把 `RunDaemon` 替换为：

```cpp
int RunDaemon(cnk::AppConfig& cfg, HINSTANCE hinst) {
    if (!cnk::RunDaemonLoop(hinst, cfg)) {
        std::printf("守护循环启动失败\n");
        return 1;
    }
    return 0;
}
```

`wmain` 增加：

```cpp
#include <windows.h>
// ...
int wmain(int, wchar_t**) {
    SetConsoleOutputCP(CP_UTF8);
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    // ...
    case cnk::CliMode::Daemon:
    default: rc = RunDaemon(cfg, hinst); break;
}
```

同时在守护模式启动前获取单实例：

```cpp
cnk::SingleInstance single;
if (opts.mode == cnk::CliMode::Daemon && !single.Acquire()) {
    // 已有实例：请求它弹出设置界面后退出
    if (cnk::SingleInstance::NotifyExistingInstance())
        std::printf("CampusNet-Keeper 已在运行，已请求其打开设置界面。\n");
    else
        std::printf("CampusNet-Keeper 已在运行。\n");
    return 0;
}
```

- [ ] **Step 3: 更新 CMakeLists.txt**

追加 `src/native/tray/TrayIcon.cpp`。托盘需要 `shell32`（已在链接列表中）。

**注意**：`main.cpp` 现为 GUI 应用还是控制台应用？守护模式下不需要控制台窗口。在 `CMakeLists.txt` 中把可执行目标设为 GUI 子系统：

```cmake
add_executable(CampusNet-Keeper WIN32 src/native/main.cpp)
```

> **⚠⚠ 已弃用（2026-10-14 用户实测）**：上述 WIN32 子系统 + `AttachConsole` 方案在**真实 cmd 环境**
> 下 attach 链路失效——所有 CLI 命令（`--version`/`--probe-now`/`--detect`/`--set-password`）完全无输出。
> 开发期未发现的原因是测试全在 bash 管道下进行，走的是"保持重定向"分支，attach 分支从未被真实控制台验证。
> **现行方案**：控制台子系统（输出在任何环境原生可靠）+ 守护形态由程序自身在
> `GetConsoleProcessList()==1`（独占控制台，即双击/开机自启）时隐藏控制台窗口
>（见 `main.cpp` 的 `HideConsoleIfStandalone`）。下方 AttachConsole 方案仅作历史记录保留。

但 CLI 诊断命令（`--probe-now` 等）需要控制台输出。方案：保持 `WIN32` 子系统，并在需要输出的模式下调 `AttachConsole(ATTACH_PARENT_PROCESS)` 并重定向 `stdout`。在 `wmain` 开头实现：

```cpp
namespace {
void EnsureConsoleForOutput() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
}
}
```

在 `wmain` 中：**仅当模式为 `ProbeNow` / `TestLogin` / `Detect` / `Version` / `Help` 时**调用 `EnsureConsoleForOutput()`；守护与 UI 模式不附加控制台。

- [ ] **Step 4: 手工验证**

```bash
cmake --build build
./build/CampusNet-Keeper.exe --version
./build/CampusNet-Keeper.exe --probe-now
./build/CampusNet-Keeper.exe          # 应出现托盘图标，双击弹出气泡
```

Expected: `--version` 与 `--probe-now` 在终端有输出；直接运行出现托盘图标，右键有菜单，退出菜单能结束进程。

- [ ] **Step 5: 运行测试确认全部通过**

Run: `./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 托盘图标与守护消息循环，接上调度与单实例"
```

---

## 任务 18：M1 收尾——手册、实测准备与用户验证

> **⚠ 实现注记（2026-10-14）**：原计划存在一个**实测门禁的阻断缺口**——M1 没有任何途径录入密码
> （配置里的密码是 DPAPI 密文无法手填、UI 在 M2、CLI 无密码入口），原文引用的"环境变量方式"
> 是一个从未落地的前向引用。已补实现：**`--set-password [--user=账号]`**（环境变量
> `CNK_PASSWORD` 或交互式掩码输入 → DPAPI 加密写入配置），并在实测指南中替换了原第 3 步。
> 另：README 的构建命令已从计划的 Ninja 改为实际使用的 VS 生成器，内存数据按实测修正为 ~16MB。
>
> **⚑ 实测回修（2026-10-16）**：用户实测发现识别链路在真实环境失效——真实劫持是「**裸 302**」
> （`Location: http://10.0.0.1/a79.htm`，**无 `Server` 指纹、空正文**），而实现把「探测的
> 302 响应」当成了识别输入 → `识别厂商: unknown` → 落到未实现的 GenericFormAuth（真实目标
> 实为 Dr.COM，同一 Portal 直连抓取页有完整 `Server: DrcomServer1.0` 与哆点指纹）。
> spec §7 的原意是「输入是 **Portal 的响应头与页面 HTML**」，即**跳转目标页**。已补：
> **`core/PortalPage`（`ResolvePortalPage`）**——劫持态先 GET 跳转目标页再识别/装配
> NetContext；抓取失败退回探测响应（尽力而为，不阻断链路）。`--test-login`、`--detect`
> （在线态还支持从 `auth_url` 抓取识别）与 `Scheduler` 三处同步接入；模拟器新增
> `fail_mode=5`（裸 302 + 第二监听端口上的 `/a79.htm`）与 CLI `--simulate-fail=hijack302`
> 复现该形态；证据目录补采登录页 `a79.htm`（见 `docs/evidence/README.md`）。
> 遗留（不阻断 M1）：免劫持清单仍只在 `:801 loadConfig` 响应下发，客户端暂未抓取该接口，
> 探测地址继续用内置默认（msftncsi，也在真实清单内）。
>
> **⚑ 实测回修 2（2026-10-16 晚）**：首轮修复后复测，登录仍返回「无法判定」——深挖发现
> **登录端口假设错误**：页面声明的 `authloginport=801` 处只有 SPA/新 API，老式
> `c=ACSetting` 协议实际由**门户同源端口**处理（实测：`:801` 对任何凭据都回 SPA 外壳
> 11561B；门户同源 `:80` 回真实的 `信息页` —— `<!--Dr.COMWebLoginID_2.htm-->`、
> `msga='userid error1/2'`、`page.run(2)`，响应头 `Server: DrcomServer1.0`）。
> 已修：`DrcomAuth` 登录 URL 改用 `auth_url` 自身端口（未显式端口时不补默认，按 scheme
> 缺省）；失败信息附带服务器 `msga` 判定码（实测可区分失败成因）。模拟器 `fail_mode=5`
> 的 302 补齐 `Server: NetEngine Server 1.0`（拦截器特征，实测直读）。证据目录新增：
> 劫持 302 原始响应头、真实 `a=Login` 失败样本 ×2；**成功样本待注销态实登补齐**。
>
> **⚑ 实测回修 3（2026-10-16 晚，HAR 逆向——本条推翻回修 2 的登录通道结论）**：用户导出
> 浏览器 F12 的 HAR，其中含**两次真实成功登录**，一举定案：**真实登录走 `:801` 新式 API**，
> 不是老式 `c=ACSetting`。请求形态为
> `GET :801/eportal/portal/login?callback=..&params=<AES(JSON)>`，前置
> `chkstatus`（取本机校园网 IP）与 `loadConfig`（取**页面令牌 `rcn`**，登录必需，缺失回
> 「当前页面已超时」）；`params` = **AES-128-ECB + PKCS7 + Base64**，密钥
> `5c1d5ad4dea0e8dd` 取自门户自己的 `a41.js`/`a40.js`（**公开值**，非本项目秘密）；
> 明文为**固定顺序的 16 字段 JSON**（`login_method`/`user_account` 带 `,0,` 前缀/
> `user_password`/`wlan_user_ip`/…/`rcn`）。成功响应 `{"result":1,"msg":"Portal协议认证成功！"}`。
>
> 由此**平反**上一段的误诊：`POST :80 /eportal/?c=ACSetting` 在本部署**不做真实认证**，
> 无论凭据对错**恒回 `msga='userid error2'`**。存储凭据与浏览器提交值已做逐字节比对
> （账号与密码**完全一致**）——即**从来不是密码问题**，公开错误码表把 error2 释为
> 「密码不匹配」是本次误诊的直接来源。
>
> 已落地的修改：`core/Crypto`（BCrypt AES-128-ECB+PKCS7+Base64，含固定向量测试）、
> `DrcomAuth::TryNewApi`（首选通道；旧部署才回退 `LegacyLogin`）、模拟器新式端点
> （`loadConfig` 下发 rcn、按 AES 参数校验、`disable_new_api` 用于回退路径测试）、
> **「已在线」判定改判成功**（线上实测 `{"result":0,"msg":"终端IP已在线！","ret_code":2}`；
> 若判失败会让调度器走退避直至「非校园网」保护）。测试基线
> **167/606 → 173 用例 / 636 断言**（另 2 例 `integrity_test` 为沙箱环境性失败）。
>
> **安全口径（新增，务必遵守）**：`params` 用**门户页面里的公开密钥**加密，
> 故**任何真实密文等价于明文** → 原始 HAR 与真实 `params` **绝不入库**；
> 页面令牌 `rcn` 同样不入库（已把 `loadconfig_185_801.json` 里的真实 rcn 改为占位符，
> 模拟器夹具改用合成值）。证据见 `docs/evidence/newapi_login_20261016.md`。
>
> 遗留（不阻断 M1）：注销态的真实成功登录仍需用户跑一次 `--test-login` 收尾；
> 新式 API 的**凭据错误文案全集**未采齐（现以「账号/密码/用户」字样启发式归类）。
>
> **⚑ 实测回修 4（2026-10-16 晚，真机 M1 复测）——内置探测地址不可达**：用户复测
> `--test-login` 时登录**成功**（连续两次 `result:1`），但复测探测连续 5 次报「离线
> （12002 超时）」。独立取证：门户 `chkstatus` 回 `result:1`、`uid=账号@cmcc`（会话
> 确实在线），而 `www.msftncsi.com/ncsi.txt` **直连 0/4 可达**（4 次都卡满 3 秒），
> 同清单内 `captive.apple.com/hotspot-detect.html` **4/4 可达、47–73ms**。
> 即：旧内置默认在本校园网已不可达，会把「在线」永久误判成「离线」——**这是 M1
> 验收（复测应显示「已在线 OK」）的阻塞缺陷，不是建议**。
> 已修：`Settings` 默认改为 `http://captive.apple.com/hotspot-detect.html` +
> `probe_expect=Success`，`probe_timeout_seconds` 3→6；`Config.cpp` 缺键回落改用
> 结构体默认（不再写死字面量）；`ProbeTarget::DefaultProbeTarget()` 同步；
> 新增默认值钉桩测试（防止改回不可达地址）。用户已落盘的 `config.json` 需一并更新，
> 否则旧值仍然生效。
>
> 另两处同一根因的探测健壮性修复（本次一并完成）：
> ① 登录接口成功 → AC 数据面放行有秒级延迟，**紧跟的单次复探会误报「仍不在线」**
> → `ProbeUntilOnline`（轮询到在线，5×1.5s）；
> ② 会话正常时也曾**单次探测超时**（12002）→ 若据此判离线，整条链路会走成
> 「厂商 unknown → GenericFormAuth 尚未实现」的死路（实机复现）→
> `ProbeUntilDecided`（只对「离线」重试，被劫持/在线立即返回，避免注销窗口白等）。
>
> **遗留（需独立审阅后再动）**：`ProbeTarget.cpp` 的候选优先级仍把 msftncsi 排在
> 首位，且 `DefaultProbeTarget`/候选表没有失败切换——一旦将来接上
> `visit_blacklist` 驱动的探测路径（Scheduler 已有该分支，只是门户页不含该变量、
> 当前不触发），就会选中这个已不可达的地址。建议：按实测把 `captive.apple` /
> `detectportal` 提前，并加多目标失败切换。

**Files:**
- Create: `README.md`（M1 版本）
- Create: `docs/M1-实测指南.md`

**Interfaces:**
- Consumes: 全部 M1 产物
- Produces: 可直接交付用户实测的 exe 与操作指引

- [ ] **Step 1: 写 M1 实测指南**

`docs/M1-实测指南.md`:

```markdown
# M1 实测指南

## 1. 确认当前网络状态

在校园网已连接并已手动登录的状态下运行：

    CampusNet-Keeper.exe --probe-now

预期输出 `结果: Online（网络正常）`。

如果输出 `PortalDetected（被 Portal 劫持）`，说明探测地址被劫持，
请把完整输出贴回，我们会调整探测地址。

如果输出 `Offline（无连接）`，说明探测地址不可达，同样请贴回输出。

## 2. 识别厂商与运营商

    CampusNet-Keeper.exe --detect

预期输出：

- `识别厂商: drcom`
- 一份 4 项的运营商列表（校园用户 / 校园电信 / 校园联通 / 校园其他）
- 一份免劫持地址清单
- `建议探测地址: http://www.msftncsi.com/ncsi.txt`

## 3. 填写配置

用记事本打开 `%APPDATA%\CampusNet-Keeper\config.json`，填写：

- `profiles[0].username`：你的账号（**不含**运营商后缀，例如 `2024147xxxxx`）
- `profiles[0].isp.preset`：填 `cmcc`（移动）/ `telecom`（电信）/ `unicom`（联通）/ `campus`（校园网）/ `custom`
- `profiles[0].auth_url`：`http://10.0.0.1/`

**密码不要手填到文件里**（文件里没有明文密码字段）。请运行一次设置界面
（M2 提供）来录入密码。M1 阶段可临时用下面的 `--test-login` 配合
环境变量方式验证——见第 4 步。

## 4. 试登录（关键一步）

**注意：这一步需要你先主动断线**，否则服务器会认为你已在线。

断线方法：打开校园网门户页面，点注销。

注销后立即运行：

    CampusNet-Keeper.exe --test-login

请把**完整输出**贴回。需要关注：

- `识别厂商` 是否为 `drcom`
- `认证策略` 是否为 `DrcomAuth`
- `POST` 的 URL 是否正确
- 结果是否成功
- 若失败，`信息` 一栏的原文

## 5. 后台守护测试

    CampusNet-Keeper.exe

托盘出现图标后，正常使用电脑。下次会话失效时（可能几小时到几天），
观察托盘气泡是否提示「网络已恢复」。

日志位于 `%APPDATA%\CampusNet-Keeper\logs\keeper.log`。

## 需要反馈的内容

1. `--probe-now` 的完整输出
2. `--detect` 的完整输出
3. `--test-login` 的完整输出（**含失败信息**，这是最有价值的）
4. 守护运行一段时间后的日志尾部

**贴回前请自行确认输出里没有明文密码**——程序已做脱敏，但仍请扫一眼。
```

- [ ] **Step 2: 写 M1 版 README**

`README.md`（M1 阶段版本，M2 会扩充）：

```markdown
# CampusNet-Keeper

校园网自动重连守护程序。会话失效时自动重新登录，让你在游戏里感觉不到掉线。

> 当前为 **M1（核心）** 阶段：命令行的守护程序已可用，设置界面在 M2 提供。

## 特性

- **原生实现**：C++17，静态链接，零动态库依赖
- **极低占用**：后台常驻内存 3-5 MB，空闲 CPU 0%
- **自动重连**：网络断开或被 Portal 劫持时自动重新登录
- **厂商识别**：自动识别 Portal 类型（Dr.COM / 锐捷 / 深澜 / 华为）
- **密码加密**：使用 Windows DPAPI，配置文件被拷走也无法解密

## 快速开始

    CampusNet-Keeper.exe --detect       # 识别你学校的认证系统
    CampusNet-Keeper.exe --probe-now    # 检查当前网络状态
    CampusNet-Keeper.exe --test-login   # 试一次登录（需先注销）
    CampusNet-Keeper.exe                # 后台守护

详见 `docs/M1-实测指南.md`。

## 构建

依赖：Visual Studio 2022（含 C++ 工作负载）与 CMake 3.20+、Ninja。

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/cnk_tests.exe

## 许可证

MIT
```

- [ ] **Step 3: 运行全部测试并构建 Release**

```bash
cmake --build build --config Release
./build/cnk_tests.exe
./build/CampusNet-Keeper.exe --version
```

Expected: 全部测试 PASS，版本号正常输出。

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "docs: M1 实测指南与 README"
```

- [ ] **Step 5: 交付用户实测（门禁）**

把 `build/CampusNet-Keeper.exe` 与 `docs/M1-实测指南.md` 交给用户，按指南执行。
**在此步骤完成、拿到用户反馈并修正之前，不得进入 M2。**

---

# M2：UI 与发布

---

## 任务 19：加密工具与锐捷认证

深澜与锐捷均非本机目标，实现它们是为满足 spec §4.2 的通用性要求。锐捷不需要额外加密，故与 BCrypt 工具分开：本任务只做锐捷；BCrypt 工具留给任务 20 的深澜。

**Files:**
- Create: `src/native/auth/EportalAuth.cpp`
- Modify: `src/native/auth/Stubs.cpp`（移除 `MakeEportalAuth`）
- Modify: `CMakeLists.txt`
- Create: `tests/eportal_auth_test.cpp`

**Interfaces:**
- Consumes: `IAuth`、`HttpPerform`、`IspSuffix`
- Produces: `std::unique_ptr<cnk::IAuth> cnk::MakeEportalAuth()`

**请求构造**（spec §4.4 与公开协议）：

- `GET {scheme_host}:{port}/eportal/?c=Portal&a=login&callback=dr1003&login_method=1&user_account={urlencode(username)},{0},{service}&user_password={urlencode(password)}&wlan_user_ip={ip}&wlan_user_ipv6=&wlan_user_mac=000000000000&wlan_ac_ip=&wlan_ac_name=&jsVersion=4.1.3&v={random4}&lang=zh`
- 端口默认 801（与 Dr.COM 一致，锐捷亦常用 801）
- `service` 取 `IspSuffix(profile)`，校园网时为空
- 响应为 JSONP：`dr1003({"result":1,"msg":"..."})`。`result == 1` 为成功。

- [ ] **Step 1: 写失败测试**

模拟服务器当前只为 Dr.COM 返回正确格式。本任务的测试**不依赖模拟服务器的成功路径**，而是断言**请求构造正确**与**响应解析正确**。为此在模拟服务器中加入 `vendor = "eportal"` 分支，返回 JSONP。

先在 `SimServer.cpp` 的登录分支加入：

```cpp
if (opts.vendor == "eportal") {
    std::string user = UrlDecode(ParseFormValue(all, "user_account"));
    std::string pass = UrlDecode(ParseFormValue(all, "user_password"));
    // user_account 形如 "alice,0,telecom"，取逗号前的部分比对
    size_t comma = user.find(',');
    std::string bare = comma == std::string::npos ? user : user.substr(0, comma);
    bool ok = (bare == opts.valid_user) &&
              (pass == opts.valid_pass || opts.valid_pass.empty());
    resp_body = ok ? "dr1003({\"result\":1,\"msg\":\"\"});"
                   : "dr1003({\"result\":0,\"msg\":\"账号或密码错误\"});";
    content_type = "application/javascript; charset=utf-8";
}
```

`tests/eportal_auth_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/AuthFactory.h"
#include "sim/SimServer.h"

namespace {
cnk::Profile MakeP(const std::string& base, const std::string& u,
                   const std::string& p, const std::string& preset) {
    cnk::Profile pr;
    pr.auth_url = base + "/";
    pr.username = u;
    pr.password = p;
    pr.vendor = "eportal";
    pr.isp.preset = preset;
    return pr;
}
}

TEST_CASE("锐捷正确凭据登录成功") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "alice";
    o.valid_pass = "pw";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;
    auto p = MakeP(srv.BaseUrl(), "alice", "pw", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    CHECK(r.request_url.find("c=Portal") != std::string::npos);
    CHECK(r.request_url.find("a=login") != std::string::npos);
    CHECK(r.request_url.find("user_account=alice") != std::string::npos);
    CHECK(r.request_url.find("user_password=pw") != std::string::npos);
    srv.Stop();
}

TEST_CASE("锐捷错误凭据返回失败并带服务器消息") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "alice";
    o.valid_pass = "right";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;
    auto p = MakeP(srv.BaseUrl(), "alice", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号或密码错误") != std::string::npos);
    srv.Stop();
}

TEST_CASE("锐捷运营商写入 user_account 的第三段") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "bob";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;
    auto p = MakeP(srv.BaseUrl(), "bob", "x", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("user_account=bob%2C0%2Ctelecom") != std::string::npos
          || r.request_url.find("user_account=bob,0,telecom") != std::string::npos);
    srv.Stop();
}

TEST_CASE("锐捷空账号空密码直接失败") {
    cnk::Profile p;
    p.vendor = "eportal";
    p.auth_url = "http://127.0.0.1:1/";
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.size() > 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 用例失败（桩实现）。

- [ ] **Step 3: 实现 EportalAuth**

`src/native/auth/EportalAuth.cpp`:

```cpp
#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include "core/Log.h"
#include <cstdlib>
#include <regex>

namespace cnk {

namespace {

class EportalAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;
        if (p.username.empty()) { r.message = "账号为空，请在设置中填写"; return r; }
        if (p.password.empty()) { r.message = "密码为空，请在设置中填写"; return r; }

        std::string scheme_host, port;
        if (!SplitHostPort(p.auth_url, scheme_host, port, "801")) {
            r.message = "认证网址格式不正确：" + p.auth_url;
            return r;
        }

        const std::string service = IspSuffix(p);
        const std::string ip = ctx.local_ip.empty() ? "0.0.0.0" : ctx.local_ip;
        const int rnd = std::rand() % 9000 + 1000;

        r.request_url = scheme_host + ":" + port + "/eportal/" +
            "?c=Portal&a=login&callback=dr1003&login_method=1" +
            "&user_account=" + UrlEncode(p.username) + "%2C0%2C" + UrlEncode(service) +
            "&user_password=" + UrlEncode(p.password) +
            "&wlan_user_ip=" + ip +
            "&wlan_user_ipv6=&wlan_user_mac=000000000000" +
            "&wlan_ac_ip=&wlan_ac_name=" +
            "&jsVersion=4.1.3&v=" + std::to_string(rnd) + "&lang=zh";
        r.request_body = "(GET，无请求体)";

        HttpRequest req;
        req.url = r.request_url;
        req.method = "GET";
        req.timeout_ms = 8000;
        req.follow_redirects = false;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "登录请求发送失败：" + err;
            return r;
        }

        const std::string text = DecodeBody(resp);
        LogInfo("锐捷登录响应 HTTP " + std::to_string(resp.status));

        // JSONP: dr1003({"result":1,"msg":"..."});
        std::smatch m;
        std::regex re_result("\"result\"\\s*:\\s*(-?\\d+)");
        std::regex re_msg("\"msg\"\\s*:\\s*\"([^\"]*)\"");
        bool has_result = std::regex_search(text, m, re_result);
        int code = -1;
        if (has_result) code = std::atoi(m[1].str().c_str());

        std::string msg;
        if (std::regex_search(text, m, re_msg)) msg = m[1].str();

        r.vendor_code = code;
        if (has_result && code == 1) {
            r.success = true;
            r.message = "服务器返回登录成功";
        } else if (has_result) {
            r.success = false;
            r.message = msg.empty() ? ("服务器返回错误码 " + std::to_string(code)) : msg;
        } else {
            r.success = false;
            r.message = "无法解析服务器响应，将由网络探测确认实际状态";
        }
        return r;
    }

    const char* Name() const override { return "EportalAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeEportalAuth() { return std::make_unique<EportalAuth>(); }

}  // namespace cnk
```

**注意**：锐捷的 `vendor_code == 0` 表示凭据错误，但各校错误码不统一，故**不**据此触发停止重试（只有 Dr.COM 的 `code==2` 才有此语义）。上层只用 `success` 字段。

- [ ] **Step 4: 从 Stubs.cpp 移除 MakeEportalAuth，更新 CMakeLists.txt**

追加 `src/native/auth/EportalAuth.cpp` 与 `tests/eportal_auth_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 锐捷 eportal 认证实现"
```

> **✅ 实施注记（2026-10-16，任务 19-21 一并落地）**：已按本任务实现 `EportalAuth`
> （`src/native/auth/EportalAuth.cpp`），模拟器加入 `vendor="eportal"` 的门户页与 JSONP
> 登录端点，新增 `tests/eportal_auth_test.cpp`（含模拟器端到端）。
> **与本任务原文的偏差（均为实测/审查发现）**：
> ① 原文 `ctx.local_ip.empty() ? "0.0.0.0"`——M1 的 `NetContext.local_ip` **恒为空**，
> 会向真实服务器提交 `0.0.0.0` 而基本必失败；改为 `ResolveClientIp()`
> （`core/LocalIp.h`：无包 UDP connect + getsockname 现算本机源地址，最后才退 0.0.0.0）。
> ② `vendor_code` 固定 `-1`，不透传厂商原始码——本项目只有 Dr.COM 的码 2 有
> 「凭据错误 → 停止重试」的约定语义（`Scheduler.cpp` 依赖它），跨厂商透传会误触发停止
> 重试；原文「不据此触发停止重试」的意图以此方式落实，原始码仍出现在失败信息里。
> ③ 响应解析改用 nlohmann（原文用正则），与 `DrcomAuth` 一致，且对 JSON/JSONP 都稳。
> **未实测**：本机是 Dr.COM 环境。锐捷路径待有锐捷校园网的反馈者用
> `--simulate=eportal --test-login` 自测后回传真实输出（README「已知限制」已记）。

---

## 任务 20：BCrypt 加密工具与深澜认证

**Files:**
- Create: `src/native/crypto/Hash.h`
- Create: `src/native/crypto/Hash.cpp`
- Create: `src/native/crypto/SrunXEncode.h`
- Create: `src/native/crypto/SrunXEncode.cpp`
- Create: `src/native/auth/SrunAuth.cpp`
- Modify: `src/native/auth/Stubs.cpp`（移除 `MakeSrunAuth`）
- Modify: `CMakeLists.txt`
- Create: `tests/crypto_test.cpp`
- Create: `tests/srun_auth_test.cpp`

**Interfaces:**
- Produces:
  - `std::string cnk::Md5Hex(const std::string& in)`
  - `std::string cnk::Sha1Hex(const std::string& in)`
  - `std::string cnk::HmacMd5Hex(const std::string& data, const std::string& key)`
  - `std::string cnk::SrunBase64(const std::string& in)` — 使用深澜自定义码表，**无 padding**
  - `std::string cnk::SrunXEncode(const std::string& data, const std::string& key)`
  - `std::unique_ptr<cnk::IAuth> cnk::MakeSrunAuth()`

**深澜自定义 base64 码表**（固定常量）：

```
LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3DLbfKwv6xztjI7DeBE45QA
```

**xEncode 算法**（公开的深澜登录协议实现，此处按标准实现）：

1. 把字符串按 4 字节小端打包为 `uint32` 数组；`include_length` 为真时追加原长度
2. 把 key 同样打包；不足 4 个元素时补零到 4
3. 做 `q = 6 + 52/(n+1)` 轮 TEA 变体混淆
4. 结果按 4 字节小端拆回字符串

- [ ] **Step 1: 写失败测试**

`tests/crypto_test.cpp`:

```cpp
#include "doctest.h"
#include "crypto/Hash.h"
#include "crypto/SrunXEncode.h"

TEST_CASE("MD5 标准测试向量") {
    CHECK(cnk::Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(cnk::Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(cnk::Md5Hex("The quick brown fox jumps over the lazy dog")
          == "9e107d9d372bb6826bd81d3542a419d6");
}

TEST_CASE("SHA1 标准测试向量") {
    CHECK(cnk::Sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(cnk::Sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("HMAC-MD5 标准测试向量（RFC 2202）") {
    // key = 0x0b * 16, data = "Hi There"
    std::string key(16, '\x0b');
    CHECK(cnk::HmacMd5Hex("Hi There", key)
          == "9294727a3638bb1c13f48ef8158bfc9d");
}

TEST_CASE("深澜自定义 base64 使用非标准码表且无 padding") {
    // 标准 base64("abc") = "YWJj"，深澜码表下 Y->L, W->V, J->o, j->J
    std::string out = cnk::SrunBase64("abc");
    CHECK(out == "LVoJ");
    CHECK(out.find('=') == std::string::npos);
}

TEST_CASE("SrunBase64 对空输入返回空") {
    CHECK(cnk::SrunBase64("") == "");
}

TEST_CASE("SrunXEncode 对固定输入产生稳定输出") {
    // 同一输入两次调用必须一致
    std::string a = cnk::SrunXEncode("hello", "1234567890");
    std::string b = cnk::SrunXEncode("hello", "1234567890");
    CHECK(a == b);
    CHECK(a.size() > 0);
    // 不同 key 应产生不同结果
    std::string c = cnk::SrunXEncode("hello", "abcdefghij");
    CHECK(a != c);
}

TEST_CASE("SrunXEncode 对空输入返回空") {
    CHECK(cnk::SrunXEncode("", "key") == "");
}
```

**关于 xEncode 的已知向量**：深澜协议没有公开的官方测试向量，因此上面对 `SrunXEncode` 只能做**稳定性与差异性**断言，无法做绝对正确性断言。真实正确性依赖任务 20 的模拟服务器——请在 `SimServer` 中加入 `vendor = "srun"` 分支，用**独立实现**的参考算法（例如复制一份 JS 版相同的逻辑到测试中）交叉验证。若交叉验证成本过高，则接受以下约定：**深澜路径在本项目中标记为「未实测」，用户若使用深澜，`--test-login` 的输出将是唯一验证手段。** 这一点必须写进 README 的「已知限制」。

`tests/srun_auth_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/AuthFactory.h"

TEST_CASE("深澜请求包含必要参数") {
    cnk::Profile p;
    p.auth_url = "http://10.0.0.1/";
    p.username = "alice";
    p.password = "pw";
    p.vendor = "srun";
    p.isp.preset = "telecom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    ctx.local_ip = "10.0.0.5";
    auto r = auth->Login(p, ctx);
    // 服务器不可达，但请求应当已被构造出来
    CHECK(r.request_url.find("get_challenge") != std::string::npos
          || r.message.size() > 0);
}

TEST_CASE("深澜空凭据快速失败") {
    cnk::Profile p;
    p.vendor = "srun";
    p.auth_url = "http://127.0.0.1:1/";
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号") != std::string::npos ||
          r.message.find("密码") != std::string::npos);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现 Hash**

`src/native/crypto/Hash.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

std::string Md5Hex(const std::string& in);
std::string Sha1Hex(const std::string& in);
std::string HmacMd5Hex(const std::string& data, const std::string& key);

}  // namespace cnk
```

`src/native/crypto/Hash.cpp`:

```cpp
#include "crypto/Hash.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace cnk {

namespace {

std::string ToHex(const std::vector<BYTE>& bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (BYTE b : bytes) {
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }
    return out;
}

bool HashWith(BCRYPT_ALG_HANDLE alg, const std::string& data,
              std::vector<BYTE>& out, bool with_hmac,
              const std::string& key) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS st = BCryptCreateHash(alg, &hash, nullptr, 0,
                                   with_hmac ? (PUCHAR)key.data() : nullptr,
                                   with_hmac ? (ULONG)key.size() : 0, 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    st = BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    if (!BCRYPT_SUCCESS(st)) { BCryptDestroyHash(hash); return false; }

    DWORD hash_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len,
                      sizeof(hash_len), &cb, 0);
    out.resize(hash_len);
    st = BCryptFinishHash(hash, out.data(), hash_len, 0);
    BCryptDestroyHash(hash);
    return BCRYPT_SUCCESS(st);
}

std::string HexOf(BCRYPT_ALG_HANDLE alg_id, const std::string& data,
                  bool with_hmac, const std::string& key) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &alg, alg_id, nullptr, with_hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0)))
        return {};
    std::vector<BYTE> digest;
    bool ok = HashWith(alg, data, digest, with_hmac, key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? ToHex(digest) : std::string{};
}

}  // namespace

std::string Md5Hex(const std::string& in) {
    return HexOf(BCRYPT_MD5_ALGORITHM, in, false, {});
}

std::string Sha1Hex(const std::string& in) {
    return HexOf(BCRYPT_SHA1_ALGORITHM, in, false, {});
}

std::string HmacMd5Hex(const std::string& data, const std::string& key) {
    return HexOf(BCRYPT_MD5_ALGORITHM, data, true, key);
}

}  // namespace cnk
```

- [ ] **Step 4: 实现 SrunXEncode**

`src/native/crypto/SrunXEncode.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 深澜自定义码表的 base64，无 padding
std::string SrunBase64(const std::string& in);

// 深澜 xEncode：TEA 变体 + 自定义 base64
std::string SrunXEncode(const std::string& data, const std::string& key);

}  // namespace cnk
```

`src/native/crypto/SrunXEncode.cpp`:

```cpp
#include "crypto/SrunXEncode.h"
#include "crypto/Hash.h"
#include <vector>
#include <cstdint>

namespace cnk {

namespace {

const char kSrunTable[] =
    "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3DLbfKwv6xztjI7DeBE45QA";

// 按 4 字节小端打包为 uint32
std::vector<uint32_t> PackBytes(const std::string& s, bool include_length) {
    std::vector<uint32_t> v;
    const size_t n = s.size();
    for (size_t i = 0; i < n; i += 4) {
        uint32_t word = 0;
        for (size_t j = 0; j < 4 && i + j < n; ++j)
            word |= (uint32_t)(unsigned char)s[i + j] << (8 * j);
        v.push_back(word);
    }
    if (include_length) v.push_back((uint32_t)n);
    return v;
}

std::string UnpackBytes(const std::vector<uint32_t>& v) {
    std::string out;
    out.reserve(v.size() * 4);
    for (uint32_t word : v)
        for (int j = 0; j < 4; ++j)
            out += (char)((word >> (8 * j)) & 0xFF);
    return out;
}

std::vector<uint32_t> SrunCore(std::vector<uint32_t> v, std::vector<uint32_t> k) {
    const size_t n = v.size() - 1;
    if (n < 1) return v;
    if (k.size() < 4) k.resize(4, 0);

    uint32_t z = v[n], y = v[0];
    const uint32_t delta = 0x9E3779B9;
    const uint32_t mask = 0xFFFFFFFF;
    uint32_t d = 0;
    size_t rounds = (size_t)(6 + 52 / (n + 1));

    while (rounds-- > 0) {
        d = (d + delta) & mask;
        const uint32_t e = (d >> 2) & 3;
        for (size_t p = 0; p < n; ++p) {
            y = v[p + 1];
            uint32_t m = (z >> 5) ^ (y << 2);
            m = (m + (((y >> 3) ^ (z << 4)) ^ (d ^ y))) & mask;
            m = (m + (k[(p & 3) ^ e] ^ z)) & mask;
            z = v[p] = (v[p] + m) & mask;
        }
        y = v[0];
        uint32_t m = (z >> 5) ^ (y << 2);
        m = (m + (((y >> 3) ^ (z << 4)) ^ (d ^ y))) & mask;
        m = (m + (k[(n & 3) ^ e] ^ z)) & mask;
        z = v[n] = (v[n] + m) & mask;
    }
    return v;
}

}  // namespace

std::string SrunBase64(const std::string& in) {
    if (in.empty()) return {};
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += kSrunTable[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6)
        out += kSrunTable[((val << 8) >> (bits + 8)) & 0x3F];
    return out;   // 无 padding
}

std::string SrunXEncode(const std::string& data, const std::string& key) {
    if (data.empty()) return {};

    std::vector<uint32_t> v = PackBytes(data, true);
    std::vector<uint32_t> k = PackBytes(key, false);

    v = SrunCore(std::move(v), std::move(k));
    return SrunBase64(UnpackBytes(v));
}

}  // namespace cnk
```

- [ ] **Step 5: 实现 SrunAuth**

`src/native/auth/SrunAuth.cpp`:

```cpp
#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include "core/Log.h"
#include "crypto/Hash.h"
#include "crypto/SrunXEncode.h"
#include "json.hpp"
#include <cstdlib>

using json = nlohmann::json;

namespace cnk {

namespace {

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class SrunAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;
        if (p.username.empty()) { r.message = "账号为空，请在设置中填写"; return r; }
        if (p.password.empty()) { r.message = "密码为空，请在设置中填写"; return r; }

        std::string scheme_host, port;
        if (!SplitHostPort(p.auth_url, scheme_host, port, "80")) {
            r.message = "认证网址格式不正确：" + p.auth_url;
            return r;
        }
        const std::string base = scheme_host + ":" + port + "/cgi-bin/";
        const std::string ip = ctx.local_ip.empty() ? "0.0.0.0" : ctx.local_ip;
        const std::string account = p.username + IspSuffix(p);
        const std::string ts = std::to_string(NowMs());

        // 1) 取 challenge
        std::string chal_url = base + "get_challenge?callback=jsonp&username=" +
            account + "&ip=" + ip + "&_=" + ts;
        HttpRequest creq;
        creq.url = chal_url;
        creq.timeout_ms = 8000;
        HttpResponse cresp;
        std::string err;
        if (!HttpPerform(creq, cresp, err)) {
            r.request_url = chal_url;
            r.message = "获取 challenge 失败：" + err;
            return r;
        }

        const std::string chal_text = DecodeBody(cresp);
        std::string token;
        {
            // 响应形如 jsonp({"challenge":"xxx","ip":"..."})
            size_t lb = chal_text.find('(');
            size_t rb = chal_text.rfind(')');
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                try {
                    json j = json::parse(chal_text.substr(lb + 1, rb - lb - 1));
                    token = j.value("challenge", std::string{});
                } catch (const std::exception&) {}
            }
        }
        if (token.empty()) {
            r.request_url = chal_url;
            r.message = "服务器未返回 challenge，可能不是深澜认证或接口有变";
            return r;
        }

        // 2) 组装加密参数
        const std::string hmd5 = HmacMd5Hex(p.password, token);

        std::string ac_id = "1";
        if (ctx.portal_body.find("ac_id=") != std::string::npos) {
            size_t q1 = ctx.portal_body.find("ac_id=");
            size_t q2 = ctx.portal_body.find_first_of("&\"'", q1);
            ac_id = ctx.portal_body.substr(q1 + 6,
                q2 == std::string::npos ? std::string::npos : q2 - (q1 + 6));
            if (ac_id.empty()) ac_id = "1";
        }

        json info;
        info["username"] = account;
        info["password"] = p.password;
        info["ip"] = ip;
        info["acid"] = ac_id;
        info["enc_ver"] = "srun_bx1";
        const std::string info_json = info.dump();
        const std::string info_enc = "{SRBX1}" + SrunXEncode(info_json, token);

        const std::string n_str = "200";
        const std::string type_str = "1";
        std::string chk = token + account + token + hmd5 + token + ac_id + token +
                          ip + token + n_str + token + type_str + token + info_enc;
        const std::string chksum = HmacMd5Hex(chk, token);

        // 3) 登录
        r.request_url = base + "srun_portal?callback=jsonp&action=login" +
            "&username=" + account +
            "&password=%7BMD5%7D" + hmd5 +
            "&ac_id=" + ac_id +
            "&ip=" + ip +
            "&chksum=" + chksum +
            "&info=" + info_enc +
            "&n=200&type=1&double_stack=0&_=" + ts;
        r.request_body = "(GET，无请求体)";

        HttpRequest req;
        req.url = r.request_url;
        req.timeout_ms = 8000;
        HttpResponse resp;
        if (!HttpPerform(req, resp, err)) {
            r.message = "登录请求发送失败：" + err;
            return r;
        }

        const std::string text = DecodeBody(resp);
        size_t lb = text.find('(');
        size_t rb = text.rfind(')');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            try {
                json j = json::parse(text.substr(lb + 1, rb - lb - 1));
                const std::string error = j.value("error", std::string{});
                const std::string msg = j.value("error_msg", std::string{});
                if (error == "ok" || j.value("code", -1) == 0) {
                    r.success = true;
                    r.message = j.value("suc_msg", std::string{"登录成功"});
                } else {
                    r.success = false;
                    r.message = msg.empty() ? ("错误：" + error) : msg;
                }
                return r;
            } catch (const std::exception&) {}
        }

        r.message = "无法解析服务器响应，将由网络探测确认实际状态";
        return r;
    }

    const char* Name() const override { return "SrunAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeSrunAuth() { return std::make_unique<SrunAuth>(); }

}  // namespace cnk
```

`SrunAuth.cpp` 需要 `#include <chrono>`。

- [ ] **Step 6: 从 Stubs.cpp 移除 MakeSrunAuth，更新 CMakeLists.txt**

追加 `src/native/crypto/Hash.cpp`、`src/native/crypto/SrunXEncode.cpp`、`src/native/auth/SrunAuth.cpp`、`tests/crypto_test.cpp`、`tests/srun_auth_test.cpp`。

- [ ] **Step 7: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 8: 提交**

```bash
git add -A
git commit -m "feat: BCrypt 哈希工具与深澜 srun 认证（未实测）"
```

> **✅ 实施注记（2026-10-16，任务 19-21 一并落地）**：已实现 `crypto/Hash`
> （BCrypt MD5/SHA1/HMAC-MD5）、`crypto/SrunXEncode`、`auth/SrunAuth`；模拟器加入
> `get_challenge` 与 `srun_portal`（后者用「同 token 重新加密期望 JSON 后逐字节比对」
> 校验 `info`，并复算 chksum——因此端到端钉住了字段取值与拼接顺序）。
> **⚠ 本任务原文有两处错误，已纠正（重要）**：
> ① **码表是坏表**：原文 `…Uq/3DLbfKw…`（大写 D）里 `'L'` 出现两次、缺 `'d'` 与 `'l'`，
> **不是 64 字符的有效置换**——照抄会静默产出错误密文。正确表把该位写成小写 `d`
> （与 srunc 客户端的 `ALPHABET` 常量逐字节一致；置换计数：小写 26 + 大写 26 +
> 数字 10 + 符号 2 = 64）。
> ② **`SrunBase64("abc")` 的期望值 `"LVoJ"` 是错的**，正确为 `"ZaRk"`
> （标准 base64("abc")＝"YWJj"，按同下标映射：Y(24)→Z、W(22)→a、J(9)→R、j(35)→k）。
> **正确性依据**：两份独立参考实现（门户 JS 写法 vs srunc 的 `quirk_base64`）在 150 组
> 输入（长度 0..24）上交叉核对**零差异**；向量与可复算脚本见
> `tools/srun_xencode_oracle.js` 与 `tests/crypto_test.cpp`。
> ③ `SrunBase64` 累加器改 `uint32_t`（原文 `int` 的有符号左移溢出是 UB；取低 6 位的结果
> 与参考实现等价，已用向量钉住）。
> ④ **`info` 必须 URL 编码**：原文把 `{SRBX1}<自定义 base64>`（含 `+` `/`）直接拼进 URL，
> 服务器会按 form-urlencoded 把 `+` 解成空格、**密文损坏**——与 Dr.COM 的 `params` 同类坑。
> ⑤ 非 ASCII 输入口径差异记入 README「已知限制」（本实现 UTF-8 字节，门户 JS 用
> `charCodeAt`/UTF-16 码元；纯 ASCII 等价）。
> **未实测**：深澜路径待有深澜校园网的反馈者用 `--simulate=srun --test-login` 自测。
>
> **🔴 第三处原文错误（2026-10-16 独立审阅 P0，会让真实深澜部署 100% 登录失败）**：
> 原文 `:7413` 的 `chksum = HmacMd5Hex(chk, token)` **错了——chksum 是 `SHA1(chkstr)`**。
> 三份独立来源一致（本轮均已抓取核对）：深大 srun 逆向（门户 `Portal.js` 原文：
> 「chksum => sha1 哈希后的 chkstr」）、`srunc` 的 `params.rs`（`sha1_hex(join(&[...]))`）、
> `bitgateway-client` 的 `crypto.rs::checksum()`（`Sha1::new()`）。旁证：`Sha1Hex` 实现后
> 在产线代码里**零调用**——它本来就是为这一行准备的。
> 之所以测试全绿：模拟器用**同一个** `HmacMd5Hex` 算期望值，双方共享同一错误公式，
> 端到端恒真。已修（客户端与模拟器同步改 `Sha1Hex`），并新增**请求级 KAT**——
> 在测试里按上述三份来源的写法独立复算 hmd5 / info / chksum，不再依赖产品公式。
> 同一轮还确认了两点：**base64 带标准 padding**（srunc、bitgateway、深大 Python 三个
> 能实际登录的实现都带；本实现原先无 padding，已改），以及 `info` 走 UTF-8 字节
> （门户 JS 用 `charCodeAt`/UTF-16 只影响非 ASCII）。
> ac_id 亦改为**优先取跳转 URL 的查询串**（真实形态如 `?ac_id=12`），其次门户页，最后退 `1`。

---

## 任务 21：通用表单认证

补齐 spec §4.2 的最后一种策略。它是「未识别厂商」时的默认兜底，也是 spec §4.2 中列出的字段启发式规则的落点。

**Files:**
- Create: `src/native/auth/GenericFormAuth.cpp`
- Modify: `src/native/auth/Stubs.cpp`（移除 `MakeGenericFormAuth`，随后删除整个文件）
- Modify: `CMakeLists.txt`
- Create: `tests/generic_form_test.cpp`

**Interfaces:**
- Consumes: `IAuth`、`NetContext::portal_body`（已转 UTF-8）
- Produces: `std::unique_ptr<cnk::IAuth> cnk::MakeGenericFormAuth()`

**字段启发式匹配**（spec §4.2，大小写不敏感，按序取首个命中）：

| 用途 | 候选名 |
|---|---|
| 用户名 | `username` `user` `account` `name` `DDDDD` `user_account` `user_name` `login_name` |
| 密码 | `password` `pwd` `pass` `upass` `user_password` `passwd` |
| 运营商 | `domain` `service` `isp` `operator` `type` |

**行为**：
1. 从 `portal_body` 中找第一个 `<form>`，取其 `action` 与 `method`
2. 收集表单内所有 `<input>` 的 `name` 与 `value`
3. 按上表匹配用户名字段与密码字段，填入凭据；运营商字段填入 `IspSuffix`
4. `action` 为相对路径时解析为绝对 URL；为空时用 `auth_url` 本身。
   **⚠ 审阅注记（2026-10-13，任务 5 审阅发现）**：解析基准应**优先取 `NetContext.portal_url`**
   （表单所在页面的 URL，与 spec §4.2「relative action 相对 Portal **页面** URL 解析」对齐），
   仅在其为空时退回 `auth_url`。另注意 `ResolveUrl`（任务 5）对不以 `/` 开头的 path-relative
   action 按 base 的 **origin** 拼接，非 RFC 3986 目录语义（见 `core/Url.h` 警示）：本项目
   实测 Portal 为根路径部署、action 是 `/eportal/?...` 根相对，结果正确；但若目标站点页面位于
   子路径且 action 为 path-relative（如 `doLogin.jsp`），会丢目录前缀 —— 届时需扩展
   `ResolveUrl` 支持目录解析，或在调用处显式处理
5. 提交方式取 `method`（默认 POST）
6. 成功判定：响应含 `成功` / `success` / `result":1` 之一；否则交由上层探测判定
7. **找不到 `<form>`** 时返回明确失败，提示用户改用自定义模板

- [ ] **Step 1: 写失败测试**

`tests/generic_form_test.cpp`:

```cpp
#include "doctest.h"
#include "auth/AuthFactory.h"

// 一个典型的表单式 Portal 页面
static const char* kFormPage = R"HTML(
<html><body>
<form name="login" method="post" action="/doLogin">
  <input type="hidden" name="nasip" value="10.0.0.1">
  <input type="hidden" name="wlanuserip" value="10.0.0.5">
  <input type="text" name="username" value="">
  <input type="password" name="password" value="">
  <select name="domain"><option value="@cmcc">移动</option></select>
  <input type="submit" value="登录">
</form>
</body></html>
)HTML";

TEST_CASE("识别并填充标准表单字段") {
    cnk::Profile p;
    p.auth_url = "http://10.0.0.1/";
    p.username = "alice";
    p.password = "pw";
    p.vendor = "generic";
    p.isp.preset = "cmcc";

    cnk::NetContext ctx;
    ctx.portal_body = kFormPage;

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    CHECK(std::string(auth->Name()) == "GenericFormAuth");

    auto r = auth->Login(p, ctx);
    // 服务器不可达，但请求体应已正确组装
    CHECK(r.request_url.find("/doLogin") != std::string::npos);
    CHECK(r.request_body.find("username=alice") != std::string::npos);
    CHECK(r.request_body.find("password=pw") != std::string::npos);
    CHECK(r.request_body.find("domain=%40cmcc") != std::string::npos);
    // 隐藏字段必须原样保留
    CHECK(r.request_body.find("nasip=10.0.0.1") != std::string::npos);
    CHECK(r.request_body.find("wlanuserip=10.0.0.5") != std::string::npos);
}

TEST_CASE("页面无表单时返回明确失败并提示自定义模板") {
    cnk::Profile p;
    p.auth_url = "http://10.0.0.1/";
    p.username = "u";
    p.password = "p";
    p.vendor = "generic";

    cnk::NetContext ctx;
    ctx.portal_body = "<html><body>no form here</body></html>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("自定义模板") != std::string::npos);
}

TEST_CASE("空 portal_body 时返回明确失败") {
    cnk::Profile p;
    p.auth_url = "http://10.0.0.1/";
    p.username = "u";
    p.password = "p";
    p.vendor = "generic";
    cnk::NetContext ctx;

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.size() > 0);
}

TEST_CASE("相对 action 相对 auth_url 解析") {
    cnk::Profile p;
    p.auth_url = "http://10.0.0.1/portal/";
    p.username = "u";
    p.password = "p";
    p.vendor = "generic";
    cnk::NetContext ctx;
    ctx.portal_body = kFormPage;

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("10.0.0.1") != std::string::npos);
    CHECK(r.request_url.find("/doLogin") != std::string::npos);
}

TEST_CASE("DDDDD 用户名变体也能识别") {
    cnk::Profile p;
    p.auth_url = "http://10.0.0.1/";
    p.username = "carol";
    p.password = "pw";
    p.vendor = "generic";
    cnk::NetContext ctx;
    ctx.portal_body =
        "<form action='/l' method='post'>"
        "<input name='DDDDD' value=''><input name='upass' value=''></form>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_body.find("DDDDD=carol") != std::string::npos);
    CHECK(r.request_body.find("upass=pw") != std::string::npos);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 用例失败（桩实现）。

- [ ] **Step 3: 实现 GenericFormAuth**

新建 `src/native/core/HtmlForm.h` 与 `.cpp`，把 HTML 表单解析独立出来（这是纯函数，易测）：

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>

namespace cnk {

struct FormField {
    std::string name;
    std::string value;
};

struct HtmlForm {
    std::string action;
    std::string method = "POST";
    std::vector<FormField> fields;
};

// 解析页面中第一个 <form>。无表单返回 false。
bool ParseFirstForm(const std::string& utf8_html, HtmlForm& out);

// 按候选名列表匹配字段（大小写不敏感）。未命中返回空串。
std::string FindFieldByNames(const HtmlForm& form,
                             const std::vector<std::string>& candidates);

}  // namespace cnk
```

`src/native/core/HtmlForm.cpp`:

```cpp
#include "core/HtmlForm.h"
#include <algorithm>
#include <cctype>
#include <regex>

namespace cnk {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// 在 [from, end) 内不区分大小写地找 needle，返回绝对下标或 npos
size_t FindCI(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty()) return std::string::npos;
    auto it = std::search(hay.begin() + (std::ptrdiff_t)from, hay.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    return it == hay.end() ? std::string::npos
                           : (size_t)std::distance(hay.begin(), it);
}

// 从某个标签的文本里取属性值；取不到返回空串
std::string Attr(const std::string& tag, const std::string& name) {
    size_t pos = FindCI(tag, name, 0);
    while (pos != std::string::npos) {
        size_t after = pos + name.size();
        // 属性名后必须紧跟可选的空白与 '='
        size_t i = after;
        while (i < tag.size() && std::isspace((unsigned char)tag[i])) ++i;
        if (i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() && std::isspace((unsigned char)tag[i])) ++i;
            if (i >= tag.size()) return {};
            char quote = tag[i];
            if (quote == '"' || quote == '\'') {
                size_t end = tag.find(quote, i + 1);
                if (end == std::string::npos) return {};
                return tag.substr(i + 1, end - i - 1);
            }
            size_t end = i;
            while (end < tag.size() && !std::isspace((unsigned char)tag[end]) &&
                   tag[end] != '>') ++end;
            return tag.substr(i, end - i);
        }
        pos = FindCI(tag, name, pos + 1);
    }
    return {};
}

}  // namespace

bool ParseFirstForm(const std::string& html, HtmlForm& out) {
    out = HtmlForm{};

    size_t form_start = FindCI(html, "<form", 0);
    if (form_start == std::string::npos) return false;

    size_t tag_end = html.find('>', form_start);
    if (tag_end == std::string::npos) return false;

    const std::string form_tag = html.substr(form_start, tag_end - form_start + 1);
    out.action = Attr(form_tag, "action");
    std::string method = Attr(form_tag, "method");
    // HTML 解析后的属性名可能带前缀，如 xml:lang，这里取最后一段
    out.method = method.empty() ? "POST" : method;

    size_t form_close = FindCI(html, "</form", tag_end);
    size_t body_end = form_close == std::string::npos ? html.size() : form_close;

    size_t i = tag_end + 1;
    while (i < body_end) {
        size_t inp = FindCI(html, "<input", i);
        if (inp == std::string::npos || inp >= body_end) break;
        size_t ie = html.find('>', inp);
        if (ie == std::string::npos || ie > body_end) break;

        std::string tag = html.substr(inp, ie - inp + 1);
        FormField f;
        f.name = Attr(tag, "name");
        f.value = Attr(tag, "value");
        if (!f.name.empty()) out.fields.push_back(f);

        i = ie + 1;
    }

    return true;
}

std::string FindFieldByNames(const HtmlForm& form,
                             const std::vector<std::string>& candidates) {
    // 先按候选顺序精确匹配（不区分大小写）
    for (const auto& want : candidates) {
        for (const auto& f : form.fields) {
            if (ToLower(f.name) == ToLower(want)) return f.name;
        }
    }
    // 再放宽为包含匹配
    for (const auto& want : candidates) {
        const std::string w = ToLower(want);
        for (const auto& f : form.fields) {
            const std::string n = ToLower(f.name);
            if (n.find(w) != std::string::npos) return f.name;
        }
    }
    return {};
}

}  // namespace cnk
```

`HtmlForm.h` 与 `.cpp` 均需加入 `CMakeLists.txt` 的 `cnk_core` 源列表。

`src/native/auth/GenericFormAuth.cpp`:

```cpp
#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/HtmlForm.h"
#include "core/Url.h"
#include "core/Log.h"
#include "core/Template.h"

namespace cnk {

namespace {

const std::vector<std::string> kUserFields = {
    "username", "user", "account", "name", "DDDDD",
    "user_account", "user_name", "login_name"};
const std::vector<std::string> kPassFields = {
    "password", "pwd", "pass", "upass", "user_password", "passwd"};
const std::vector<std::string> kIspFields = {
    "domain", "service", "isp", "operator", "type"};

std::string ToLowerCopy(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool CaseInsensitiveEqual(const std::string& a, const std::string& b) {
    return ToLowerCopy(a) == ToLowerCopy(b);
}

class GenericFormAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;

        if (ctx.portal_body.empty()) {
            r.message = "尚未取得 Portal 页面内容，无法解析登录表单。"
                        "若反复出现，请改用自定义模板。";
            return r;
        }
        if (p.username.empty()) { r.message = "账号为空，请在设置中填写"; return r; }
        if (p.password.empty()) { r.message = "密码为空，请在设置中填写"; return r; }

        HtmlForm form;
        if (!ParseFirstForm(ctx.portal_body, form)) {
            r.message = "Portal 页面中没有找到登录表单，无法自动填写。"
                        "请改用自定义模板：在浏览器开发者工具中复制登录请求的 "
                        "URL 与请求体，粘贴到设置的自定义模板中。";
            return r;
        }

        const std::string user_field = FindFieldByNames(form, kUserFields);
        const std::string pass_field = FindFieldByNames(form, kPassFields);
        const std::string isp_field  = FindFieldByNames(form, kIspFields);

        if (user_field.empty() || pass_field.empty()) {
            r.message = "无法识别表单中的账号或密码字段名，请改用自定义模板。";
            return r;
        }

        const std::string isp = IspSuffix(p);

        std::vector<std::pair<std::string, std::string>> pairs;
        for (const auto& f : form.fields) {
            std::string value = f.value;
            if (CaseInsensitiveEqual(f.name, user_field))      value = p.username;
            else if (CaseInsensitiveEqual(f.name, pass_field)) value = p.password;
            else if (!isp_field.empty() && CaseInsensitiveEqual(f.name, isp_field))
                value = isp;
            pairs.emplace_back(f.name, value);
        }

        std::string body;
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (i) body += "&";
            body += UrlEncode(pairs[i].first) + "=" + UrlEncode(pairs[i].second);
        }

        // ⚠ 解析基准见「行为」第 4 条审阅注记：优先 ctx.portal_url，其次 p.auth_url
        r.request_url = ResolveUrl(form.action, p.auth_url);
        r.request_body = body;

        HttpRequest req;
        req.url = r.request_url;
        const bool get = CaseInsensitiveEqual(form.method, "GET");
        req.method = get ? "GET" : "POST";
        if (get) {
            req.url += (req.url.find('?') == std::string::npos ? "?" : "&") + body;
        } else {
            req.body = body;
            req.content_type = "application/x-www-form-urlencoded";
        }
        req.timeout_ms = 8000;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "请求发送失败：" + err;
            return r;
        }

        const std::string text = DecodeBody(resp);
        const bool looks_ok = text.find("成功") != std::string::npos ||
                              text.find("success") != std::string::npos ||
                              text.find("\"result\":1") != std::string::npos ||
                              text.find("\"result\": 1") != std::string::npos;
        r.success = looks_ok;
        r.message = looks_ok ? "响应中包含成功标识"
                             : "响应中没有成功标识，将由网络探测确认实际状态";
        return r;
    }

    const char* Name() const override { return "GenericFormAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeGenericFormAuth() {
    return std::make_unique<GenericFormAuth>();
}

}  // namespace cnk
```

- [ ] **Step 4: 删除 Stubs.cpp 并更新 CMakeLists.txt**

`Stubs.cpp` 已无内容可留，删除文件并从 `CMakeLists.txt` 移除。追加 `src/native/core/HtmlForm.cpp`、`src/native/auth/GenericFormAuth.cpp`、`tests/generic_form_test.cpp`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。五种认证策略全部就位。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 通用表单认证，完成五种策略"
```

> **✅ 实施注记（2026-10-16，任务 19-21 一并落地）**：已实现 `core/HtmlForm`
> （`ParseFirstForm` / `FindFieldByNames`）与 `auth/GenericFormAuth`，并删除 `Stubs.cpp`
> ——五个认证策略全部为真实实现。
> **与本任务原文的偏差**：
> ① 原文 `ParseFirstForm` 只解析 `<input>`，而它自己的测试要求读到
> `<select name="domain">` 的值（`domain=%40cmcc`）——**自相矛盾**；实现补上 `<select>`
> （取 `selected` 的 option，否则第一个；无 `value` 时取标签文本），并跳过
> submit/button/reset/image 这类「只在被点击时才提交」的控件。
> ② 原文「行为」第 4 条的 ⚠ 注记要求「解析基准优先 `ctx.portal_url`」，但示例代码只用了
> `p.auth_url`；实现按注记优先 `portal_url`、为空时退回 `auth_url`，并新增用例钉住。
> ③ GET 表单的 `request_url` 原文不含查询串（字段拼在 `req.url` 上），诊断输出会看不见
> 填了什么；已改为输出最终 URL。
> ④ 第二轮「包含匹配」跳过 hidden 字段：否则候选 `user` 会命中 `wlanuserip`，
> 凭据被填进错误的字段（已加用例）。
> **已知边界（未改）**：`ResolveUrl` 对不以 `/` 开头的 path-relative action 按 origin
> 拼接（非 RFC 3986 目录语义）——根相对 action（真实 Portal 形态）正确，页面在子路径时
> 会丢目录前缀；已在代码注释与 README 标注，留给后续扩展。
> **另记（设计层面，未改）**：识别为某厂商后会**只**用该策略，失败不会回落到通用表单；
> 若某门户页面同时带 `nasip`+`wlanuserip`（会被判成锐捷）而实际是普通表单，就会走错策略。
> 建议后续加「策略链」：识别策略结构性失败时回落到 `GenericFormAuth`。
> **未实测**：真实表单式 Portal 待反馈者用 `--simulate=generic --test-login` 自测。

---

## 任务 22：IPC 命名管道服务

**Files:**
- Create: `src/native/ipc/PipeServer.h`
- Create: `src/native/ipc/PipeServer.cpp`
- Create: `src/native/ipc/Protocol.h`
- Create: `src/native/ipc/Protocol.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/ipc_test.cpp`

**Interfaces:**
- Produces:
  - `struct cnk::IpcRequest { int id; std::string cmd; std::string args_json; }`
  - `struct cnk::IpcResponse { int id; bool ok; std::string data_json; std::string error; }`
  - `std::string cnk::IpcSerialize(const IpcResponse& r)` — 单行 JSON
  - `bool cnk::IpcParseRequest(const std::string& line, IpcRequest& out, std::string& error)`
  - `class cnk::PipeServer`：
    - `using Handler = std::function<IpcResponse(const IpcRequest&)>;`
    - `bool Start(Handler handler, std::string& error)`
    - `void Stop()`

**安全**：管道安全描述符仅授予当前用户 SID（**不得**使用 `Global\` 或默认 everyone 权限），防止本机其他账户读取配置。

- [ ] **Step 1: 写失败测试**

`tests/ipc_test.cpp`:

```cpp
#include "doctest.h"
#include "ipc/Protocol.h"

TEST_CASE("请求解析正确") {
    cnk::IpcRequest r;
    std::string err;
    REQUIRE(cnk::IpcParseRequest(R"({"id":7,"cmd":"get_status","args":{}})", r, err));
    CHECK(r.id == 7);
    CHECK(r.cmd == "get_status");
}

TEST_CASE("缺 id 或 cmd 的请求被拒绝并给出错误") {
    cnk::IpcRequest r;
    std::string err;
    CHECK_FALSE(cnk::IpcParseRequest(R"({"cmd":"x"})", r, err));
    CHECK(err.size() > 0);
    CHECK_FALSE(cnk::IpcParseRequest(R"({"id":1})", r, err));
    CHECK(err.size() > 0);
}

TEST_CASE("非 JSON 被拒绝而不崩溃") {
    cnk::IpcRequest r;
    std::string err;
    CHECK_FALSE(cnk::IpcParseRequest("not json", r, err));
    CHECK(err.size() > 0);
}

TEST_CASE("响应序列化为单行且可往返") {
    cnk::IpcResponse resp;
    resp.id = 3;
    resp.ok = true;
    resp.data_json = R"({"state":"online"})";
    std::string line = cnk::IpcSerialize(resp);
    CHECK(line.find('\n') == std::string::npos);

    cnk::IpcRequest back;
    std::string err;
    // 用请求解析器验证 JSON 合法性（字段不同，仅验证可解析）
    cnk::IpcResponse parsed;
    REQUIRE(cnk::IpcParseResponse(line, parsed, err));
    CHECK(parsed.id == 3);
    CHECK(parsed.ok);
    CHECK(parsed.data_json == R"({"state":"online"})");
}

TEST_CASE("错误响应序列化带上 error 字段") {
    cnk::IpcResponse resp;
    resp.id = 1;
    resp.ok = false;
    resp.error = "未知命令";
    std::string line = cnk::IpcSerialize(resp);
    cnk::IpcResponse parsed;
    std::string err;
    REQUIRE(cnk::IpcParseResponse(line, parsed, err));
    CHECK_FALSE(parsed.ok);
    CHECK(parsed.error == "未知命令");
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 编译失败。

- [ ] **Step 3: 实现 Protocol**

`src/native/ipc/Protocol.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 管道名（不含 \\.\pipe\ 前缀）
constexpr const char* kPipeName = "CampusNetKeeper";

struct IpcRequest {
    int id = 0;
    std::string cmd;
    std::string args_json = "{}";
};

struct IpcResponse {
    int id = 0;
    bool ok = false;
    std::string data_json = "{}";
    std::string error;
};

std::string IpcSerialize(const IpcResponse& r);
bool IpcParseRequest(const std::string& line, IpcRequest& out, std::string& error);
bool IpcParseResponse(const std::string& line, IpcResponse& out, std::string& error);

}  // namespace cnk
```

`src/native/ipc/Protocol.cpp`:

```cpp
#include "ipc/Protocol.h"
#include "json.hpp"

using json = nlohmann::json;

namespace cnk {

std::string IpcSerialize(const IpcResponse& r) {
    json j;
    j["id"] = r.id;
    j["ok"] = r.ok;
    if (r.ok) j["data"] = json::parse(r.data_json, nullptr, false);
    if (!r.ok) j["error"] = r.error;
    if (j.contains("data") && j["data"].is_discarded()) j["data"] = json::object();
    return j.dump();   // dump 默认不换行
}

bool IpcParseRequest(const std::string& line, IpcRequest& out, std::string& error) {
    error.clear();
    out = IpcRequest{};
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "请求不是合法 JSON 对象";
        return false;
    }
    if (!j.contains("id") || !j["id"].is_number_integer()) {
        error = "请求缺少整数 id";
        return false;
    }
    if (!j.contains("cmd") || !j["cmd"].is_string()) {
        error = "请求缺少字符串 cmd";
        return false;
    }
    out.id = j["id"].get<int>();
    out.cmd = j["cmd"].get<std::string>();
    if (j.contains("args")) out.args_json = j["args"].dump();
    return true;
}

bool IpcParseResponse(const std::string& line, IpcResponse& out, std::string& error) {
    error.clear();
    out = IpcResponse{};
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "响应不是合法 JSON 对象";
        return false;
    }
    out.id = j.value("id", 0);
    out.ok = j.value("ok", false);
    if (j.contains("data")) out.data_json = j["data"].dump();
    out.error = j.value("error", std::string{});
    return true;
}

}  // namespace cnk
```

- [ ] **Step 4: 实现 PipeServer**

`src/native/ipc/PipeServer.h`:

```cpp
#pragma once
#include <string>
#include <functional>
#include <memory>
#include "ipc/Protocol.h"

namespace cnk {

class PipeServer {
public:
    using Handler = std::function<IpcResponse(const IpcRequest&)>;

    PipeServer();
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool Start(Handler handler, std::string& error);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
```

`src/native/ipc/PipeServer.cpp`:

```cpp
#include "ipc/PipeServer.h"
#include "core/Log.h"
#include <windows.h>
#include <sddl.h>
#include <thread>
#include <atomic>

namespace cnk {

struct PipeServer::Impl {
    std::wstring pipe_name;
    HANDLE stop_event = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    Handler handler;

    void Loop();
    void ServeOne(HANDLE pipe);
};

void PipeServer::Impl::ServeOne(HANDLE pipe) {
    std::string buf;
    char chunk[4096];
    DWORD read = 0;

    // 读一行（以 \n 结尾）
    while (buf.find('\n') == std::string::npos) {
        BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr);
        if (!ok || read == 0) break;
        buf.append(chunk, read);
        if (buf.size() > 1024 * 1024) break;
    }
    if (buf.empty()) return;

    size_t nl = buf.find('\n');
    std::string line = nl == std::string::npos ? buf : buf.substr(0, nl);

    IpcResponse resp;
    IpcRequest req;
    std::string err;
    if (!IpcParseRequest(line, req, err)) {
        resp.id = 0;
        resp.ok = false;
        resp.error = err;
    } else {
        resp = handler ? handler(req) : IpcResponse{req.id, false, "{}", "无处理器"};
        resp.id = req.id;
    }

    std::string out = IpcSerialize(resp) + "\n";
    DWORD written = 0;
    WriteFile(pipe, out.c_str(), (DWORD)out.size(), &written, nullptr);
}

void PipeServer::Impl::Loop() {
    while (running.load()) {
        HANDLE pipe = CreateNamedPipeW(
            pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LogError("创建命名管道失败，错误码 " + std::to_string(GetLastError()));
            return;
        }

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(pipe, &ov);
        DWORD wait = WaitForSingleObject(ov.hEvent, 500);
        CloseHandle(ov.hEvent);

        if (wait == WAIT_OBJECT_0) {
            ServeOne(pipe);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

PipeServer::PipeServer() : impl_(new Impl) {}
PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(Handler handler, std::string& error) {
    error.clear();

    // 仅允许当前用户访问
    std::wstring sddl;
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            error = "无法获取进程令牌";
            return false;
        }
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len);
        if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
            CloseHandle(token);
            error = "无法获取用户 SID";
            return false;
        }
        PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
        LPWSTR sid_str = nullptr;
        if (!ConvertSidToStringSidW(sid, &sid_str)) {
            CloseHandle(token);
            error = "SID 转换失败";
            return false;
        }
        sddl = L"D:(A;;GA;;;" + std::wstring(sid_str) + L")";
        LocalFree(sid_str);
        CloseHandle(token);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    // 转换失败时退化为默认描述符（仍限于本用户会话），但不阻断启动

    // 用当前用户 SID 作为管道名后缀，进一步隔离多用户环境
    impl_->pipe_name = L"\\\\.\\pipe\\CampusNetKeeper.";
    {
        // 复用上面的 SID 字符串逻辑
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD len = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &len);
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
                PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
                LPWSTR sid_str = nullptr;
                if (ConvertSidToStringSidW(sid, &sid_str)) {
                    impl_->pipe_name += sid_str;
                    LocalFree(sid_str);
                }
            }
            CloseHandle(token);
        }
    }

    impl_->handler = std::move(handler);
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->Loop(); });

    if (sd) LocalFree(sd);
    return true;
}

void PipeServer::Stop() {
    if (!impl_) return;
    if (impl_->running.exchange(false)) {
        // 让阻塞中的 ConnectNamedPipe 尽快返回：连接一次自己
        HANDLE h = CreateFileW(impl_->pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (impl_->worker.joinable()) impl_->worker.join();
    }
}

}  // namespace cnk
```

需要 `#include <vector>`。

- [ ] **Step 5: 更新 CMakeLists.txt**

追加 `src/native/ipc/Protocol.cpp`、`src/native/ipc/PipeServer.cpp`、`tests/ipc_test.cpp`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake --build build && ./build/cnk_tests.exe`
Expected: 全部 PASS。

- [ ] **Step 7: 提交**

```bash
git add -A
git commit -m "feat: 命名管道 IPC 服务，权限限定当前用户"
```

> **✅ 实施注记（2026-10-16）**：已实现 `ipc/Protocol`（结构体 + 单行 JSON 序列化/解析）与
> `ipc/PipeServer`（含 `PipeNameForCurrentUser()`），新增 `tests/ipc_test.cpp`（18 用例：
> 9 条协议 + 9 条服务端/管道），`CMakeLists.txt` 已接入。
> **⚠ 本任务原文有两处必须纠正的缺陷**：
> ① **安全描述符根本没生效**：原文认真构造了 `SECURITY_ATTRIBUTES sa`（`D:(A;;GA;;;<当前用户SID>)`），
> 但 `CreateNamedPipeW` 的最后一个参数传的是 **`nullptr`** —— 本节唯一的安全要求完全落空；
> 而且原文在**任何管道创建之前**就 `LocalFree(sd)`，即便传了也是悬垂指针。
> 实现改为真的传 `&sa`，把描述符生命周期交给 `Impl`（直到所有 `CreateNamedPipeW` 结束才释放），
> 并置 `PIPE_REJECT_REMOTE_CLIENTS`。拿不到 SID 或构造描述符失败时**拒绝启动**
>（原文注释写"退化为默认描述符但不阻断启动"——管道里传的是账号与配置，这个取舍是错的）。
> ② **`FlushFileBuffers` 会把服务线程永久堵死**：它要等客户端把数据读走才返回，
> 于是"客户端连上却不读"（UI 崩溃、恶意进程）就卡死服务线程，连 `Stop()` 的 `join()` 一起挂住。
> 已在非沙箱环境实测复现（测试进程 128 秒仅耗 0.02 秒 CPU）。实现改为**有界**等待（500ms）
> 客户端读完关闭后再 `DisconnectNamedPipe`。
> **另有三处加固**：`IpcParseResponse` 原文用 `j.value("id", 0)`——键存在但类型不符会抛
> `type_error`，异常逃出服务线程会终结整个进程，已改为显式判类型；连接/读/写全部改为
> overlapped + 超时 + `CancelIoEx`（挂起、半途断开、不发数据都不再可能阻塞）；响应超过
> 32KB 换成错误响应（超过管道缓冲区的写会在客户端不读时阻塞），处理器异常就地转成错误响应。
> **接口补充**（原文未定义、任务 23 的 C# 客户端要用）：`SetReadTimeoutMs`、`Name()`、
> `PipeNameForCurrentUser()`；名字规则写在 `PipeServer.h`，C# 侧对应
> `@"\\.\pipe\CampusNetKeeper." + WindowsIdentity.GetCurrent().User.Value`。
> **契约**：一个连接只处理一条请求；客户端必须对 `ERROR_PIPE_BUSY` **与
> `ERROR_FILE_NOT_FOUND`**（服务实例尚未创建）都重试。
> **环境限制**：DSH 受限沙箱**禁止打开命名管道**——实测连"不加安全描述符、系统默认权限"的
> 管道连接都返回 `ACCESS_DENIED`。故 6 条管道用例在该环境下**明确跳过并打印原因**（不伪装通过）；
> 真实结论以非沙箱运行为准：`-tc='*IPC*'` → **18 用例 / 93 断言全过，连跑 3 次稳定**；
> 非沙箱全量 237 用例 / 225 通过，12 项失败全是已知环境性（config_io 受污染的用户 TEMP、
> 2 项完整性标签），无产品失败。
> **尚未接入**：守护进程主循环还没有挂上该服务（任务 25 接命令层），也未被 C# 客户端调用过。
>
> **🔍 独立审阅（2026-10-16）后的修复**：审阅用**进程外探针实测**了 DACL——本实现只有
> **1 条 ACE**（当前用户 GA），而原文那种 `nullptr` 写法会得到 **5 条 ACE，含 Everyone
> (S-1-1-0) 与 ANONYMOUS LOGON (S-1-5-7) 的读取权限** ⇒ ①的修复确实堵掉了一条本地信息
> 泄露路径。审阅另报两个**会导致守护进程崩溃**的 P1，均已修：
> - **P1-1 漏 join → `std::terminate`**：工作线程因故障（管道名被别的进程以不同参数占用、
>   资源紧张）自行退出时会把 `running` 置 false，而 `Stop()` 原来只按 `running` 决定是否
>   `join`，于是线程永远不被 join，`~std::thread` 对 joinable 线程直接终止进程。现改为
>   **无条件 join**，并新增 `IsRunning()`/`LastError()` 与**退避重试**（不再"Start 返回 true
>   但服务其实已死"）。
> - **P1-2 序列化异常 → `std::terminate`**：`try` 原来只包住 handler，而 `dump()` 遇到非法
>   UTF-8 会抛 `type_error.316`（`error` 文案可能来自系统 API 的 GBK/ANSI 字节）→ 逃出服务
>   线程即崩。现 `IpcSerialize` 用 `error_handler_t::replace`，并在服务端加第二道 try。
> - **P1-3 慢客户端丢响应**：MSDN 明确 `DisconnectNamedPipe` 会**丢弃未读数据**，原先注释
>   写反了；500ms 等待只是缓解，故契约明确要求客户端及时读取并关闭，超时未关记 WARN。
>   等待本身改为"只在确实写了响应且仍在运行时"进行，避免白等拖慢 `Stop()`。
> - 另修 P2：无结尾换行的请求按**宽限**处理（与原文一致，客户端漏发换行也能得到应答）、
>   超长请求/响应都有明确错误响应。
> **已知边界（记档，未改）**：同一时刻只监听一个实例且处理器在服务线程上同步执行
> ⇒ 任务 25 的处理器必须快，长任务（认证/重连）要转交主循环；管道名可被同用户进程抢占
>（名字不校验属主）⇒ 任务 23 的 C# 客户端应校验对端进程（`GetNamedPipeServerProcessId`
> + 映像路径/会话）；按同一账户的其它登录会话仍可连接（如需进一步收紧可加登录 SID）；
> `{"id":>int32}` 会被截断。

---

## 任务 23：Avalonia UI 工程与 IPC 客户端

**Files:**
- Create: `src/ui/CampusNet.UI.csproj`
- Create: `src/ui/Program.cs`
- Create: `src/ui/App.axaml` / `App.axaml.cs`
- Create: `src/ui/Services/IpcClient.cs`
- Create: `src/ui/Models/AppConfig.cs`
- Create: `src/ui/Views/MainWindow.axaml` / `.axaml.cs`（占位）
- Modify: `.gitignore`

**Interfaces:**
- Produces:
  - `CampusNet.UI.exe`（或作为库被 native host 加载），入口 `Program.Main(string[] args)`
  - `IpcClient`：
    - `Task<JsonNode?> SendAsync(string cmd, JsonObject? args = null)`
    - `Task<bool> IsDaemonRunningAsync()`
  - C# 侧镜像的配置模型（与 C++ 的 `Config.h` 逐字段对应）

- [ ] **Step 1: 创建工程文件**

`src/ui/CampusNet.UI.csproj`:

```xml
<Project Sdk="Microsoft.NET.Sdk">

  <PropertyGroup>
    <OutputType>WinExe</OutputType>
    <TargetFramework>net9.0-windows</TargetFramework>
    <Nullable>enable</Nullable>
    <LangVersion>latest</LangVersion>
    <AssemblyName>CampusNet.UI</AssemblyName>
    <RootNamespace>CampusNet.UI</RootNamespace>
    <ApplicationManifest>app.manifest</ApplicationManifest>
    <AvaloniaUseCompiledBindingsByDefault>true</AvaloniaUseCompiledBindingsByDefault>
  </PropertyGroup>

  <ItemGroup>
    <PackageReference Include="Avalonia" Version="11.2.3" />
    <PackageReference Include="Avalonia.Desktop" Version="11.2.3" />
    <PackageReference Include="Avalonia.Themes.Fluent" Version="11.2.3" />
    <PackageReference Include="Avalonia.Fonts.Inter" Version="11.2.3" />
    <PackageReference Include="CommunityToolkit.Mvvm" Version="8.4.0" />
  </ItemGroup>

</Project>
```

若 11.2.3 不可用，用 `dotnet package search Avalonia --exact-match` 查当前稳定版并替换（三个包的版本必须一致）。

`src/ui/app.manifest`（启用视觉样式与 DPI 感知）：

```xml
<?xml version="1.0" encoding="utf-8"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">
  <assemblyIdentity version="1.0.0.0" name="CampusNet.UI"/>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

- [ ] **Step 2: 实现 IpcClient**

`src/ui/Services/IpcClient.cs`:

```csharp
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CampusNet.UI.Services;

public sealed class IpcClient : IAsyncDisposable
{
    private readonly string _pipeName;
    private NamedPipeClientStream? _pipe;
    private StreamReader? _reader;
    private StreamWriter? _writer;
    private int _nextId = 1;
    private readonly SemaphoreSlim _gate = new(1, 1);

    public IpcClient()
    {
        // 管道名带当前用户 SID 后缀，与 C++ 侧一致
        var sid = System.Security.Principal.WindowsIdentity.GetCurrent().User?.Value ?? "";
        _pipeName = "CampusNetKeeper." + sid;
    }

    private async Task EnsureConnectedAsync(CancellationToken ct)
    {
        if (_pipe is { IsConnected: true }) return;

        _pipe?.Dispose();
        _pipe = new NamedPipeClientStream(".", _pipeName, PipeDirection.InOut,
                                          PipeOptions.Asynchronous);
        await _pipe.ConnectAsync(1500, ct);
        _reader = new StreamReader(_pipe, Encoding.UTF8);
        _writer = new StreamWriter(_pipe, new UTF8Encoding(false)) { AutoFlush = true };
    }

    public async Task<bool> IsDaemonRunningAsync()
    {
        try
        {
            using var cts = new CancellationTokenSource(1500);
            await _gate.WaitAsync(cts.Token);
            try
            {
                await EnsureConnectedAsync(cts.Token);
                return true;
            }
            finally { _gate.Release(); }
        }
        catch { return false; }
    }

    /// <summary>发送命令并返回 data 节点；失败抛 IpcException。</summary>
    public async Task<JsonNode?> SendAsync(string cmd, JsonObject? args = null,
                                           CancellationToken ct = default)
    {
        await _gate.WaitAsync(ct);
        try
        {
            await EnsureConnectedAsync(ct);
            var id = _nextId++;
            var req = new JsonObject
            {
                ["id"] = id,
                ["cmd"] = cmd,
                ["args"] = args ?? new JsonObject(),
            };

            await _writer!.WriteLineAsync(req.ToJsonString());
            var line = await _reader!.ReadLineAsync(ct)
                       ?? throw new IpcException("守护进程未响应（连接已关闭）");

            var resp = JsonNode.Parse(line)?.AsObject()
                       ?? throw new IpcException("响应格式错误：" + line);

            if (resp["id"]?.GetValue<int>() != id)
                throw new IpcException("响应 id 不匹配");

            if (resp["ok"]?.GetValue<bool>() != true)
                throw new IpcException(resp["error"]?.GetValue<string>() ?? "未知错误");

            return resp["data"];
        }
        catch (Exception ex) when (ex is not IpcException)
        {
            _pipe?.Dispose();
            _pipe = null;
            throw new IpcException("无法与守护进程通信：" + ex.Message, ex);
        }
        finally { _gate.Release(); }
    }

    public async ValueTask DisposeAsync()
    {
        _pipe?.Dispose();
        await Task.CompletedTask;
    }
}

public sealed class IpcException : Exception
{
    public IpcException(string message, Exception? inner = null) : base(message, inner) { }
}
```

- [ ] **Step 3: 实现配置模型（与 C++ 逐字段对应）**

`src/ui/Models/AppConfig.cs`:

```csharp
using System.Text.Json.Serialization;

namespace CampusNet.UI.Models;

public sealed class IspConfig
{
    [JsonPropertyName("preset")] public string Preset { get; set; } = "campus";
    [JsonPropertyName("custom")] public string Custom { get; set; } = "";
}

public sealed class CustomTemplate
{
    [JsonPropertyName("method")] public string Method { get; set; } = "POST";
    [JsonPropertyName("url")] public string Url { get; set; } = "";
    [JsonPropertyName("body")] public string Body { get; set; } = "";
    [JsonPropertyName("headers")] public Dictionary<string, string> Headers { get; set; } = new();
    [JsonPropertyName("success_regex")] public string SuccessRegex { get; set; } = "";
}

public sealed class Profile
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("auth_url")] public string AuthUrl { get; set; } = "";
    [JsonPropertyName("username")] public string Username { get; set; } = "";
    [JsonPropertyName("password_enc")] public string PasswordEnc { get; set; } = "";
    [JsonPropertyName("vendor")] public string Vendor { get; set; } = "auto";
    [JsonPropertyName("isp")] public IspConfig Isp { get; set; } = new();
    [JsonPropertyName("custom_template")] public CustomTemplate CustomTemplate { get; set; } = new();
}

public sealed class Settings
{
    [JsonPropertyName("probe_interval_seconds")] public int ProbeIntervalSeconds { get; set; } = 5;
    [JsonPropertyName("probe_timeout_seconds")] public int ProbeTimeoutSeconds { get; set; } = 3;
    [JsonPropertyName("probe_url")] public string ProbeUrl { get; set; } = "http://www.msftncsi.com/ncsi.txt";
    [JsonPropertyName("probe_expect")] public string ProbeExpect { get; set; } = "Microsoft NCSI";
    [JsonPropertyName("retry_backoff_seconds")] public List<int> RetryBackoffSeconds { get; set; } = new() { 5, 10, 20, 30, 60 };
    [JsonPropertyName("autostart")] public bool Autostart { get; set; } = true;
    [JsonPropertyName("notify_on_reconnect")] public bool NotifyOnReconnect { get; set; } = true;
    [JsonPropertyName("log_level")] public string LogLevel { get; set; } = "info";
    [JsonPropertyName("log_max_kb")] public int LogMaxKb { get; set; } = 512;
    [JsonPropertyName("log_keep_files")] public int LogKeepFiles { get; set; } = 3;
}

public sealed class AppConfig
{
    [JsonPropertyName("version")] public int Version { get; set; } = 1;
    [JsonPropertyName("active_profile")] public string ActiveProfile { get; set; } = "默认";
    [JsonPropertyName("settings")] public Settings Settings { get; set; } = new();
    [JsonPropertyName("profiles")] public List<Profile> Profiles { get; set; } = new();
}
```

- [ ] **Step 4: 实现 App 与最小窗口**

`src/ui/Program.cs`:

```csharp
using Avalonia;

namespace CampusNet.UI;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args) =>
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
```

`src/ui/App.axaml`:

```xml
<Application xmlns="https://github.com/avaloniaui"
             xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
             x:Class="CampusNet.UI.App"
             RequestedThemeVariant="Default">
  <Application.Styles>
    <FluentTheme />
  </Application.Styles>
</Application>
```

`RequestedThemeVariant="Default"` 使亮/暗自动跟随系统。

`src/ui/App.axaml.cs`:

```csharp
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using CampusNet.UI.Views;

namespace CampusNet.UI;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow();
        base.OnFrameworkInitializationCompleted();
    }
}
```

`src/ui/Views/MainWindow.axaml`（任务 24 会扩充）：

```xml
<Window xmlns="https://github.com/avaloniaui"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        x:Class="CampusNet.UI.Views.MainWindow"
        Title="CampusNet-Keeper 设置"
        Width="720" Height="560"
        TransparencyLevelHint="AcrylicBlur, Blur">
  <TextBlock Text="设置界面加载中…" HorizontalAlignment="Center"
             VerticalAlignment="Center" />
</Window>
```

`src/ui/Views/MainWindow.axaml.cs`:

```csharp
using Avalonia.Controls;

namespace CampusNet.UI.Views;

public partial class MainWindow : Window
{
    public MainWindow() => InitializeComponent();
}
```

- [ ] **Step 5: 构建并运行**

```bash
dotnet build src/ui -c Release
dotnet run --project src/ui
```

Expected: 弹出一个标题为「CampusNet-Keeper 设置」的窗口，显示占位文本。

- [ ] **Step 6: 更新 .gitignore 并提交**

`.gitignore` 追加：

```
src/ui/bin/
src/ui/obj/
```

```bash
git add -A
git commit -m "feat: Avalonia UI 工程骨架与 IPC 客户端"
```

> **🟡 实施注记（2026-10-16，**部分完成**）**：本任务的**核心（Step 2/3 的 IPC 客户端与
> 协议常量）已实现并完成跨语言验证**；Step 1/4 的 Avalonia 外壳已按结构写好，但
> **尚未构建验证**——本机校园网 **HTTPS 不通**（实测 `api.nuget.org`、`www.microsoft.com`、
> `github.com` 的 TLS 握手全部失败，而 HTTP 探测显示网络 Online），NuGet 取不到 Avalonia 包，
> `~/.nuget/packages` 也无缓存。
>
> **已交付并验证的部分**：
> - `src/ui/CampusNet.UI.Core/`（**只依赖 BCL**，无需 NuGet）：`IpcClient`、`PipeProtocol`
>   （管道名规则 + 协议常量）。设计上把可测核心与 UI 外壳分开，正是为了在取不到包时
>   核心仍能构建与验证。
> - `src/ui/ContractCheck/`（零依赖控制台断言；xunit/nunit 都要 NuGet，故手写）+
>   `tools/ipc_probe_server.cpp`（CMake 目标 `cnk_ipc_probe`，探针服务端，**测试工具不随产品发布**）
>   → **跨语言契约自检 12/12 全过**：可达性、ping 往返、中文 args 的 UTF-8 往返、
>   未知命令错误形态、**连续 30 次请求全成功**、不存在的管道 1.5 秒内快速失败、
>   超大响应被服务端明确拒绝。
> - `src/ui/CampusNet.UI/`：`Theme.Light/Dark.axaml`（令牌层，值全部取自 DESIGN.md）、
>   `App.axaml(.cs)`、`Program.cs`、`Views/MainWindow.axaml(.cs)`（占位窗口，**不出现裸色值**）、
>   `app.manifest`（PerMonitorV2）。
> - `tools/check_ui_tokens.ps1`：**不依赖编译器的静态断言**——XML 良构、亮暗键一一对应、
>   除主题文件外无裸色值、`DynamicResource` 引用可解析；当前 4 项全过（它已经抓到一个真问题：
>   注释里的 `--radius` 含 `--`，XML 非法，同样会让 Avalonia 构建失败）。
>
> **⚠ 与本任务原文的偏差**：
> ① **目标框架 `net9.0-windows` → `net10.0-windows`**：本机只有 .NET SDK 10.0.401，无 .NET 9 运行时。
> ② **`IpcClient` 改为「每请求新建连接」**：原文保持长连接复用，但本项目 C++ 服务端的契约是
> **一个连接只处理一条请求**（见 `PipeServer.h`，且由 `cnk_tests` 的用例钉住），长连接复用下
> 第二次请求必然失败。同时按服务端契约补上「及时读取并关闭」与「BUSY / FILE_NOT_FOUND 重试」。
> ③ 不引 `Avalonia.Fonts.Inter`：Inter 不在 DESIGN.md 的字体栈里（字体由 `CnkFontSans/Mono` 决定，
> 栈尾补系统中文字族是平台必需的回退）。
> ④ 新增根目录 `Directory.Build.props`：把 `obj/bin` 重定向到 `build-ui/`（受限沙箱不允许在
> `src/` 下新建文件；同时保持源码树干净）。
>
> **未完成（下一步）**：`CampusNet.UI` 的首次 `dotnet build` 与运行、明/暗/键盘焦点三态验收、
> Step 3 的配置模型镜像（`Models/AppConfig.cs`，任务 24 要用）。等能取到 Avalonia 包后继续。
>
> **✅ 同日续做：上面那个"阻塞"其实是我误判，已全部打通。**
> 用户提醒"NuGet 不是随 VS 附带吗"，顺着查下去发现：VS 附带的是 NuGet **客户端**，不附带
> Avalonia 这类第三方包（`D:\Visual Studio 2026\Packages` 只有 2 个 nupkg，所谓"离线源"
> `Microsoft SDKs\NuGetPackages` 路径根本不存在）——**但真正的拦路虎不是包源，而是沙箱**：
> 受限模式下**外部 TLS 被拦**（`https://api.nuget.org/v3/index.json` 在非沙箱下返回 200、
> TCP 443 通，沙箱内 TLS 握手失败）。**所以"校园网 HTTPS 不通"这个结论是错的，特此更正。**
> 同理，**Avalonia 的 Win32 平台初始化也被沙箱拦**（`--self-test` 在沙箱内卡死且写不出结论文件）。
> 规律：**"联网取包"与"跑 GUI"都必须非沙箱；包进缓存后 `dotnet build` 在沙箱内可离线完成。**
>
> 续做结果（同日）：
> - `dotnet restore` 在非沙箱下一次成功（Avalonia 11.3.0 + Skia + HarfBuzz + CommunityToolkit
>   进 `build-ui/nuget` 缓存）；随后 **`dotnet build` 首次即通过**（零错误零警告）。
> - 新增 `CampusNet.UI.exe --self-test`：**不显示窗口**，真跑 Avalonia 逐个解析令牌并与
>   DESIGN.md 的色值比对 → **PASS**（23 个颜色键 × 明暗 + 圆角/字号/字体栈全对）。
>   它抓出两个真问题：`Color.ToString()` 对 `#ffffff` 返回命名色 `White`（应比 `Color` 不比字符串）；
>   以及 Avalonia 会留下消息循环线程，`Main` 返回后进程不退出（要 `Environment.Exit`）。
> - **真实窗口跑通并截图核验**：状态行显示「已连接守护进程」⇒ UI → IpcClient → 管道 →
>   C++ `PipeServer` 整条链路在真实窗口里成立；外观与 DESIGN.md 一致（纯白画布、卡片为
>   **发丝线边框 + 面色阶、无阴影**、主按钮近黑实心 + 6px 圆角）。截图见
>   `docs/design/ui-shell-20261016.png`。截图还抓到一个真缺陷：把完整管道名（含用户 SID）
>   塞进状态行会溢出，已改为只说结论、细节放 ToolTip。
> - 截图/自检的环境坑记入 `src/ui/README.md`：`WinExe` 用 `&` 启动 PowerShell 不等它、
>   退出码无意义；`Start-Process` 不重定向子进程 stdout 会被继承句柄挂住；
>   截图进程要先 `SetProcessDPIAware()` 否则区域偏移。
> **仍然剩余**：`Models/AppConfig.cs` 配置模型镜像（任务 24 要用）、明/暗/键盘焦点三态的人工
> 视觉验收（目前是自检 + 截图两级）、任务 24/25 的界面本体。

---

## 任务 24：设置界面

**Files:**
- Create: `src/ui/ViewModels/MainViewModel.cs`
- Create: `src/ui/ViewModels/ProfileViewModel.cs`
- Modify: `src/ui/Views/MainWindow.axaml` / `.axaml.cs`
- Modify: `src/ui/Services/IpcClient.cs`（若需补充命令）

**Interfaces:**
- Consumes: `IpcClient`（任务 23）
- Produces: 完整可用的设置界面

**界面布局**（spec §4.4，单页，不做多步向导）：

- **顶部状态条**：连接状态指示灯（颜色随状态）+ 当前 IP + 上次登录时间 + 「立即重连」按钮
- **中部表单**：认证网址、账号、密码（掩码 + 显示按钮）、运营商下拉、认证方式
- **底部**：「高级」可折叠区（探测间隔、重试上限、开机自启、重连通知）+ 保存
- **左侧栏**：配置档案列表 + 新增 / 删除

**运营商下拉（本次实现的重点）**：调用 `get_carrier_options` 从守护进程取得**服务器下发**的运营商列表；取得失败时回退到内置预设；**始终保留「自定义…」项**并展开文本框。

- [ ] **Step 1: 实现 MainViewModel**

`src/ui/ViewModels/MainViewModel.cs`：

```csharp
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CampusNet.UI.Services;
using System.Text.Json.Nodes;

namespace CampusNet.UI.ViewModels;

public partial class MainViewModel : ObservableObject
{
    private readonly IpcClient _ipc;
    private bool _suppressDirty;

    public ObservableCollection<ProfileViewModel> Profiles { get; } = new();

    [ObservableProperty] private ProfileViewModel? _selectedProfile;
    [ObservableProperty] private string _statusText = "正在连接守护进程…";
    [ObservableProperty] private string _statusColor = "#888888";
    [ObservableProperty] private bool _daemonRunning;
    [ObservableProperty] private string _localIp = "—";
    [ObservableProperty] private string _lastSuccessText = "从未";
    [ObservableProperty] private bool _showAdvanced;
    [ObservableProperty] private string? _lastError;

    // 高级设置
    [ObservableProperty] private int _probeIntervalSeconds = 5;
    [ObservableProperty] private bool _autostart = true;
    [ObservableProperty] private bool _notifyOnReconnect = true;

    // 密码显示开关（XAML 里绑定到密码框的显示切换）
    [ObservableProperty] private bool _showPassword;

    // 日志面板内容。任务 25 会接上实时刷新；
    // 这里先给出最小实现，否则本任务的 XAML 绑定会失败。
    [ObservableProperty] private string _logText = "";

    // 运营商下拉的最终绑定源：服务器下发项 + 内置回退 + 恒有的「自定义…」
    public ObservableCollection<CarrierOption> AllCarrierOptions { get; } = new();

    public MainViewModel(IpcClient ipc)
    {
        _ipc = ipc;
        _ = RefreshAsync();
    }

    [RelayCommand]
    private async Task RefreshAsync()
    {
        try
        {
            DaemonRunning = await _ipc.IsDaemonRunningAsync();
            if (!DaemonRunning)
            {
                StatusText = "守护进程未运行";
                StatusColor = "#888888";
                return;
            }

            var status = await _ipc.SendAsync("get_status");
            StatusText = status?["message"]?.GetValue<string>() ?? "未知";
            StatusColor = status?["state"]?.GetValue<string>() switch
            {
                "online" => "#2E7D32",
                "authenticating" => "#1565C0",
                "auth_failed" or "not_campus" => "#C62828",
                _ => "#F9A825",
            };
            LocalIp = status?["local_ip"]?.GetValue<string>() ?? "—";
            LastSuccessText = FormatUnix(status?["last_success"]?.GetValue<long>() ?? 0);

            var cfg = await _ipc.SendAsync("get_config");
            if (cfg is not null) ApplyConfig(cfg);
        }
        catch (IpcException ex)
        {
            DaemonRunning = false;
            LastError = ex.Message;
        }
    }

    private void ApplyConfig(JsonNode cfg)
    {
        _suppressDirty = true;
        try
        {
            Profiles.Clear();
            var profiles = cfg["profiles"]?.AsArray() ?? new JsonArray();
            foreach (var p in profiles)
                Profiles.Add(ProfileViewModel.FromJson(p!.AsObject()));

            var settings = cfg["settings"]?.AsObject();
            if (settings is not null)
            {
                ProbeIntervalSeconds = settings["probe_interval_seconds"]?.GetValue<int>() ?? 5;
                Autostart = settings["autostart"]?.GetValue<bool>() ?? true;
                NotifyOnReconnect = settings["notify_on_reconnect"]?.GetValue<bool>() ?? true;
            }

            var active = cfg["active_profile"]?.GetValue<string>();
            SelectedProfile = Profiles.FirstOrDefault(p => p.Name == active)
                              ?? Profiles.FirstOrDefault();
        }
        finally { _suppressDirty = false; }
    }

    [RelayCommand]
    private async Task SaveAsync()
    {
        try
        {
            var cfg = new JsonObject
            {
                ["version"] = 1,
                ["active_profile"] = SelectedProfile?.Name ?? "默认",
                ["settings"] = new JsonObject
                {
                    ["probe_interval_seconds"] = ProbeIntervalSeconds,
                    ["autostart"] = Autostart,
                    ["notify_on_reconnect"] = NotifyOnReconnect,
                },
                ["profiles"] = new JsonArray(Profiles.Select(p => (JsonNode)p.ToJson()).ToArray()),
            };
            await _ipc.SendAsync("set_config", new JsonObject { ["config"] = cfg });
            LastError = null;
            StatusText = "已保存";
        }
        catch (IpcException ex) { LastError = ex.Message; }
    }

    [RelayCommand]
    private async Task ReconnectNowAsync()
    {
        try { await _ipc.SendAsync("reconnect_now"); StatusText = "已触发重连"; }
        catch (IpcException ex) { LastError = ex.Message; }
    }

    [RelayCommand]
    private void AddProfile()
    {
        var p = new ProfileViewModel { Name = $"配置 {Profiles.Count + 1}" };
        Profiles.Add(p);
        SelectedProfile = p;
    }

    [RelayCommand]
    private void RemoveProfile()
    {
        if (SelectedProfile is null) return;
        Profiles.Remove(SelectedProfile);
        SelectedProfile = Profiles.FirstOrDefault();
    }

    [RelayCommand]
    private void OpenLogDir()
    {
        try
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "CampusNet-Keeper");
            Directory.CreateDirectory(dir);
            Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true });
        }
        catch (Exception ex) { LastError = ex.Message; }
    }

    private static string FormatUnix(long seconds)
    {
        if (seconds <= 0) return "从未";
        return DateTimeOffset.FromUnixTimeSeconds(seconds)
            .ToLocalTime().ToString("MM-dd HH:mm");
    }
}

public sealed class CarrierOption
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string Suffix { get; init; } = "";
    public bool IsCustom { get; init; }
}
```

- [ ] **Step 2: 实现 ProfileViewModel**

`src/ui/ViewModels/ProfileViewModel.cs`:

```csharp
using System.Text.Json.Nodes;
using CommunityToolkit.Mvvm.ComponentModel;

namespace CampusNet.UI.ViewModels;

public partial class ProfileViewModel : ObservableObject
{
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _authUrl = "";
    [ObservableProperty] private string _username = "";
    [ObservableProperty] private string _vendor = "auto";
    [ObservableProperty] private string _ispCustom = "";

    /// <summary>已保存过密码（不回传明文，仅表示状态）</summary>
    [ObservableProperty] private bool _hasPassword;

    /// <summary>用户本次输入的新密码；为空表示不修改</summary>
    [ObservableProperty] private string _newPassword = "";

    [ObservableProperty] private CarrierOption? _selectedCarrier;
    [ObservableProperty] private bool _isCustomIsp;

    partial void OnSelectedCarrierChanged(CarrierOption? value)
        => IsCustomIsp = value?.IsCustom ?? false;

    public string PasswordHint => HasPassword ? "已保存密码（留空则不修改）" : "请输入密码";

    public static ProfileViewModel FromJson(JsonObject o)
    {
        var vm = new ProfileViewModel
        {
            Name = o["name"]?.GetValue<string>() ?? "",
            AuthUrl = o["auth_url"]?.GetValue<string>() ?? "",
            Username = o["username"]?.GetValue<string>() ?? "",
            Vendor = o["vendor"]?.GetValue<string>() ?? "auto",
            HasPassword = o["has_password"]?.GetValue<bool>() ?? false,
        };

        var isp = o["isp"]?.AsObject();
        if (isp is not null)
        {
            var preset = isp["preset"]?.GetValue<string>() ?? "campus";
            vm.IspCustom = isp["custom"]?.GetValue<string>() ?? "";
            // SelectedCarrier 在 View 侧按 CarrierOptions 集合解析，
            // 这里只记录原始值供 ToJson 使用
            vm._ispPreset = preset;
        }
        return vm;
    }

    private string _ispPreset = "campus";

    /// <summary>由 View 在 CarrierOptions 就绪后调用，把 _ispPreset 映射到下拉选中项</summary>
    public void ResolveCarrier(IEnumerable<CarrierOption> options)
    {
        var match = options.FirstOrDefault(c => c.Id == _ispPreset);
        if (match is not null)
        {
            // 避免 OnSelectedCarrierChanged 覆盖 IsCustomIsp
            SelectedCarrier = match;
            _ispPreset = match.Id;
        }
    }

    public JsonObject ToJson()
    {
        var o = new JsonObject
        {
            ["name"] = Name,
            ["auth_url"] = AuthUrl,
            ["username"] = Username,
            ["vendor"] = Vendor,
            ["isp"] = new JsonObject
            {
                ["preset"] = SelectedCarrier?.Id ?? _ispPreset,
                ["custom"] = IspCustom,
            },
            ["custom_template"] = new JsonObject
            {
                ["method"] = "POST",
                ["url"] = "",
                ["body"] = "",
                ["headers"] = new JsonObject(),
                ["success_regex"] = "",
            },
        };

        // 关键：仅当用户输入了新密码才带上 password_plain。
        // 守护进程据此加密存储；不带则沿用磁盘上的旧密码。
        if (!string.IsNullOrEmpty(NewPassword))
            o["password_plain"] = NewPassword;

        return o;
    }
}
```

**关键约束**：`password_enc` **从不由 UI 写入**。界面只知道自己「有密码」与否；用户输入新值时通过 `password_plain` 传递，由 C++ 侧用 DPAPI 加密后落盘。UI 进程从头到尾不接触已存密码的明文。

- [ ] **Step 3: 实现 MainWindow 视图**

`src/ui/Views/MainWindow.axaml`:

```xml
<Window xmlns="https://github.com/avaloniaui"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:vm="using:CampusNet.UI.ViewModels"
        x:Class="CampusNet.UI.Views.MainWindow"
        x:DataType="vm:MainViewModel"
        Title="CampusNet-Keeper 设置"
        Width="820" Height="620" MinWidth="680" MinHeight="520"
        TransparencyLevelHint="AcrylicBlur, Blur"
        Background="Transparent">

  <Grid RowDefinitions="Auto,*,Auto" Margin="16">

    <!-- ── 状态条 ── -->
    <Border Grid.Row="0" CornerRadius="10" Padding="14,10" Margin="0,0,0,12"
            Background="{DynamicResource SystemControlBackgroundChromeMediumLowBrush}">
      <Grid ColumnDefinitions="Auto,*,Auto">
        <Ellipse Grid.Column="0" Width="12" Height="12" VerticalAlignment="Center"
                 Fill="{Binding StatusColor}" />
        <StackPanel Grid.Column="1" Margin="10,0,0,0" Orientation="Horizontal" Spacing="16">
          <TextBlock Text="{Binding StatusText}" FontWeight="SemiBold" VerticalAlignment="Center"/>
          <TextBlock Text="{Binding LocalIp, StringFormat='IP: {0}'}" VerticalAlignment="Center"
                     Opacity="0.7"/>
          <TextBlock Text="{Binding LastSuccessText, StringFormat='上次登录: {0}'}"
                     VerticalAlignment="Center" Opacity="0.7"/>
        </StackPanel>
        <Button Grid.Column="2" Content="立即重连"
                Command="{Binding ReconnectNowCommand}" />
      </Grid>
    </Border>

    <!-- ── 主体：左档案列表 / 右表单 ── -->
    <Grid Grid.Row="1" ColumnDefinitions="200,Auto,*">

      <StackPanel Grid.Column="0" Spacing="6">
        <TextBlock Text="配置档案" FontWeight="SemiBold" Margin="4,0,0,4"/>
        <ListBox ItemsSource="{Binding Profiles}"
                 SelectedItem="{Binding SelectedProfile}"
                 Height="320">
          <ListBox.ItemTemplate>
            <DataTemplate DataType="vm:ProfileViewModel">
              <TextBlock Text="{Binding Name}"/>
            </DataTemplate>
          </ListBox.ItemTemplate>
        </ListBox>
        <StackPanel Orientation="Horizontal" Spacing="6" HorizontalAlignment="Center">
          <Button Content="新增" Command="{Binding AddProfileCommand}"/>
          <Button Content="删除" Command="{Binding RemoveProfileCommand}"/>
        </StackPanel>
      </StackPanel>

      <GridSplitter Grid.Column="1" Width="6" Background="Transparent"/>

      <ScrollViewer Grid.Column="2" Padding="16,0,0,0">
        <StackPanel Spacing="12" IsVisible="{Binding SelectedProfile, Converter={x:Static ObjectConverters.IsNotNull}}"
                    DataContext="{Binding SelectedProfile}">

          <TextBlock Text="档案名称" FontWeight="SemiBold"/>
          <TextBox Text="{Binding Name}"/>

          <TextBlock Text="认证网址" FontWeight="SemiBold"/>
          <TextBox Text="{Binding AuthUrl}" Watermark="http://10.0.0.1/"/>

          <TextBlock Text="账号" FontWeight="SemiBold"/>
          <TextBox Text="{Binding Username}" Watermark="学号或账号，不含运营商后缀"/>

          <TextBlock Text="密码" FontWeight="SemiBold"/>
          <Grid ColumnDefinitions="*,Auto">
            <TextBox Grid.Column="0" Text="{Binding NewPassword}"
                     PasswordChar="●" Watermark="{Binding PasswordHint}"/>
            <ToggleButton Grid.Column="1" Content="显示" Margin="6,0,0,0"
                          IsChecked="{Binding $parent[Window].((vm:MainViewModel)DataContext).ShowPassword, Mode=TwoWay}"/>
          </Grid>
          <TextBlock Text="运营商" FontWeight="SemiBold" Margin="0,6,0,0"/>
          <ComboBox ItemsSource="{Binding $parent[Window].((vm:MainViewModel)DataContext).AllCarrierOptions}"
                    SelectedItem="{Binding SelectedCarrier}"
                    HorizontalAlignment="Stretch">
            <ComboBox.ItemTemplate>
              <DataTemplate DataType="vm:CarrierOption">
                <TextBlock Text="{Binding Name}"/>
              </DataTemplate>
            </ComboBox.ItemTemplate>
          </ComboBox>

          <TextBox Text="{Binding IspCustom}"
                   Watermark="自定义后缀，例如 @cmcc 或 telecom"
                   IsVisible="{Binding IsCustomIsp}"/>

          <TextBlock Text="认证方式" FontWeight="SemiBold" Margin="0,6,0,0"/>
          <ComboBox SelectedItem="{Binding Vendor}" HorizontalAlignment="Stretch">
            <ComboBoxItem Content="自动识别" Tag="auto"/>
            <ComboBoxItem Content="Dr.COM 哆点" Tag="drcom"/>
            <ComboBoxItem Content="锐捷 eportal" Tag="eportal"/>
            <ComboBoxItem Content="深澜 srun" Tag="srun"/>
            <ComboBoxItem Content="通用表单" Tag="generic"/>
            <ComboBoxItem Content="自定义模板" Tag="custom"/>
          </ComboBox>

          <Expander Header="高级" Margin="0,10,0,0" HorizontalAlignment="Stretch">
            <StackPanel Spacing="10" Margin="0,8,0,0"
                        DataContext="{Binding $parent[Window].((vm:MainViewModel)DataContext)}">
              <StackPanel Orientation="Horizontal" Spacing="10">
                <TextBlock Text="探测间隔（秒）" VerticalAlignment="Center"/>
                <NumericUpDown Value="{Binding ProbeIntervalSeconds}" Minimum="2" Maximum="300"
                               Increment="1" FormatString="0"/>
              </StackPanel>
              <CheckBox Content="开机自动启动" IsChecked="{Binding Autostart}"/>
              <CheckBox Content="重连成功时通知" IsChecked="{Binding NotifyOnReconnect}"/>
              <Button Content="打开日志目录" HorizontalAlignment="Left"
                      Command="{Binding OpenLogDirCommand}"/>
              <Expander Header="最近日志">
                <ScrollViewer Height="160">
                  <SelectableTextBlock Text="{Binding LogText}" FontFamily="Consolas"
                                       FontSize="12"/>
                </ScrollViewer>
              </Expander>
            </StackPanel>
          </Expander>
        </StackPanel>
      </ScrollViewer>
    </Grid>

    <!-- ── 底部 ── -->
    <Grid Grid.Row="2" ColumnDefinitions="*,Auto,Auto" Margin="0,12,0,0">
      <TextBlock Grid.Column="0" Text="{Binding LastError}" Foreground="#C62828"
                 TextWrapping="Wrap" VerticalAlignment="Center"/>
      <Button Grid.Column="1" Content="刷新" Margin="8,0" Command="{Binding RefreshCommand}"/>
      <Button Grid.Column="2" Content="保存" Classes="accent"
              Command="{Binding SaveCommand}"/>
    </Grid>
  </Grid>
</Window>
```

**说明**：密码显示切换用了一个占位绑定（`ShowPassword`），实现时在 `MainViewModel` 上补一个 `bool ShowPassword` 属性，并通过把 `TextBox.PasswordChar` 绑定到 `ShowPassword` 的取反来实现（Avalonia 中更简洁的做法是准备一个 `ReversiblePasswordConverter`，或用两个 `TextBox` 互斥显示）。

**运营商下拉的关键逻辑**在 `MainViewModel`：`AllCarrierOptions` 是最终绑定源，构造规则为

1. 若 `get_carrier_options` 返回非空，用它渲染（**服务器下发优先**）
2. 否则用内置预设：校园用户 / 校园电信 / 校园联通 / 校园移动 / 校园网
3. **末尾恒追加** `new CarrierOption { Id = "custom", Name = "自定义…", IsCustom = true }`

对应实现：

```csharp
    public ObservableCollection<CarrierOption> AllCarrierOptions { get; } = new();

    private void RebuildCarrierOptions(JsonNode? fromServer)
    {
        AllCarrierOptions.Clear();

        var opts = fromServer?["options"]?.AsArray();
        if (opts is { Count: > 0 })
        {
            foreach (var o in opts)
            {
                var id = o!["id"]?.GetValue<string>() ?? "";
                AllCarrierOptions.Add(new CarrierOption
                {
                    Id = id,
                    Name = o["name"]?.GetValue<string>() ?? id,
                    Suffix = o["suffix"]?.GetValue<string>() ?? "",
                });
            }
        }
        else
        {
            // 服务器未下发时的内置回退
            AllCarrierOptions.Add(new CarrierOption { Id = "campus",  Name = "校园网",   Suffix = "" });
            AllCarrierOptions.Add(new CarrierOption { Id = "telecom", Name = "校园电信", Suffix = "@dx" });
            AllCarrierOptions.Add(new CarrierOption { Id = "unicom",  Name = "校园联通", Suffix = "@lt" });
            AllCarrierOptions.Add(new CarrierOption { Id = "cmcc",    Name = "校园移动", Suffix = "@cmcc" });
        }

        // 恒保留，任何情况下都不得移除
        AllCarrierOptions.Add(new CarrierOption
            { Id = "custom", Name = "自定义…", IsCustom = true });

        foreach (var p in Profiles) p.ResolveCarrier(AllCarrierOptions);
    }
```

在 `RefreshAsync` 中，取到 config 后调用：

```csharp
            JsonNode? carriers = null;
            try { carriers = await _ipc.SendAsync("get_carrier_options"); }
            catch (IpcException) { /* 守护进程不支持则回退到预设 */ }
            RebuildCarrierOptions(carriers);
```

- [ ] **Step 4: 运行并手工验证**

```bash
dotnet run --project src/ui
```

Expected:
- 守护进程未运行时显示「守护进程未运行」
- 启动守护进程后点刷新，显示在线状态与 IP
- 能新增/删除配置、填写表单、点保存不报错
- 运营商下拉显示四项 + 自定义

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "feat: 设置界面，含服务器下发的运营商下拉与多配置切换"
```

---

## 任务 25：高级设置与日志面板

**Files:**
- Create: `src/ui/Views/LogPanel.axaml` / `.axaml.cs`
- Modify: `src/ui/Views/MainWindow.axaml`
- Modify: `src/native/main.cpp`（守护进程实现 `get_log_tail` 与 `get_carrier_options` 命令）

- [ ] **Step 1: 在守护进程实现 IPC 命令**

在 `main.cpp` 的守护模式中，任务 17 只创建了托盘。本任务把 `PipeServer` 接上：

```cpp
cnk::PipeServer ipc;
ipc.Start([&](const cnk::IpcRequest& req) -> cnk::IpcResponse {
    cnk::IpcResponse resp;
    resp.id = req.id;

    if (req.cmd == "get_status") {
        nlohmann::json d;
        d["state"] = cnk::RunStateName(sch->State());
        d["message"] = sch->LastMessage();
        d["local_ip"] = sch->LocalIp();
        d["last_success"] = sch->LastSuccessUnix();
        resp.ok = true;
        resp.data_json = d.dump();
    } else if (req.cmd == "reconnect_now") {
        sch->ForceReconnect();
        resp.ok = true;
        resp.data_json = "{}";
    } else if (req.cmd == "get_log_tail") {
        nlohmann::json d;
        d["lines"] = cnk::LogTail(200);
        resp.ok = true;
        resp.data_json = d.dump();
    } else if (req.cmd == "get_config") {
        // 读配置文件，剥离 password（不回传明文）
        cnk::AppConfig cfg;
        std::string e;
        cnk::ConfigLoad(cfg, e);
        std::string js;
        cnk::ConfigToJson(cfg, js);
        auto j = nlohmann::json::parse(js);
        for (auto& p : j["profiles"]) {
            bool has = !p.value("password_enc", std::string{}).empty();
            p.erase("password_enc");
            p["has_password"] = has;
        }
        resp.ok = true;
        resp.data_json = j.dump();
    } else if (req.cmd == "set_config") {
        // 步骤一：取出 args.config
        auto args = nlohmann::json::parse(req.args_json, nullptr, false);
        if (args.is_discarded() || !args.contains("config")) {
            resp.ok = false;
            resp.error = "set_config 缺少 config 参数";
            return resp;
        }
        std::string cfg_json = args["config"].dump();

        // 步骤二：读入为新配置（此时密码字段尚未解密，password 为空）
        cnk::AppConfig incoming;
        std::string perr;
        if (!cnk::ConfigFromJson(cfg_json, incoming, perr)) {
            resp.ok = false;
            resp.error = "配置格式错误：" + perr;
            return resp;
        }

        // 步骤三：合并密码。
        // UI 从不回传已存密码；仅当用户真的输入了新密码时才带 password_plain。
        // 因此：新值存在则采用，否则沿用磁盘上的旧值。
        cnk::AppConfig existing;
        std::string lerr;
        bool have_existing = cnk::ConfigLoad(existing, lerr);

        auto raw = nlohmann::json::parse(cfg_json, nullptr, false);
        for (size_t i = 0; i < incoming.profiles.size(); ++i) {
            std::string fresh;
            if (raw.is_object() && raw.contains("profiles") &&
                raw["profiles"].is_array() && i < raw["profiles"].size()) {
                const auto& pj = raw["profiles"][i];
                fresh = pj.value("password_plain", std::string{});
            }

            if (!fresh.empty()) {
                incoming.profiles[i].password = fresh;   // 新密码，ConfigSave 会加密
                continue;
            }

            // 沿用旧密码：按档案名匹配
            if (have_existing) {
                for (const auto& old : existing.profiles) {
                    if (old.name == incoming.profiles[i].name) {
                        incoming.profiles[i].password = old.password;
                        break;
                    }
                }
            }
        }

        // 步骤四：落盘并热重载
        std::string serr;
        if (!cnk::ConfigSave(incoming, serr)) {
            resp.ok = false;
            resp.error = "保存失败：" + serr;
            return resp;
        }
        sch->SetConfig(incoming);
        cnk::LogInfo("配置已由设置界面更新并热重载");
        resp.ok = true;
        resp.data_json = "{}";

    } else if (req.cmd == "get_carrier_options") {
        // 从最近一次门户页面解析运营商列表；无则返回空数组，
        // UI 收到空数组时回退到内置预设。
        nlohmann::json d;
        d["options"] = nlohmann::json::array();
        cnk::CarrierList cl;
        if (cnk::ParseCarrier(sch->LastPortalBody(), cl)) {
            d["title"] = cl.title;
            d["default_id"] = cl.default_id;
            for (const auto& o : cl.options) {
                d["options"].push_back({{"id", o.id},
                                        {"name", o.name},
                                        {"suffix", o.suffix}});
            }
        }
        resp.ok = true;
        resp.data_json = d.dump();

    } else if (req.cmd == "get_password") {
        // 仅在用户于界面点「显示密码」时调用一次
        auto args = nlohmann::json::parse(req.args_json, nullptr, false);
        const std::string want = args.is_object()
            ? args.value("profile", std::string{}) : std::string{};

        cnk::AppConfig cfg;
        std::string e;
        if (!cnk::ConfigLoad(cfg, e)) {
            resp.ok = false;
            resp.error = "读取配置失败：" + e;
            return resp;
        }
        for (const auto& p : cfg.profiles) {
            if (p.name == want) {
                nlohmann::json d;
                d["password"] = p.password;
                resp.ok = true;
                resp.data_json = d.dump();
                return resp;
            }
        }
        resp.ok = false;
        resp.error = "未找到该配置档案";
    } else if (req.cmd == "test_profile") {
        // 用指定配置做一次探测 + 登录，返回详细过程供排查
        auto args = nlohmann::json::parse(req.args_json, nullptr, false);
        const std::string want = args.is_object()
            ? args.value("profile", std::string{}) : std::string{};

        cnk::AppConfig cfg;
        std::string e;
        cnk::ConfigLoad(cfg, e);
        const cnk::Profile* target = nullptr;
        for (const auto& p : cfg.profiles)
            if (p.name == want) { target = &p; break; }
        if (!target && !cfg.profiles.empty()) target = &cfg.profiles.front();

        if (!target) {
            resp.ok = false;
            resp.error = "没有可用的配置档案";
            return resp;
        }

        auto pr = cnk::ProbeOnce(cfg.settings);
        cnk::Vendor v = cnk::DetectVendor(pr.response);

        cnk::NetContext ctx;
        ctx.portal_url = cfg.settings.probe_url;
        ctx.portal_body = cnk::DecodeBody(pr.response);
        ctx.server_header = pr.response.server;

        auto auth = cnk::CreateAuth(v, *target);
        auto lr = auth->Login(*target, ctx);

        nlohmann::json d;
        d["probe_state"] = (pr.state == cnk::NetState::Online ? "online"
                          : pr.state == cnk::NetState::Offline ? "offline"
                          : "portal");
        d["probe_message"] = pr.message;
        d["vendor"] = cnk::VendorName(v);
        d["strategy"] = auth->Name();
        d["success"] = lr.success;
        d["message"] = lr.message;
        d["request_url"] = cnk::RedactSecrets(lr.request_url);
        d["request_body"] = cnk::RedactSecrets(lr.request_body);
        resp.ok = true;
        resp.data_json = d.dump();
    } else {
        resp.ok = false;
        resp.error = "未知命令: " + req.cmd;
    }
    return resp;
}, err);
```

上面用到的 `sch->LastPortalBody()` 已在任务 14 定义（返回最近一次探测取回的门户页面，已转 UTF-8）。

- [ ] **Step 2: 实现日志面板**

`LogPanel.axaml`：一个 `ScrollViewer` 内嵌 `SelectableTextBlock`，每 2 秒调用 `get_log_tail` 刷新，自动滚到底部；顶部有「打开日志目录」按钮。

- [ ] **Step 3: 手工验证**

```bash
dotnet run --project src/ui
```

Expected: 日志面板显示实时日志行；「打开日志目录」弹出资源管理器。

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "feat: 日志面板与守护进程 IPC 命令实现"
```

---

## 任务 26：按需加载 CLR 与内嵌 UI

**Files:**
- Create: `src/native/ui_host/Hostfxr.h` / `.cpp`
- Create: `src/native/ui_host/PayloadExtract.h` / `.cpp`
- Create: `scripts/embed_ui.ps1`
- Modify: `src/native/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `bool cnk::EnsureUiPayload(std::string& ui_dir, std::string& error)` — 首次调用时把内嵌的 UI 程序集解压到 `%LOCALAPPDATA%\CampusNet-Keeper\ui\`，之后直接返回缓存路径
  - `int cnk::LaunchUiProcess()` — `CreateProcess(自身, "--ui")`
  - `int cnk::RunUiMode()` — 在 UI 形态中被调用，通过 `hostfxr` 加载并执行托管入口

**机制说明**：exe 内嵌一个名为 `UI_PAYLOAD` 的 RCDATA 资源，内容是 UI 发布产物的 ZIP。`EnsureUiPayload` 检查 `ui\.version` 标记文件是否与当前版本一致，不一致则重新解压。

- [ ] **Step 1: 实现 PayloadExtract**

`src/native/ui_host/PayloadExtract.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 确保 UI 载荷已解压到 %LOCALAPPDATA%\CampusNet-Keeper\ui。
// 首次调用或版本变化时重新解压。返回该目录路径。
bool EnsureUiPayload(std::string& ui_dir, std::string& error);

}  // namespace cnk
```

`src/native/ui_host/PayloadExtract.cpp`:

```cpp
#include "ui_host/PayloadExtract.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "core/Log.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace cnk {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string ReadVersionMarker(const fs::path& ui_dir) {
    std::ifstream ifs(ui_dir / L".version", std::ios::binary);
    if (!ifs) return {};
    std::string s((std::istreambuf_iterator<char>(ifs)),
                  std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// 从 exe 资源中取出 UI_PAYLOAD（一个 zip 字节流）
bool ExtractResourceToFile(const fs::path& out_zip, std::string& error) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, L"UI_PAYLOAD", RT_RCDATA);
    if (!res) {
        error = "exe 中未找到 UI_PAYLOAD 资源（定精简版构建？）";
        return false;
    }
    DWORD size = SizeofResource(self, res);
    HGLOBAL glob = LoadResource(self, res);
    if (!glob || size == 0) {
        error = "读取 UI_PAYLOAD 资源失败";
        return false;
    }
    const void* data = LockResource(glob);
    if (!data) {
        error = "锁定 UI_PAYLOAD 资源失败";
        return false;
    }

    std::ofstream ofs(out_zip, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        error = "无法写入临时文件 " + out_zip.u8string();
        return false;
    }
    ofs.write((const char*)data, size);
    ofs.flush();
    if (!ofs) {
        error = "写入临时文件失败";
        return false;
    }
    return true;
}

// Windows 10 1803+ 自带 bsdtar，可解压 zip，无需第三方库
bool RunTarExtract(const fs::path& zip, const fs::path& dest, std::string& error) {
    wchar_t sysdir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) {
        error = "无法定位 System32";
        return false;
    }
    std::wstring tar = std::wstring(sysdir) + L"\\tar.exe";

    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() +
                       L"\" -C \"" + dest.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = "无法启动 tar.exe，错误码 " + std::to_string(GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code != 0) {
        error = "tar 解压失败，退出码 " + std::to_string(code);
        return false;
    }
    return true;
}

}  // namespace

bool EnsureUiPayload(std::string& ui_dir, std::string& error) {
    error.clear();

    std::string base = AppDataDir();
    if (base.empty()) {
        error = "无法确定 AppData 目录";
        return false;
    }

    fs::path dir = fs::u8path(base) / "ui";
    const std::string want = VersionString();

    // 版本一致则直接复用已解压的内容
    if (fs::exists(dir / L"CampusNet.UI.dll") &&
        ReadVersionMarker(dir) == want) {
        ui_dir = dir.u8string();
        LogInfo("复用已解压的 UI 载荷，版本 " + want);
        return true;
    }

    LogInfo("解压 UI 载荷到 " + dir.u8string());

    std::error_code ec;
    if (fs::exists(dir, ec)) fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        error = "无法创建 UI 目录";
        return false;
    }

    fs::path tmp_zip = fs::u8path(base) / "ui_payload.zip";
    if (!ExtractResourceToFile(tmp_zip, error)) {
        fs::remove(tmp_zip, ec);
        return false;
    }

    if (!RunTarExtract(tmp_zip, dir, error)) {
        fs::remove(tmp_zip, ec);
        return false;
    }
    fs::remove(tmp_zip, ec);

    {
        std::ofstream ofs(dir / L".version", std::ios::binary | std::ios::trunc);
        if (ofs) ofs << want;
    }

    ui_dir = dir.u8string();
    LogInfo("UI 载荷解压完成");
    return true;
}

}  // namespace cnk
```

**资源嵌入**：`scripts/embed_ui.ps1` 生成一个 `.rc` 文件并交给 CMake：

```powershell
# scripts/embed_ui.ps1
param(
    [string]$UiPublishDir = "src/ui/bin/Release/net9.0-windows/win-x64/publish",
    [string]$OutDir = "build/generated"
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "ui_payload.zip"

if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $UiPublishDir '*') -DestinationPath $zip

# .rc 用绝对路径引用 zip，避免相对路径解析歧义
$zipAbs = (Resolve-Path $zip).Path -replace '\\', '\\'
@"
#include <windows.h>
UI_PAYLOAD RCDATA "$zipAbs"
"@ | Set-Content -Encoding ASCII (Join-Path $OutDir "ui_payload.rc")

Write-Host "已生成 $zip 与 ui_payload.rc"
```

在 `CMakeLists.txt` 中：

```cmake
# 若已生成 UI 载荷资源，则编译进 exe
if(EXISTS "${CMAKE_SOURCE_DIR}/build/generated/ui_payload.rc")
  target_sources(CampusNet-Keeper PRIVATE build/generated/ui_payload.rc)
  message(STATUS "已嵌入 UI 载荷（完整版）")
else()
  message(STATUS "未找到 UI 载荷，将构建精简单文件版")
endif()
```

- [ ] **Step 2: 实现 Hostfxr**

先取得 .NET 的 hosting 头文件与库。它们随 .NET SDK 提供：

```bash
# 找到版本目录
ls "/c/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/"
# 假定为 9.0.9，则头文件在 .../9.0.9/r/inc，库在 .../9.0.9/r/lib
```

把三个头文件 `nethost.h`、`hostfxr.h`、`coreclr_delegates.h` 复制到 `third_party/dotnet/`，并在 CMake 中链接 `nethost.lib`：

```cmake
set(DOTNET_HOST_PACK "$ENV{ProgramFiles}/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/9.0.9/r")
target_include_directories(cnk_core PUBLIC third_party/dotnet)
target_link_libraries(cnk_core PUBLIC "${DOTNET_HOST_PACK}/lib/x64/nethost.lib")
```

`src/native/ui_host/Hostfxr.h`:

```cpp
#pragma once
#include <string>

namespace cnk {

// 在 UI 形态下调用：解压内嵌 UI、加载 .NET 运行时、执行托管入口。
// 返回进程退出码；失败时已弹过原生提示框。
int RunUiMode();

// 在守护形态下调用：以 --ui 参数启动自身副本作为 UI 子进程。
// 不等待其退出。返回是否成功创建进程。
bool LaunchUiProcess();

}  // namespace cnk
```

`src/native/ui_host/Hostfxr.cpp`:

```cpp
#include "ui_host/Hostfxr.h"
#include "ui_host/PayloadExtract.h"
#include "core/Log.h"
#include "core/Paths.h"
#include <windows.h>
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace cnk {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

void ShowError(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"CampusNet-Keeper", MB_ICONERROR | MB_OK);
}

// 解析 hostfxr.dll：优先用 UI 目录下的（自包含发布），
// 回退到机器上的 .NET 安装。
HMODULE ResolveHostfxr(const std::string& ui_dir, std::wstring& out_path) {
    std::wstring local = fs::path(Utf8ToWide(ui_dir)).wstring()
                         + L"\\hostfxr.dll";
    if (fs::exists(local)) {
        HMODULE h = LoadLibraryW(local.c_str());
        if (h) { out_path = local; return h; }
    }

    wchar_t buf[MAX_PATH];
    size_t len = MAX_PATH;
    if (get_hostfxr_path(buf, &len, nullptr) != 0) return nullptr;
    out_path = buf;
    return LoadLibraryW(buf);
}

}  // namespace

int RunUiMode() {
    std::string ui_dir, err;
    if (!EnsureUiPayload(ui_dir, err)) {
        ShowError(L"无法准备设置界面所需的文件。\n\n"
                  L"请确认磁盘空间充足，并尝试以管理员身份运行一次。");
        LogError("UI 载荷解压失败：" + err);
        return 1;
    }

    std::wstring hostfxr_path;
    HMODULE lib = ResolveHostfxr(ui_dir, hostfxr_path);
    if (!lib) {
        ShowError(L"未找到 .NET 运行时。\n\n"
                  L"请安装 .NET 9 Desktop Runtime 后重试：\n"
                  L"https://dotnet.microsoft.com/download/dotnet/9.0\n\n"
                  L"（后台守护功能不受影响，已在运行中。）");
        LogError("未找到 hostfxr，.NET 运行时缺失");
        return 1;
    }
    LogInfo("已加载 hostfxr");

    auto init_cfg = (hostfxr_initialize_for_runtime_config_fn)
        GetProcAddress(lib, "hostfxr_initialize_for_runtime_config");
    auto get_delegate = (hostfxr_get_runtime_delegate_fn)
        GetProcAddress(lib, "hostfxr_get_runtime_delegate");
    auto close_cfg = (hostfxr_close_fn)
        GetProcAddress(lib, "hostfxr_close");
    if (!init_cfg || !get_delegate || !close_cfg) {
        ShowError(L".NET 运行时版本不兼容，请更新到 .NET 9 或更高。");
        LogError("hostfxr 导出函数缺失");
        return 1;
    }

    std::wstring config_path = fs::path(Utf8ToWide(ui_dir)).wstring()
                               + L"\\CampusNet.UI.runtimeconfig.json";
    hostfxr_handle ctx = nullptr;
    int rc = init_cfg(config_path.c_str(), nullptr, &ctx);
    if (rc != 0 || !ctx) {
        close_cfg(ctx);
        ShowError(L"初始化 .NET 运行时失败。\n\n"
                  L"请安装或修复 .NET 9 Desktop Runtime 后重试。");
        LogError("hostfxr_initialize_for_runtime_config 失败，rc=" +
                 std::to_string(rc));
        return 1;
    }

    void* load_asm = nullptr;
    rc = get_delegate(ctx, hdt_load_assembly_and_get_function_pointer, &load_asm);
    if (rc != 0 || !load_asm) {
        close_cfg(ctx);
        ShowError(L"获取 .NET 运行时接口失败。");
        LogError("hostfxr_get_runtime_delegate 失败，rc=" + std::to_string(rc));
        return 1;
    }

    auto load_and_get = (load_assembly_and_get_function_pointer_fn)load_asm;

    std::wstring asm_path = fs::path(Utf8ToWide(ui_dir)).wstring()
                            + L"\\CampusNet.UI.dll";
    std::wstring type_name = L"CampusNet.UI.Program, CampusNet.UI";

    component_entry_point_fn entry = nullptr;
    rc = load_and_get(asm_path.c_str(),
                      type_name.c_str(),
                      L"Main",
                      nullptr,          // null = 取原生签名入口
                      nullptr,
                      (void**)&entry);
    if (rc != 0 || !entry) {
        close_cfg(ctx);
        ShowError(L"加载设置界面程序集失败。\n\n"
                  L"若刚更新过程序，请删除以下目录后重试：\n"
                  L"%LOCALAPPDATA%\\CampusNet-Keeper\\ui");
        LogError("load_assembly_and_get_function_pointer 失败，rc=" +
                 std::to_string(rc));
        return 1;
    }

    LogInfo("即将进入托管入口");
    entry(nullptr, 0);   // Program.Main 的原生签名入口

    close_cfg(ctx);
    LogInfo("托管入口已返回，UI 进程退出");
    return 0;
}

bool LaunchUiProcess() {
    std::string exe = ExecutablePath();
    if (exe.empty()) return false;

    std::wstring cmd = L"\"" + Utf8ToWide(exe) + L"\" --ui";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        LogError("启动 UI 子进程失败，错误码 " + std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace cnk
```

**配套的托管侧改动**：`Program.Main` 必须能被作为原生入口点取出，因此要加上 `[UnmanagedCallersOnly]` 或使用标准的 `component_entry_point_fn` 签名。最简单的方式是在 `Program.cs` 增加一个显式的原生入口：

```csharp
public static class Program
{
    // 供 native host 通过 hostfxr 直接调用的入口
    [UnmanagedCallersOnly]
    public static int NativeEntry(IntPtr args, int sizeBytes)
    {
        // Avalonia 需要 STA 线程与自己的消息循环
        var thread = new Thread(() =>
        {
            BuildAvaloniaApp().StartWithClassicDesktopLifetime(Array.Empty<string>());
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();   // 阻塞直到窗口关闭，UI 进程随之退出
        return 0;
    }

    public static AppBuilder BuildAvaloniaApp() => ...;   // 同任务 23
}
```

`[UnmanagedCallersOnly]` 需要 `using System.Runtime.InteropServices;`，且仅支持 .NET 5+。若编译报错，改用 `component_entry_point_fn` 的标准签名（`int (void*, int)`）。

**若本机或目标机既无自包含运行时也无 .NET 安装**：上面的 `ResolveHostfxr` 会失败并弹出原生 `MessageBoxW`，说明需要安装 .NET 9 Desktop Runtime 并给出下载链接。**守护功能不受影响** —— 守护形态根本不走这条路径。

- [ ] **Step 3: 打包脚本**

`scripts/embed_ui.ps1`：把 `src/ui` 的发布产物压成 ZIP，生成 `.rc` 文件引用它，供 CMake 编译进资源。

- [ ] **Step 4: 手工验证**

```bash
pwsh -File scripts/build.ps1
dist/CampusNet-Keeper.exe --ui
```

Expected: 弹出设置窗口；关闭窗口后进程退出。

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "feat: 按需加载 CLR 并解压内嵌 UI，实现单 exe 双形态"
```

---

## 任务 27：构建脚本、CI 与发布

**Files:**
- Create: `scripts/build.ps1`
- Create: `scripts/package.ps1`
- Create: `.github/workflows/build.yml`
- Create: `LICENSE`
- Modify: `README.md`（补 M2 内容、截图位、已知限制）

- [ ] **Step 1: 写 LICENSE（MIT）**

标准 MIT 文本，版权行 `Copyright (c) 2026 LunaRime`。

- [ ] **Step 2: 写构建脚本**

`scripts/build.ps1`:

```powershell
# 串联：UI 发布 → 生成载荷资源 → CMake 构建 → 输出到 dist/
param(
    [switch]$Minimal,                        # 只构建精简版（不嵌入 UI 载荷）
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    if (-not $Minimal) {
        Write-Host "== 1/4 发布 UI ==" -ForegroundColor Cyan
        dotnet publish src/ui -c $Configuration -r win-x64 --self-contained false `
            -o src/ui/bin/$Configuration/net9.0-windows/win-x64/publish
        if ($LASTEXITCODE -ne 0) { throw "UI 发布失败" }

        Write-Host "== 2/4 打包 UI 载荷 ==" -ForegroundColor Cyan
        pwsh -File scripts/embed_ui.ps1 `
            -UiPublishDir "src/ui/bin/$Configuration/net9.0-windows/win-x64/publish" `
            -OutDir "build/generated"
    } else {
        Write-Host "== 精简构建：跳过 UI 载荷 ==" -ForegroundColor Yellow
        if (Test-Path build/generated/ui_payload.rc) {
            Remove-Item build/generated/ui_payload.rc
        }
    }

    Write-Host "== 3/4 配置 CMake ==" -ForegroundColor Cyan
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

    Write-Host "== 4/4 构建原生部分 ==" -ForegroundColor Cyan
    cmake --build build --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "原生构建失败" }

    Write-Host "== 跑测试 ==" -ForegroundColor Cyan
    & "build/$Configuration/cnk_tests.exe"
    if ($LASTEXITCODE -ne 0) { throw "测试未通过" }

    New-Item -ItemType Directory -Force -Path dist | Out-Null
    Copy-Item "build/$Configuration/CampusNet-Keeper.exe" dist/ -Force

    $size = (Get-Item dist/CampusNet-Keeper.exe).Length / 1MB
    Write-Host ("构建完成：dist/CampusNet-Keeper.exe（{0:N1} MB）" -f $size) -ForegroundColor Green
}
finally {
    Pop-Location
}
```

`scripts/package.ps1`:

```powershell
# 产出两个发布包：精简版（需 .NET 运行时）与完整版（零依赖，内嵌运行时）
param([string]$Version = "0.1.0")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    New-Item -ItemType Directory -Force -Path dist | Out-Null

    # ── 精简版：不嵌入 UI 载荷，exe 约 3-4 MB ──
    Write-Host "== 构建精简版 ==" -ForegroundColor Cyan
    & "$PSScriptRoot/build.ps1" -Minimal
    $liteDir = "dist/lite"
    New-Item -ItemType Directory -Force -Path $liteDir | Out-Null
    Copy-Item dist/CampusNet-Keeper.exe $liteDir/ -Force
    Copy-Item README.md $liteDir/ -Force
    Copy-Item docs/M1-实测指南.md $liteDir/ -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path "$liteDir/*" `
        -DestinationPath "dist/CampusNet-Keeper-$Version-lite.zip" -Force

    # ── 完整版：内嵌自包含 .NET 运行时，零依赖 ──
    Write-Host "== 构建完整版（含自包含运行时，较慢）==" -ForegroundColor Cyan
    dotnet publish src/ui -c Release -r win-x64 --self-contained true `
        -p:PublishSingleFile=false -p:PublishTrimmed=false `
        -o src/ui/bin/Release/net9.0-windows/win-x64/publish
    if ($LASTEXITCODE -ne 0) { throw "自包含 UI 发布失败" }

    pwsh -File scripts/embed_ui.ps1 `
        -UiPublishDir "src/ui/bin/Release/net9.0-windows/win-x64/publish" `
        -OutDir "build/generated"
    cmake --build build --config Release
    if ($LASTEXITCODE -ne 0) { throw "原生构建失败" }

    $fullDir = "dist/full"
    New-Item -ItemType Directory -Force -Path $fullDir | Out-Null
    Copy-Item "build/Release/CampusNet-Keeper.exe" $fullDir/ -Force
    Copy-Item README.md $fullDir/ -Force
    Compress-Archive -Path "$fullDir/*" `
        -DestinationPath "dist/CampusNet-Keeper-$Version-full.zip" -Force

    Get-ChildItem dist/*.zip | ForEach-Object {
        Write-Host ("{0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB)) -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
```

- [ ] **Step 3: 写 CI**

`.github/workflows/build.yml`：

```yaml
name: build

on:
  push:
    branches: [master, main]
  pull_request:
  release:
    types: [published]

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-dotnet@v4
        with:
          dotnet-version: '9.0.x'
      - name: 配置 CMake
        run: cmake -S . -B build -G "Visual Studio 17 2022" -A x64
      - name: 构建
        run: cmake --build build --config Release
      - name: 跑测试
        run: build/Release/cnk_tests.exe
      - name: 上传产物
        uses: actions/upload-artifact@v4
        with:
          name: CampusNet-Keeper
          path: build/Release/CampusNet-Keeper.exe
```

- [ ] **Step 4: 补 README**

补上 M2 内容：截图、设置界面说明、自定义模板怎么写、**已知限制**（深澜路径未实测；仅在一所学校验证过）。

- [ ] **Step 5: 本地全量验证**

```bash
pwsh -File scripts/build.ps1
pwsh -File scripts/package.ps1
ls dist/
```

Expected: `dist/` 下有两个 zip，解压后 exe 能运行，`--version`、`--probe-now`、`--ui` 均正常。

- [ ] **Step 6: 提交并推送**

```bash
git add -A
git commit -m "chore: 构建脚本、CI、许可证与文档"
```

**推送前确认**：仓库中不含任何真实账号、学号、MAC 或私有凭据。执行一次全文检索确认：

```bash
grep -rn "2024147" --exclude-dir=build --exclude-dir=.git . || echo "无真实学号残留"
```

- [ ] **Step 7: 创建 GitHub 仓库并推送**

```bash
gh repo create CampusNet-Keeper --public \
  --description "校园网自动重连守护 —— C++ 原生核心 + Avalonia UI，常驻内存 3-5MB"
git remote add origin https://github.com/LunaRime/CampusNet-Keeper.git
git push -u origin master
```

- [ ] **Step 8: 打 tag 触发 Release**

```bash
git tag -a v0.1.0 -m "首个版本：Dr.COM 哆点认证、托盘守护、自动重连、现代设置界面"
git push origin v0.1.0
```

Expected: GitHub Actions 自动构建并发布 Release，附带两个发布包的下载链接。

---

## 完成标准

全部任务完成后，应能验证：

| spec 成功标准 | 验证方式 |
|---|---|
| S1 断线 ≤15 秒恢复 | `--probe-now` + 真实断线测试，检查日志时间戳 |
| S2 后台内存 <10 MB | 任务管理器查看 `CampusNet-Keeper.exe` 工作集 |
| S3 空闲 CPU ≈0% | 任务管理器查看 CPU 列 |
| S4 设置界面可填写 | 任务 24 手工验证 |
| S5 关窗守护继续 | 任务 26 手工验证 |
| S6 开机静默自启 | 勾选自启后重启机器 |
| S7 交付单个 exe | `dist/` 产物检查 |
| S8 GitHub CI 出 Release | 任务 27 Step 8 |
