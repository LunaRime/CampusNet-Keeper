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
        "upass", "password", "pwd", "pass", "user_password", "passwd",
        // Dr.COM 的账号字段：学号属敏感信息，日志/诊断输出里与密码同等对待
        "ddddd"
    };
    // ASCII 大小写折叠后比较，键名大小写不影响脱敏。
    std::string lower;
    lower.reserve(key.size());
    for (char c : key)
        lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    for (auto* k : kKeys)
        if (lower == k) return true;
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

        // '?' 与 '&' 同为参数分隔符，否则内嵌查询串（如 url=http://x?upass=y）
        // 会被当成单个值整体吃掉，其中的凭据不会被脱敏。
        size_t val_end = s.find_first_of("&?", eq);
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
