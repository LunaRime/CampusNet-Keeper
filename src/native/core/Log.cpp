#include "core/Log.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <deque>
#include <chrono>
#include <cctype>
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
        "ddddd",
        // 新式 API（2026-10-16）的账号字段与页面令牌：学号与 rcn 同样不回显
        "user_account", "rcn",
        // 通用表单的账号字段：任务 21 的 GenericFormAuth 会用它提交学号
        "username"
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

// JSON 载荷里的凭据：把 "key":"value" / "key":value 形式的敏感值替换为 ***。
// 2026-10-16 审阅 P1：新式 API 的请求体是 JSON，而下面的 key=value 主循环只认
// '='，对 JSON 完全失效（学号与页面令牌会被原样打印）。这里做一遍前置替换，
// 其余文本原样保留，不影响既有的表单/查询串脱敏。
std::string RedactJsonPairs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '"') {
            out += s[i++];
            continue;
        }
        const size_t key_end = s.find('"', i + 1);
        if (key_end == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        const std::string key = s.substr(i + 1, key_end - i - 1);
        size_t j = key_end + 1;
        while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
        if (j >= s.size() || s[j] != ':') {  // 不是键值对：普通字符串，原样保留
            out.append(s, i, key_end - i + 1);
            i = key_end + 1;
            continue;
        }
        ++j;
        while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;

        out.append(s, i, j - i);  // 键名与冒号原样保留
        if (j < s.size() && s[j] == '"') {
            // 字符串值：扫到未转义的收尾引号（跳过 \" 转义，防值内含引号时截断泄漏）
            size_t k = j + 1;
            while (k < s.size()) {
                if (s[k] == '\\') {
                    k += 2;
                    continue;
                }
                if (s[k] == '"') break;
                ++k;
            }
            if (k >= s.size()) {  // 未闭合：整段按敏感处理，不冒险回显
                out += "\"***\"";
                break;
            }
            out += IsSecretKey(key) ? "\"***\"" : s.substr(j, k - j + 1);
            i = k + 1;
        } else {
            // 数字/布尔/null：值到 ',' 或 '}' 为止
            size_t k = j;
            while (k < s.size() && s[k] != ',' && s[k] != '}') ++k;
            out += IsSecretKey(key) ? "***" : s.substr(j, k - j);
            i = k;
        }
    }
    return out;
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
    // 先处理 JSON 形式的键值对（新式 API 的请求体），再走下面的 key=value 主循环
    const std::string src = RedactJsonPairs(s);
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        // 找到一个 key= 的起点
        size_t eq = src.find('=', i);
        if (eq == std::string::npos) { out.append(src, i, std::string::npos); break; }

        // key 是从上一个分隔符到 eq 的子串
        size_t key_start = src.find_last_of("?&", eq);
        key_start = (key_start == std::string::npos) ? i : key_start + 1;
        std::string key = src.substr(key_start, eq - key_start);

        // '?' 与 '&' 同为参数分隔符，否则内嵌查询串（如 url=http://x?upass=y）
        // 会被当成单个值整体吃掉，其中的凭据不会被脱敏。
        size_t val_end = src.find_first_of("&?", eq);
        if (val_end == std::string::npos) val_end = src.size();

        out.append(src, i, key_start - i);
        out += key;
        out += "=";
        out += IsSecretKey(key) ? "***" : src.substr(eq + 1, val_end - eq - 1);
        i = val_end;
    }
    return out;
}

}  // namespace cnk
