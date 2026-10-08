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
