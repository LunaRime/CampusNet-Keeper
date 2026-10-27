#include "core/LastExit.h"
#include "core/Paths.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace cnk {

namespace {

std::string g_test_dir;   // 非空时覆盖默认目录（仅测试用）

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// 目录：默认用 %APPDATA%\CampusNet-Keeper（AppDataDir 会确保目录存在）
fs::path DirPath() {
    const std::string dir = g_test_dir.empty() ? AppDataDir() : g_test_dir;
    if (dir.empty()) return {};
    return fs::path(Utf8ToWide(dir));
}

fs::path FilePath() {
    const fs::path d = DirPath();
    if (d.empty()) return {};
    return d / L"last_exit.txt";
}

std::string Timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

}  // namespace

std::string LastExitFilePath() {
    try {
        const fs::path p = FilePath();
        return p.empty() ? std::string() : WideToUtf8(p.wstring());
    } catch (...) {
        return {};
    }
}

void SetLastExitDirForTest(const std::string& dir) { g_test_dir = dir; }

LastExit TakeLastExit() {
    LastExit out;
    try {
        const fs::path p = FilePath();
        if (p.empty()) return out;
        std::error_code ec;
        if (!fs::exists(p, ec)) return out;         // 没有标记 = 上次不是正常退出

        std::vector<std::string> lines;
        {
            std::ifstream in(p);
            std::string line;
            while (std::getline(in, line)) lines.push_back(line);
        }   // ⚠ 必须先关句柄再删：Windows 不允许删除仍被打开的文件，
            // 少了这一步 remove 会静默失败 → 标记一直残留 → "上次是否正常退出"
            // 的判据永久失效（每回都读到陈旧的记录）。由 last_exit_test 抓出。

        if (lines.size() >= 1) out.reason = lines[0];
        if (lines.size() >= 2) out.at = lines[1];
        out.recorded = !out.reason.empty();

        // 取走即删除：本次运行从这里开始，标记由本次重新写。删不掉也照常返回结果
        //（下次启动会再读到同一条 → 至多让"未正常退出"的判定晚一次，不会误判来源）。
        std::error_code ec2;
        fs::remove(p, ec2);
    } catch (...) {
        // 标记文件只是可观测性辅助，任何异常都不能影响守护启动
        out = LastExit{};
    }
    return out;
}

bool RecordExit(const std::string& reason) {
    try {
        const fs::path p = FilePath();
        if (p.empty()) return false;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::trunc);
        if (!out.is_open()) return false;
        out << reason << "\n" << Timestamp() << "\n";
        out.flush();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace cnk
