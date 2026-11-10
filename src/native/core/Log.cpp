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
std::string g_log_dir;
fs::path g_log_path;
std::ofstream g_stream;
std::string g_current_day;      // 当前打开的日志属于哪一天（YYYY-MM-DD）
LogLevel g_min_level = LogLevel::Info;
size_t g_max_bytes = 512 * 1024;
int g_keep_days = 3;
constexpr int kRotateBackups = 3;   // 日内大小轮转备份数（与时间保留期无关）
bool g_rotate_failed = false;
bool g_rotate_reported = false;
int g_rotate_retry = 0;
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

std::string TodayYmd() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// 严格解析 YYYY-MM-DD，全手工，不用 stoi（服务线程里绝不让异常逃出去）。
bool ParseYmd(const std::string& s, int& y, unsigned& m, unsigned& d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (!std::isdigit((unsigned char)s[i])) return false;
    y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    m = (unsigned)((s[5] - '0') * 10 + (s[6] - '0'));
    d = (unsigned)((s[8] - '0') * 10 + (s[9] - '0'));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// 民用日期 → 距 1970-01-01 的天数（纯算术，不受时区 / DST 影响，便于测试独立复算）。
long long DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned mp = (unsigned)((int)m + (m > 2 ? -3 : 9));
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

// 日内大小保护：当天文件超过 max_bytes 才轮转，备份数固定。
void RotateIfNeeded() {
    std::error_code ec;
    if (!fs::exists(g_log_path, ec)) return;
    if ((size_t)fs::file_size(g_log_path, ec) < g_max_bytes) return;

    // 另一进程长期持文件时，每写一行都 close/rename/open 是白费力气（P2-6/N2）
    if (g_rotate_failed && ++g_rotate_retry < 64) return;
    g_rotate_retry = 0;

    g_stream.close();
    for (int i = kRotateBackups - 1; i >= 1; --i) {
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

    g_stream.clear();
    g_stream.open(g_log_path, std::ios::app);
    if (e3 || !g_stream.is_open()) {
        // 本函数在持锁状态下被调用，不能直接 LogWarn（会死锁），只置标志
        g_rotate_failed = true;
    } else {
        g_rotate_failed = false;
        g_rotate_reported = false;
    }
}

// 切到今天的文件，并顺手清理过期日志（启动时与跨天时调用）。
void OpenForToday() {
    const std::string day = TodayYmd();
    g_current_day = day;
    if (g_stream.is_open()) g_stream.close();
    // 测试/异常路径可能传入空目录：只保留内存 tail，绝不把日志文件落到当前工作目录
    //（曾因此把 keeper-YYYY-MM-DD.log 写进仓库根）。
    if (g_log_dir.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(g_log_dir), ec);
    g_log_path = fs::u8path(g_log_dir) / cnk::LogFileNameForDate(day);
    g_stream.clear();
    g_stream.open(g_log_path, std::ios::app);
    g_rotate_failed = !g_stream.is_open();
    g_rotate_reported = false;
    g_rotate_retry = 0;
    cnk::CleanupOldLogs(g_log_dir, day, g_keep_days);
}

bool IsSecretKey(const std::string& key) {
    static const char* kKeys[] = {
        "upass", "password", "pwd", "pass", "user_password", "passwd",
        // Dr.COM 的账号字段：学号属敏感信息，日志/诊断输出里与密码同等对待
        "ddddd",
        // 新式 API（2026-10-16）的账号字段与页面令牌：学号与 rcn 同样不回显
        "user_account", "rcn",
        // 通用表单的账号字段：任务 21 的 GenericFormAuth 会用它提交学号
        "username",
        // 其它厂商/模板常见的账号字段名——GET 表单会把字段拼进 URL，经日志脱敏输出；
        // 漏掉这些名字会让学号原样进日志（2026-10-16 审阅 P1）
        "user", "account", "user_name", "login_name", "userid", "uid"
    };
    std::string lower;
    lower.reserve(key.size());
    for (char c : key)
        lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    for (auto* k : kKeys)
        if (lower == k) return true;
    return false;
}

// JSON 载荷里的凭据：把 "key":"value" / "key":value 形式的敏感值替换为 ***。
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
        if (j >= s.size() || s[j] != ':') {
            out.append(s, i, key_end - i + 1);
            i = key_end + 1;
            continue;
        }
        ++j;
        while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;

        out.append(s, i, j - i);
        if (j < s.size() && s[j] == '"') {
            size_t k = j + 1;
            while (k < s.size()) {
                if (s[k] == '\\') {
                    k += 2;
                    continue;
                }
                if (s[k] == '"') break;
                ++k;
            }
            if (k >= s.size()) {
                out += "\"***\"";
                break;
            }
            out += IsSecretKey(key) ? "\"***\"" : s.substr(j, k - j + 1);
            i = k + 1;
        } else {
            size_t k = j;
            while (k < s.size() && s[k] != ',' && s[k] != '}') ++k;
            out += IsSecretKey(key) ? "***" : s.substr(j, k - j);
            i = k;
        }
    }
    return out;
}

}  // namespace

std::string LogFileNameForDate(const std::string& ymd) {
    return "keeper-" + ymd + ".log";
}

int CleanupOldLogs(const std::string& log_dir, const std::string& today_ymd, int keep_days) {
    int y = 0;
    unsigned m = 0, d = 0;
    if (!ParseYmd(today_ymd, y, m, d) || keep_days < 1) return 0;
    const long long today = DaysFromCivil(y, m, d);

    std::error_code ec;
    fs::directory_iterator it(fs::u8path(log_dir), ec), end;
    if (ec) return 0;
    int removed = 0;
    const std::string prefix = "keeper-";
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        // 迁移前遗留的 keeper.log[.N]：文件名里没有日期，按最后写入时间清理（同样 3 天），
        // 免得它变成永远删不掉的孤岛。刚写完/时间异常的文件不会被误删。
        if (name.rfind("keeper.log", 0) == 0) {
            std::error_code t_ec;
            const auto last = fs::last_write_time(it->path(), t_ec);
            if (!t_ec) {
                const auto age = fs::file_time_type::clock::now() - last;
                const auto hours = std::chrono::duration_cast<std::chrono::hours>(age).count();
                if (hours >= (long long)keep_days * 24) {
                    std::error_code del_ec;
                    fs::remove(it->path(), del_ec);
                    if (!del_ec) ++removed;
                }
            }
            continue;
        }
        // 只认 keeper-YYYY-MM-DD.log[.N]；解析不出日期的其它文件一律不动
        if (name.size() < prefix.size() + 10 + 4 ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(prefix.size() + 10, 4, ".log") != 0)
            continue;
        int fy = 0;
        unsigned fm = 0, fd = 0;
        if (!ParseYmd(name.substr(prefix.size(), 10), fy, fm, fd)) continue;
        if (today - DaysFromCivil(fy, fm, fd) < keep_days) continue;
        std::error_code del_ec;
        fs::remove(it->path(), del_ec);
        if (!del_ec) ++removed;
    }
    return removed;
}

bool HasAnyLogFile(const std::string& log_dir) {
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(log_dir), ec), end;
    if (ec) return false;   // 目录不存在 = 首次安装
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (name.rfind("keeper", 0) == 0 && name.find(".log") != std::string::npos)
            return true;
    }
    return false;
}

void LogInit(const std::string& log_dir, LogLevel level,
             size_t max_bytes, int keep_days) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_log_dir = log_dir;
    g_min_level = level;
    g_max_bytes = max_bytes > 4096 ? max_bytes : 4096;
    g_keep_days = keep_days > 0 ? keep_days : 3;
    OpenForToday();
}

void LogShutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_stream.is_open()) g_stream.flush();
    g_stream.close();
}

void LogWrite(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (level < g_min_level) return;

    const std::string line = Timestamp() + " [" + LevelName(level) + "] " + msg;

    // 跨天（或上次打开失败）时换文件；换文件同时执行 3 天清理
    if (!g_log_dir.empty() && (!g_stream.is_open() || g_current_day != TodayYmd()))
        OpenForToday();

    if (g_stream.is_open()) {
        RotateIfNeeded();
        g_stream << line << "\n";
        if (g_rotate_failed && !g_rotate_reported) {
            g_rotate_reported = true;
            g_stream << Timestamp() << " [" << LevelName(LogLevel::Warn) << "] "
                     << "日志轮转失败（文件可能被其它进程占用；每 64 行重试一次，"
                        "日志会继续增长）\n";
        }
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
    const std::string src = RedactJsonPairs(s);
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        size_t eq = src.find('=', i);
        if (eq == std::string::npos) { out.append(src, i, std::string::npos); break; }

        size_t key_start = src.find_last_of("?&", eq);
        key_start = (key_start == std::string::npos) ? i : key_start + 1;
        std::string key = src.substr(key_start, eq - key_start);

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
