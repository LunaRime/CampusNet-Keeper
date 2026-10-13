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
    if (len <= 0) return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), len, nullptr, nullptr);
    return out;
}

bool SetAutostart(bool enable, std::string& error) {
    error.clear();
    HKEY key = nullptr;
    // 启用与禁用都只需写权限（删除值同样属于写操作）
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                            KEY_SET_VALUE, &key);
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
        if (wn <= 0) {
            RegCloseKey(key);
            error = "路径转换失败";
            return false;
        }
        std::wstring wexe((size_t)wn, L'\0');
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
    if (rc != ERROR_SUCCESS || size == 0 || type != REG_SZ) {
        RegCloseKey(key);
        return false;
    }
    std::wstring buf((size_t)(size / sizeof(wchar_t)), L'\0');
    rc = RegQueryValueExW(key, kValueName, nullptr, nullptr,
                          (BYTE*)buf.data(), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();

    // 校验指向的确实是本程序：exe 被移动/改名后不得误报「已启用」
    std::string exe = ExecutablePath();
    if (exe.empty()) return false;
    int wn = MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), (int)exe.size(),
                                 nullptr, 0);
    if (wn <= 0) return false;
    std::wstring wexe((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), (int)exe.size(),
                        wexe.data(), wn);
    return buf == (L"\"" + wexe + L"\"");
}

}  // namespace cnk
