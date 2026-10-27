#include "sys/Autostart.h"
#include "core/Paths.h"

#include <windows.h>

#include <string>
#include <vector>

namespace cnk {

namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValueName = L"CampusNetKeeper";
constexpr const wchar_t* kLauncherName = L"autostart.vbs";

std::wstring ToWide(const std::string& u8) {
    if (u8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    return w;
}

std::wstring SelfPath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size() - 1) break;
        buf.resize(buf.size() * 2);
    }
    return std::wstring(buf.data());
}

// %LOCALAPPDATA%\CampusNet-Keeper\autostart.vbs（目录由 LocalAppDataDir 确保存在）
std::wstring LauncherPath() {
    const std::string dir = LocalAppDataDir();
    if (dir.empty()) return {};
    const std::wstring wdir = ToWide(dir);
    if (wdir.empty()) return {};
    return wdir + L"\\" + kLauncherName;
}

std::wstring WscriptPath() {
    wchar_t root[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p(root);
        p += L"\\System32\\wscript.exe";
        // 只认存在的文件：某些企业/教育版镜像把 WSH 精简掉了，这时必须退回老写法，
        // 否则会写进一条永远拉不起来的自启项（比闪一下黑窗糟得多）。
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

// 写启动器脚本（UTF-16LE + BOM，路径含中文也能被 WSH 正确解析）。
//
// 为什么要有启动器：本程序是**控制台子系统**，被启动项直接拉起时 Windows 一定会先
// 分配一个控制台；Win11 默认终端是 Windows Terminal，那一下就是肉眼可见的黑窗闪现
// （2026-10-27 用户反馈"每次开机闪一下终端"）。wscript 是 GUI 子系统，由它用窗口样式 0
// 拉起本程序，全程不创建可见控制台，也就不闪。
//
// 用 `--detached` 是因为此时控制台是**隐藏**的而非不存在：带上它就不会再触发
// 「分离重启」（那会白起一个进程）。--autostart 保证静默、不弹设置界面。
bool WriteLauncher(const std::wstring& exe, const std::wstring& launcher) {
    if (exe.empty() || launcher.empty()) return false;

    std::wstring body;
    body += L'\uFEFF';  // UTF-16LE BOM
    body += L"' CampusNet-Keeper 开机自启启动器：由程序生成，删除后下次启动会重建\r\n";
    body += L"Set sh = CreateObject(\"WScript.Shell\")\r\n";
    body += L"sh.Run Chr(34) & \"" + exe +
            L"\" & Chr(34) & \" --autostart --detached\", 0, False\r\n";

    HANDLE h = CreateFileW(launcher.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const DWORD bytes = (DWORD)(body.size() * sizeof(wchar_t));
    const BOOL ok = WriteFile(h, body.data(), bytes, &written, nullptr);
    CloseHandle(h);
    return ok && written == bytes;
}

std::wstring LauncherCommand() {
    const std::wstring host = WscriptPath();
    const std::wstring launcher = LauncherPath();
    if (host.empty() || launcher.empty()) return {};
    return L"\"" + host + L"\" //B //Nologo \"" + launcher + L"\"";
}

bool SameCommand(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// 读当前 Run 键值（不存在返回 false）
bool ReadRunValue(std::wstring& out) {
    out.clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG rc = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || size == 0 || type != REG_SZ) {
        RegCloseKey(key);
        return false;
    }
    std::wstring buf((size_t)(size / sizeof(wchar_t)), L'\0');
    rc = RegQueryValueExW(key, kValueName, nullptr, nullptr, (BYTE*)buf.data(), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = buf;
    return true;
}

}  // namespace

std::string ExecutablePath() {
    const std::wstring w = SelfPath();
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

bool SetAutostart(bool enable, std::string& error) {
    error.clear();
    HKEY key = nullptr;
    // 启用与禁用都只需写权限（删除值同样属于写操作）
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        error = "无法打开自启动注册表项，错误码 " + std::to_string(rc);
        return false;
    }

    if (enable) {
        const std::wstring self = SelfPath();
        if (self.empty()) {
            RegCloseKey(key);
            error = "无法取得自身路径";
            return false;
        }

        // 优先用 wscript 启动器（开机不闪终端）；写不出来就退回"直接指向 exe"的老写法，
        // 宁可闪一下也不让自启失效。
        std::wstring value = LauncherCommand();
        const std::wstring launcher = LauncherPath();
        if (value.empty() || !WriteLauncher(self, launcher)) {
            value = L"\"" + self + L"\" --autostart";
        }

        rc = RegSetValueExW(key, kValueName, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kValueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
        const std::wstring launcher = LauncherPath();
        if (!launcher.empty()) DeleteFileW(launcher.c_str());  // 失败无妨，下次启动会重建
    }
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS) {
        error = "写自启动注册表失败，错误码 " + std::to_string(rc);
        return false;
    }
    return true;
}

bool SyncAutostart(bool desired, std::string& error) {
    error.clear();
    if (desired) {
        // 已经是"启动器形态"且脚本还在 → 不动注册表（避免打乱 StartUpApproved 的排序偏好）。
        // 旧形态（直接指向 exe，会闪黑窗）与"脚本被删掉"都要重写一次。
        std::wstring value;
        if (ReadRunValue(value) && SameCommand(value, LauncherCommand())) {
            const std::wstring launcher = LauncherPath();
            if (!launcher.empty() &&
                GetFileAttributesW(launcher.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return true;
            }
        }
        return SetAutostart(true, error);
    }
    // 关闭：不管之前指向谁，都删掉本程序的自启值（避免残留指向旧路径的死条目）
    return SetAutostart(false, error);
}

bool IsAutostartEnabled() {
    std::wstring value;
    if (!ReadRunValue(value)) return false;

    // 新形态：wscript 启动器
    if (SameCommand(value, LauncherCommand())) return true;

    // 旧形态：直接指向本 exe（升级前的安装留下的，仍算"已启用"）
    const std::wstring self = SelfPath();
    if (self.empty()) return false;
    const std::wstring prefix = L"\"" + self + L"\"";
    return value.rfind(prefix, 0) == 0;
}

}  // namespace cnk
