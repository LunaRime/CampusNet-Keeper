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

fs::path LocalAppDataPathRaw() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        return {};
    std::wstring w(raw);
    CoTaskMemFree(raw);
    return fs::path(w) / L"CampusNet-Keeper";
}

// 创建目录（含父目录）。成功或"已存在"都算成功。
bool EnsureDir(const fs::path& p) {
    if (p.empty()) return false;
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return false;
    return fs::is_directory(p, ec);
}

std::string ToUtf8OrEmpty(const fs::path& p) {
    return p.empty() ? std::string() : WideToUtf8(p.wstring());
}

}  // namespace

std::string AppDataDir() {
    fs::path p = AppDataPath();
    if (!EnsureDir(p)) return {};
    return ToUtf8OrEmpty(p);
}

std::string AppDataSubDir(const std::string& name) {
    std::string base = AppDataDir();
    if (base.empty()) return {};
    fs::path p = fs::path(base) / fs::u8path(name);
    if (!EnsureDir(p)) return {};
    return ToUtf8OrEmpty(p);
}

std::string LocalAppDataPath() {
    return ToUtf8OrEmpty(LocalAppDataPathRaw());
}

std::string LocalAppDataDir() {
    fs::path p = LocalAppDataPathRaw();
    if (!EnsureDir(p)) return {};
    return ToUtf8OrEmpty(p);
}

std::string LocalAppDataSubDir(const std::string& name) {
    std::string base = LocalAppDataDir();
    if (base.empty()) return {};
    fs::path p = fs::path(base) / fs::u8path(name);
    if (!EnsureDir(p)) return {};
    return ToUtf8OrEmpty(p);
}

}  // namespace cnk
