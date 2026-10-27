#include "sys/Autostart.h"
#include "core/Paths.h"

#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace cnk {

namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValueName = L"CampusNetKeeper";
// 2026-10-27 曾用"脚本启动器"避免开机闪窗；改 GUI 子系统后不再需要，仅作迁移清理。
constexpr const wchar_t* kLegacyLauncherName = L"autostart.vbs";

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

std::wstring LegacyLauncherPath() {
    const std::string dir = LocalAppDataDir();
    if (dir.empty()) return {};
    const std::wstring wdir = ToWide(dir);
    if (wdir.empty()) return {};
    return wdir + L"\\" + kLegacyLauncherName;
}

void RemoveLegacyLauncher() {
    const std::wstring p = LegacyLauncherPath();
    if (!p.empty()) DeleteFileW(p.c_str());
}

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

// 期望值：`"<exe>" --autostart`（--autostart 表示静默启动、不弹设置界面）
std::wstring DesiredValue() {
    const std::wstring self = SelfPath();
    if (self.empty()) return {};
    return L"\"" + self + L"\" --autostart";
}

// 旧形态：`"<系统盘>\...\wscript.exe" //B //Nologo "...\autostart.vbs"`。
// 只用于迁移判断——命中就重写成直接指向 exe，并删掉脚本。
bool LooksLikeLegacyLauncher(const std::wstring& v) {
    const std::wstring p = LegacyLauncherPath();
    if (p.empty()) return false;
    // 不区分大小写地找脚本路径
    std::wstring lower_v = v, lower_p = p;
    for (auto& c : lower_v) c = (wchar_t)towlower(c);
    for (auto& c : lower_p) c = (wchar_t)towlower(c);
    return lower_v.find(lower_p) != std::wstring::npos;
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
        const std::wstring value = DesiredValue();
        if (value.empty()) {
            RegCloseKey(key);
            error = "无法取得自身路径";
            return false;
        }
        rc = RegSetValueExW(key, kValueName, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RemoveLegacyLauncher();
    } else {
        rc = RegDeleteValueW(key, kValueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
        RemoveLegacyLauncher();
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
        // 已经是期望形态就不动注册表：避免每次启动都写（也会打乱 StartUpApproved 的排序偏好）。
        // 旧形态（脚本启动器）需要迁移；指向别的 exe 的残留条目也要覆盖。
        std::wstring value;
        if (ReadRunValue(value) && value == DesiredValue()) return true;
        return SetAutostart(true, error);
    }
    // 关闭：不管之前指向谁，都删掉本程序的自启值（避免残留指向旧路径的死条目）
    return SetAutostart(false, error);
}

bool IsAutostartEnabled() {
    std::wstring value;
    if (!ReadRunValue(value)) return false;

    // 期望形态
    const std::wstring want = DesiredValue();
    if (!want.empty() && value == want) return true;

    // 旧形态（脚本启动器）：仍算"已启用"，下次启动会被迁移成直接指向 exe
    if (LooksLikeLegacyLauncher(value)) return true;

    // 更早的形态：直接指向本 exe 但没有 --autostart 参数
    const std::wstring self = SelfPath();
    if (self.empty()) return false;
    const std::wstring prefix = L"\"" + self + L"\"";
    return value.rfind(prefix, 0) == 0;
}

}  // namespace cnk
