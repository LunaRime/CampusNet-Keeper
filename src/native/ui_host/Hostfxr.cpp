#include "ui_host/Hostfxr.h"
#include "ui_host/PayloadExtract.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "sys/Autostart.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace cnk {

namespace {

// ---------------------------------------------------------------- .NET 宿主 ABI
//
// 下面这些声明来自 dotnet/runtime 的 hostfxr.h / coreclr_delegates.h（MIT）。
// 这里**只按公开 ABI 重新声明本程序用到的那部分**，不复制任何第三方源码文件、
// 不链接任何第三方库（Global Constraints）。改动前请对照官方头文件。
using hostfxr_handle = void*;

struct hostfxr_initialize_parameters {
    size_t size;
    const wchar_t* host_path;
    const wchar_t* dotnet_root;
};

enum hostfxr_delegate_type {
    hdt_com_activation = 0,
    hdt_load_in_memory_assembly = 1,
    hdt_winrt_activation = 2,
    hdt_com_register = 3,
    hdt_com_unregister = 4,
    hdt_load_assembly_and_get_function_pointer = 5,
    hdt_get_function_pointer = 6,
    hdt_load_assembly = 7,
    hdt_load_assembly_bytes = 8,
};

using hostfxr_initialize_for_runtime_config_fn = int32_t(__cdecl*)(
    const wchar_t* runtime_config_path,
    const hostfxr_initialize_parameters* parameters, hostfxr_handle* host_context_handle);
// 自包含载荷必须走这条：hostfxr_initialize_for_runtime_config 明确不支持自包含组件
//（实测 rc=0x80008093「Initialization for self-contained components is not supported」）。
// 参数形态与 `dotnet CampusNet.UI.dll` 一致，拿到的 host_context 同样能给运行时委托。
using hostfxr_initialize_for_dotnet_command_line_fn = int32_t(__cdecl*)(
    int argc, const wchar_t* const* argv,
    const hostfxr_initialize_parameters* parameters, hostfxr_handle* host_context_handle);
using hostfxr_get_runtime_delegate_fn = int32_t(__cdecl*)(
    hostfxr_handle host_context_handle, hostfxr_delegate_type type, void** delegate);
using hostfxr_close_fn = int32_t(__cdecl*)(hostfxr_handle host_context_handle);
using hostfxr_error_writer_fn = void(__cdecl*)(const wchar_t* message);
using hostfxr_set_error_writer_fn =
    hostfxr_error_writer_fn(__cdecl*)(hostfxr_error_writer_fn error_writer);

using load_assembly_and_get_function_pointer_fn = int(__stdcall*)(
    const wchar_t* assembly_path, const wchar_t* type_name, const wchar_t* method_name,
    const wchar_t* delegate_type_name, void* reserved, void** delegate);
using component_entry_point_fn = int(__stdcall*)(void* arg, int32_t arg_size_in_bytes);

// coreclr_delegates.h 里的哨兵：表示"要的是标了 UnmanagedCallersOnly 的方法"
const wchar_t* const kUnmanagedCallersOnlyMethod = (const wchar_t*)-1;

// ---------------------------------------------------------------- hostfxr 的报错文本

std::mutex g_fxr_err_mutex;
std::string g_fxr_error_text;

void __cdecl HostfxrErrorWriter(const wchar_t* message) {
    if (!message) return;
    std::lock_guard<std::mutex> lk(g_fxr_err_mutex);
    if (g_fxr_error_text.size() > 4000) return;   // 防御：不让宿主刷屏
    g_fxr_error_text += WideToUtf8(message);
    g_fxr_error_text += "\n";
}

std::string TakeHostfxrErrors() {
    std::lock_guard<std::mutex> lk(g_fxr_err_mutex);
    std::string s = g_fxr_error_text;
    g_fxr_error_text.clear();
    return s;
}

// ---------------------------------------------------------------- 定位 hostfxr

std::wstring ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[1024] = {};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, &type, buf, &bytes) != ERROR_SUCCESS)
        return {};
    return buf;
}

bool ParseVersion(const std::wstring& s, std::vector<int>& out, int& rank) {
    out.clear();
    rank = 2;   // 无后缀（正式版）优先
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] < L'0' || s[i] > L'9') {
            rank = 1;   // 非数字开头（或数字段之后跟 '-'/'+' 后缀）→ 预发布
            break;
        }
        int v = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
            v = v * 10 + (int)(s[i] - L'0');
            if (v > 100000) return false;   // 版本号不合理，当成非版本目录
            ++i;
        }
        out.push_back(v);
        if (i == s.size()) break;
        if (s[i] == L'.') {
            ++i;
            continue;
        }
        rank = 1;   // 形如 10.0.0-preview.7 / 9.0.0-rc.1
        break;
    }
    return !out.empty();
}

// 在 <root>\host\fxr\<版本>\ 里挑最高版本的 hostfxr.dll。
// 高版本 hostfxr 能加载低版本框架，所以取最高是安全的（框架不匹配会由它自己报错）。
// 版本目录名可能是 "10.0.0-preview.7.…"/"9.0.0-rc.1"：**必须认**，否则只装了预览版
// 运行时的机器会被误报成"没装 .NET"（2026-10-17 审阅 P2-7）；同等数值下正式版优先。
std::wstring FindHighestHostfxr(const std::wstring& dotnet_root) {
    const fs::path fxr_dir = fs::path(dotnet_root) / L"host" / L"fxr";
    std::error_code ec;
    if (!fs::is_directory(fxr_dir, ec)) return {};

    struct Candidate {
        std::vector<int> version;
        int rank = 2;
        std::wstring path;
    };
    std::vector<Candidate> candidates;
    // 刻意不用 range-for：它的自增是**会抛异常**的版本（构造用了 ec 版，++ 不是）
    for (fs::directory_iterator it(fxr_dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e1;
        if (!it->is_directory(e1)) continue;
        const fs::path dll = it->path() / L"hostfxr.dll";
        if (!fs::is_regular_file(dll, e1)) continue;
        Candidate c;
        if (!ParseVersion(it->path().filename().wstring(), c.version, c.rank)) continue;
        c.path = dll.wstring();
        candidates.push_back(std::move(c));
    }
    if (candidates.empty()) return {};
    const auto best = std::max_element(
        candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.version != b.version) return a.version < b.version;
            return a.rank < b.rank;
        });
    return best->path;
}

// <root>\host\fxr\<版本>\hostfxr.dll → <root>
std::wstring DeriveDotnetRoot(const std::wstring& hostfxr_path) {
    fs::path p(hostfxr_path);
    fs::path root = p.parent_path().parent_path().parent_path().parent_path();
    std::error_code ec;
    if (fs::is_directory(root, ec)) return root.wstring();
    return {};
}

std::string JoinForMessage(const std::vector<std::wstring>& roots) {
    std::string s;
    for (const auto& r : roots) {
        if (!s.empty()) s += "、";
        s += WideToUtf8(r);
    }
    return s;
}

// 界面进程活着期间持有的互斥体名（Local\ = 当前会话，无需额外权限）
constexpr const wchar_t* kUiWindowMutex = L"Local\\CampusNet-Keeper-UiWindow";

// 由本进程持有的界面标志。**故意不 CloseHandle**：进程退出时系统回收，
// 这样"界面是否在运行"这件事在整条失败路径上都不需要额外清理代码。
HANDLE g_ui_window_mutex = nullptr;

// true = 本进程可以打开界面；false = 已有界面在运行
bool AcquireUiWindowMutex() {
    if (g_ui_window_mutex) return true;
    HANDLE m = CreateMutexW(nullptr, FALSE, kUiWindowMutex);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);   // 别人持有：不要留在手里，否则会拖住"在运行"的判断
        return false;
    }
    g_ui_window_mutex = m;
    return true;
}

void ShowError(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"CampusNet-Keeper 设置界面",
                MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

std::string RcHex(int rc) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)rc);
    return buf;
}

}  // namespace

bool ResolveHostfxrPath(const std::string& ui_dir, std::string& out_path,
                        std::string& error) {
    out_path.clear();
    error.clear();

    // ① 载荷目录自带（自包含发布：零依赖单 exe 走这条）
    if (!ui_dir.empty()) {
        const fs::path local = fs::u8path(ui_dir) / L"hostfxr.dll";
        std::error_code ec;
        if (fs::is_regular_file(local, ec)) {
            out_path = WideToUtf8(local.wstring());
            return true;
        }
    }

    // ②③④ 机器上的 .NET 安装（顺序与 nethost 的口径一致：先架构特定的，再通用）
    std::vector<std::wstring> roots;
    const wchar_t* env_root_x64 = _wgetenv(L"DOTNET_ROOT_X64");
    if (env_root_x64 && *env_root_x64) roots.push_back(env_root_x64);
    const wchar_t* env_root = _wgetenv(L"DOTNET_ROOT");
    if (env_root && *env_root) roots.push_back(env_root);
    const std::wstring registered = ReadRegistryString(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x64",
        L"InstallLocation");
    if (!registered.empty()) roots.push_back(registered);
    wchar_t program_files[1024] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", program_files, 1024) > 0)
        roots.push_back(std::wstring(program_files) + L"\\dotnet");

    for (const auto& root : roots) {
        const std::wstring dll = FindHighestHostfxr(root);
        if (!dll.empty()) {
            out_path = WideToUtf8(dll);
            return true;
        }
    }

    error = "找不到 hostfxr.dll（.NET 运行时未安装或安装不完整）。已查找：";
    error += roots.empty() ? std::string("（没有可用的 dotnet 根目录）") : JoinForMessage(roots);
    error += "；期望位置形如 <dotnet 根>\\host\\fxr\\<版本>\\hostfxr.dll";
    return false;
}

namespace {

// 真正的实现。可能抛异常（文件名/路径构造、内存分配…）：--ui 是 CREATE_NO_WINDOW 子进程，
// 异常逃出去等于"点了「设置」什么都没发生、日志里也没有一行"，所以外面必须兜住
//（2026-10-17 审阅 P1 的教训：非法 UTF-8 条目名会让 fs::u8path 抛异常）。
int RunUiModeImpl() {
    std::string ui_dir;
    std::string err;
    if (!EnsureUiPayload(ui_dir, err)) {
        LogError("UI 载荷准备失败：" + err);
        ShowError(L"无法准备设置界面所需的文件。\n\n" + Utf8ToWide(err) +
                  L"\n\n（后台守护功能不受影响，已在正常运行。）");
        return 1;
    }

    std::string hostfxr_path;
    if (!ResolveHostfxrPath(ui_dir, hostfxr_path, err)) {
        LogError("UI 宿主解析失败：" + err);
        ShowError(L"未找到 .NET 运行时，无法打开设置界面。\n\n" +
                  Utf8ToWide(err) +
                  L"\n\n请安装 .NET 10 运行时（Runtime 或 Desktop Runtime 均可）后重试：\n"
                  L"https://dotnet.microsoft.com/download/dotnet/10.0\n\n"
                  L"（后台守护功能不受影响，已在正常运行。）");
        return 1;
    }

    const std::wstring hostfxr_w = Utf8ToWide(hostfxr_path);
    // LOAD_WITH_ALTERED_SEARCH_PATH：自包含载荷的 hostpolicy.dll / coreclr.dll 就在
    // hostfxr.dll 旁边，必须让加载器以该目录为基准解析依赖。
    HMODULE lib = LoadLibraryExW(hostfxr_w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) {
        const DWORD e = GetLastError();
        LogError("加载 hostfxr 失败：" + hostfxr_path + "，错误码 " + std::to_string(e));
        ShowError(L"加载 .NET 宿主 hostfxr.dll 失败。\n\n路径：" + hostfxr_w +
                  L"\n错误码：" + std::to_wstring(e) +
                  L"\n\n请重新安装 .NET 10 运行时（Runtime 或 Desktop Runtime 均可）后重试。");
        return 1;
    }
    LogInfo("已加载 hostfxr：" + hostfxr_path);

    const auto init_cfg = (hostfxr_initialize_for_runtime_config_fn)
        GetProcAddress(lib, "hostfxr_initialize_for_runtime_config");
    const auto get_delegate = (hostfxr_get_runtime_delegate_fn)
        GetProcAddress(lib, "hostfxr_get_runtime_delegate");
    const auto close_cfg = (hostfxr_close_fn)GetProcAddress(lib, "hostfxr_close");
    const auto set_error_writer = (hostfxr_set_error_writer_fn)
        GetProcAddress(lib, "hostfxr_set_error_writer");
    if (!init_cfg || !get_delegate || !close_cfg) {
        LogError("hostfxr 缺少导出函数（版本过旧？）：" + hostfxr_path);
        ShowError(L".NET 运行时版本不兼容（hostfxr 缺少所需接口）。\n\n"
                  L"请安装或更新 .NET 10 运行时（Runtime 或 Desktop Runtime 均可）后重试：\n"
                  L"https://dotnet.microsoft.com/download/dotnet/10.0");
        return 1;
    }
    if (set_error_writer) set_error_writer(&HostfxrErrorWriter);
    TakeHostfxrErrors();   // 清掉可能残留的旧文本

    const fs::path ui_path = fs::u8path(ui_dir);
    const fs::path config_path = ui_path / L"CampusNet.UI.runtimeconfig.json";
    const fs::path asm_path = ui_path / L"CampusNet.UI.dll";
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec) || !fs::is_regular_file(asm_path, ec)) {
        const std::string detail = "载荷不完整：缺少 " +
            WideToUtf8(config_path.wstring()) + " 或 " + WideToUtf8(asm_path.wstring());
        LogError(detail);
        ShowError(L"设置界面文件不完整。\n\n" + Utf8ToWide(detail) +
                  L"\n\n请删除下面的目录后重试：\n" +
                  Utf8ToWide(ui_dir));
        return 1;
    }

    hostfxr_initialize_parameters params{};
    params.size = sizeof(params);
    const std::wstring host_path = Utf8ToWide(ExecutablePath());
    params.host_path = host_path.empty() ? nullptr : host_path.c_str();
    // 自包含载荷（运行时随包发布）的标志：目录里有 hostpolicy.dll。
    // 它同时决定两件事：运行时根目录、以及用哪条 hostfxr 初始化 API（见下）。
    const bool self_contained = fs::is_regular_file(ui_path / L"hostpolicy.dll", ec);
    std::wstring dotnet_root;
    if (self_contained) {
        dotnet_root = ui_path.wstring();             // 运行时就在载荷目录里
    } else {
        dotnet_root = DeriveDotnetRoot(hostfxr_w);   // 机器安装：从 hostfxr 路径反推
    }
    params.dotnet_root = dotnet_root.empty() ? nullptr : dotnet_root.c_str();

    hostfxr_handle ctx = nullptr;
    // 自包含载荷走"运行应用"式初始化；框架依赖走组件式初始化。
    // 两条路拿到的都是同一个运行时委托（hdt_load_assembly_and_get_function_pointer）。
    int rc = -1;
    if (self_contained) {
        const auto init_cmd = (hostfxr_initialize_for_dotnet_command_line_fn)
            GetProcAddress(lib, "hostfxr_initialize_for_dotnet_command_line");
        if (!init_cmd) {
            LogError("自包含载荷需要 hostfxr_initialize_for_dotnet_command_line，但该 hostfxr 没有此导出");
            ShowError(L"这个 .NET 宿主版本不支持自包含载荷。\n\n"
                      L"请改用框架依赖版本，或更新 .NET 运行时后重试。");
            return 1;
        }
        const std::wstring app_dll = asm_path.wstring();
        const wchar_t* argv[] = {app_dll.c_str()};
        rc = init_cmd(1, argv, &params, &ctx);
    } else {
        rc = init_cfg(config_path.wstring().c_str(), &params, &ctx);
    }
    // 0 = 成功；1 = Success_HostAlreadyInitialized；2 = Success_DifferentRuntimeProperties
    if (rc != 0 && rc != 1 && rc != 2) {
        const std::string host_detail = TakeHostfxrErrors();
        if (ctx) close_cfg(ctx);
        std::string log = std::string(self_contained
                                          ? "hostfxr_initialize_for_dotnet_command_line 失败，rc="
                                          : "hostfxr_initialize_for_runtime_config 失败，rc=") +
                          RcHex(rc);
        if (!host_detail.empty()) log += "；宿主报错：" + host_detail;
        LogError(log);
        std::wstring hint =
            L"初始化 .NET 运行时失败。\n\n错误码：" + Utf8ToWide(RcHex(rc)) + L"\n";
        if ((unsigned)rc == 0x80008096u)
            hint += L"\n原因：本机缺少所需的 .NET 运行时框架。\n";
        if (!host_detail.empty()) hint += L"\n.NET 宿主输出：\n" + Utf8ToWide(host_detail) + L"\n";
        hint += L"\n请安装 .NET 10 运行时（Runtime 或 Desktop Runtime 均可）后重试：\n"
                L"https://dotnet.microsoft.com/download/dotnet/10.0\n\n"
                L"（后台守护功能不受影响。）";
        ShowError(hint);
        return 1;
    }

    void* load_asm = nullptr;
    const int rc_delegate =
        get_delegate(ctx, hdt_load_assembly_and_get_function_pointer, &load_asm);
    if (rc_delegate != 0 || !load_asm) {
        const std::string host_detail = TakeHostfxrErrors();
        close_cfg(ctx);
        LogError("hostfxr_get_runtime_delegate 失败，rc=" + RcHex(rc_delegate) +
                 (host_detail.empty() ? "" : "；宿主报错：" + host_detail));
        ShowError(L"获取 .NET 运行时接口失败。\n\n错误码：" + Utf8ToWide(RcHex(rc_delegate)) +
                  L"\n\n请安装或修复 .NET 10 运行时（Runtime 或 Desktop Runtime 均可）后重试。");
        return 1;
    }

    const auto load_and_get = (load_assembly_and_get_function_pointer_fn)load_asm;
    component_entry_point_fn entry = nullptr;
    // 方法名 NativeEntry、类型 CampusNet.UI.Program；标了 [UnmanagedCallersOnly]，
    // 因此 delegate_type_name 传哨兵值（见 coreclr_delegates.h）。
    const int rc_entry = load_and_get(asm_path.wstring().c_str(),
                                      L"CampusNet.UI.Program, CampusNet.UI",
                                      L"NativeEntry",
                                      kUnmanagedCallersOnlyMethod, nullptr, (void**)&entry);
    if (rc_entry != 0 || !entry) {
        const std::string host_detail = TakeHostfxrErrors();
        close_cfg(ctx);
        std::string log = "load_assembly_and_get_function_pointer 失败，rc=" + RcHex(rc_entry);
        if (!host_detail.empty()) log += "；宿主报错：" + host_detail;
        LogError(log);
        ShowError(L"加载设置界面程序集失败。\n\n错误码：" + Utf8ToWide(RcHex(rc_entry)) +
                  L"\n\n若刚更新过程序，请删除下面的目录后重试：\n" + Utf8ToWide(ui_dir));
        return 1;
    }

    LogInfo("即将进入托管界面入口");
    const int ui_rc = entry(nullptr, 0);
    LogInfo("托管界面入口已返回，退出码 " + std::to_string(ui_rc));
    close_cfg(ctx);
    return ui_rc;
}

// 把已经打开的设置界面提到前台。
//
// 怎么找到那个窗口：**按进程映像路径**枚举顶层窗口，而不是按标题 FindWindow。
// 标题是 UI 文案的一部分（改一个字、做本地化就失效），而 exe 路径是稳定的。
// 为什么从**本进程**调 SetForegroundWindow：用户刚点了托盘/命令，前台权在本进程手上；
// 由后台进程去抢前台会被系统的前台锁直接吞掉（窗口只在任务栏闪一下）。
struct FindUiWindowCtx {
    std::wstring exe_lower;   // 已转小写，Windows 路径比较不区分大小写
    DWORD self_pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK EnumUiWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindUiWindowCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ctx->self_pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // 跳过工具窗口/被拥有的对话框
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return TRUE;
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
    bool same_exe = false;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        std::wstring other(path, len);
        std::transform(other.begin(), other.end(), other.begin(),
                       [](wchar_t c) { return (wchar_t)towlower(c); });
        same_exe = (other == ctx->exe_lower);
    }
    CloseHandle(proc);
    if (!same_exe) return TRUE;

    ctx->found = hwnd;
    return FALSE;   // 找到一个就停
}

bool ActivateExistingUiWindow() {
    const std::string exe = ExecutablePath();
    if (exe.empty()) return false;
    FindUiWindowCtx ctx;
    ctx.exe_lower = Utf8ToWide(exe);
    std::transform(ctx.exe_lower.begin(), ctx.exe_lower.end(), ctx.exe_lower.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    ctx.self_pid = GetCurrentProcessId();

    EnumWindows(EnumUiWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found) return false;

    // 最小化的先还原，隐藏的（理论上没有）也拉出来
    ShowWindow(ctx.found, IsIconic(ctx.found) ? SW_RESTORE : SW_SHOW);
    const BOOL ok = SetForegroundWindow(ctx.found);
    LogInfo(std::string("已请求把设置界面切换到前台：") +
            (ok ? "成功" : "系统拒绝（窗口已在任务栏闪烁）"));
    return true;
}

}  // namespace

int RunUiMode() {
    // 已经有一个界面在跑就不再开第二个：托盘被连点时不该堆出一串窗口。
    // 但也不能什么都不做——用户点托盘就是"我要看那个窗口"，所以把它提到前台。
    if (!AcquireUiWindowMutex()) {
        if (!ActivateExistingUiWindow())
            LogInfo("已有设置界面在运行，但没有找到它的窗口（本次不再打开新窗口）");
        return 0;
    }
    try {
        return RunUiModeImpl();
    } catch (const std::exception& e) {
        LogError(std::string("打开设置界面时发生未捕获异常：") + e.what());
        ShowError(L"打开设置界面时发生意外错误（已写入日志）：\n\n" + Utf8ToWide(e.what()));
        return 1;
    } catch (...) {
        LogError("打开设置界面时发生未知异常");
        ShowError(L"打开设置界面时发生未知错误（已写入日志）。");
        return 1;
    }
}

bool LaunchUiProcess() {
    const std::string exe = ExecutablePath();
    if (exe.empty()) {
        LogError("无法确定自身可执行文件路径，不能启动设置界面");
        return false;
    }
    const std::wstring cmd = L"\"" + Utf8ToWide(exe) + L"\" --ui";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW：本程序是控制台子系统，不给这个标志会闪出黑窗口
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        LogError("启动设置界面进程失败，错误码 " + std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool UiProcessRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kUiWindowMutex);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

UiPayloadStatus InspectUiPayload() {
    UiPayloadStatus st;
    std::string err;
    const EmbeddedPayload payload = LoadEmbeddedPayload(err);
    st.present = payload.present;
    st.payload_bytes = payload.size;
    st.payload_id = payload.id;

    const std::string base = LocalAppDataPath();   // 只算路径，不创建目录
    if (!base.empty()) {
        const fs::path dir = fs::u8path(base) / L"ui";
        st.ui_dir = dir.u8string();
        std::error_code ec;
        st.extracted = fs::is_regular_file(dir / L"CampusNet.UI.dll", ec);
        st.up_to_date = payload.present && UiPayloadUsable(dir, payload.id);
    }

    std::string hostfxr_error;
    st.hostfxr_found = ResolveHostfxrPath(st.ui_dir, st.hostfxr_path, hostfxr_error);
    st.hostfxr_error = hostfxr_error;
    return st;
}

}  // namespace cnk
