#include "core/DiagExport.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace cnk {

namespace {

constexpr int kAroundSeconds = 120;          // 「按故障时间」= 前后各 2 分钟
constexpr int kMaxLines = 5000;              // 输出上限（行）
constexpr size_t kMaxBytes = 2 * 1024 * 1024; // 输出上限（字节）
constexpr int kMaxDaysSpan = 3;              // 窗口最多跨几天，防呆

bool ParseTs(const std::string& s, std::time_t& out) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return false;
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31) return false;
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    out = std::mktime(&tm);
    return out != (std::time_t)-1;
}

std::string FormatTs(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string Ymd(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string Compact(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[24];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// 窗口覆盖的日期（含首尾）。跨午夜会返回两天。
std::vector<std::string> DatesInRange(std::time_t start, std::time_t end) {
    std::vector<std::string> out;
    std::time_t cur = start;
    for (int i = 0; i < kMaxDaysSpan && cur <= end; ++i) {
        const std::string ymd = Ymd(cur);
        if (out.empty() || out.back() != ymd) out.push_back(ymd);
        cur += 12 * 3600;   // 半天一步，足够跨过午夜且不会漏日
    }
    const std::string tail = Ymd(end);
    if (out.empty() || out.back() != tail) out.push_back(tail);
    return out;
}

// keeper-<ymd>.log 及其 .1/.2… 轮转；返回按「旧 → 新」排列（.N 最旧）。
std::vector<std::string> CollectDayFiles(const std::string& log_dir, const std::string& ymd) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = fs::u8path(log_dir);
    for (int i = 6; i >= 0; --i) {
        fs::path p = dir / fs::u8path("keeper-" + ymd + ".log" +
                                      (i == 0 ? std::string() : "." + std::to_string(i)));
        if (fs::is_regular_file(p, ec)) out.push_back(p.u8string());
    }
    return out;
}

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

std::string RedactForExport(const std::string& line) {
    // 顺序有讲究：先按键值对吃凭据，再整体吃掉 query，避免 query 里的
    // password=... 先被 IP/数字规则打散成看得见的碎片。
    static const std::regex kPair(
        R"((password|passwd|pwd|token|secret|userid|username|user|account)=([^\s&,;]+))",
        std::regex::icase);
    static const std::regex kQuery(R"(\?[^\s"')]+)");
    static const std::regex kQuad(R"((\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}))");
    static const std::regex kMac(R"(([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})", std::regex::icase);
    static const std::regex kNum12(R"((^|[^0-9A-Za-z])([0-9]{12})([^0-9A-Za-z]|$))");

    std::string s = std::regex_replace(line, kPair, "$1=***");
    s = std::regex_replace(s, kQuery, "?<已脱敏>");
    // 保留前两段：既去掉真实地址，又还能看出"是不是同一网段"
    s = std::regex_replace(s, kQuad, "$1.$2.x.x");
    s = std::regex_replace(s, kMac, "xx-xx-xx-xx-xx-xx");
    s = std::regex_replace(s, kNum12, "$1***$3");
    return s;
}

DiagResult ExportDiagnostics(const std::string& log_dir, const std::string& out_dir,
                             const std::string& build_info, const DiagRange& range) {
    DiagResult r;
    if (log_dir.empty()) {
        r.error = "日志目录不可用";
        return r;
    }
    if (out_dir.empty()) {
        r.error = "导出目录不可用";
        return r;
    }

    const std::time_t now = std::time(nullptr);
    std::time_t start = 0;
    std::time_t end = 0;
    int minutes = range.minutes;
    if (range.around) {
        if (!ParseTs(range.around_ts, start)) {
            r.error = "时间格式应为 YYYY-MM-DD HH:MM:SS";
            return r;
        }
        start -= kAroundSeconds;
        end = start + 2 * kAroundSeconds;
    } else {
        if (minutes <= 0) minutes = 15;
        if (minutes > 24 * 60) minutes = 24 * 60;
        end = now;
        start = end - (std::time_t)minutes * 60;
    }

    std::error_code ec;
    fs::create_directories(fs::u8path(out_dir), ec);
    const fs::path out_path =
        fs::u8path(out_dir) / fs::u8path("diag-" + Compact(now) + ".log");

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        r.error = "无法写入导出文件";
        return r;
    }

    // 收集（先脱敏，后面只往文件里倒）
    std::vector<std::string> lines;
    for (const auto& ymd : DatesInRange(start, end)) {
        const auto files = CollectDayFiles(log_dir, ymd);
        if (files.empty()) {
            r.missing.push_back(ymd);
            continue;
        }
        for (const auto& f : files) {
            r.sources.push_back(fs::path(fs::u8path(f)).filename().u8string());
            std::ifstream in(fs::u8path(f), std::ios::binary);
            std::string line;
            while (!r.truncated && std::getline(in, line)) {
                std::time_t t = 0;
                if (line.size() < 19 || !ParseTs(line.substr(0, 19), t)) continue;
                if (t < start || t > end) continue;
                if ((int)lines.size() >= kMaxLines) {
                    r.truncated = true;
                    break;
                }
                lines.push_back(RedactForExport(line));
            }
        }
    }

    // 多文件时按时间戳稳定排序（轮转文件之间的先后不可靠）
    std::stable_sort(lines.begin(), lines.end(), [](const std::string& a, const std::string& b) {
        return a.substr(0, std::min<size_t>(19, a.size())) <
               b.substr(0, std::min<size_t>(19, b.size()));
    });

    out << "# CampusNet-Keeper 诊断日志\n";
    out << "# 生成时间: " << FormatTs(now) << "\n";
    if (!build_info.empty()) out << "# 版本: " << build_info << "\n";
    out << "# 时间窗: " << FormatTs(start) << " ~ " << FormatTs(end) << "（"
        << (range.around ? "故障时刻前后各 2 分钟"
                         : "最近 " + std::to_string(minutes) + " 分钟")
        << "）\n";
    out << "# 来源: " << (r.sources.empty() ? "(无)" : Join(r.sources, ", ")) << "\n";
    if (!r.missing.empty())
        out << "# 缺失: " << Join(r.missing, ", ") << "（该日没有日志文件）\n";
    if (r.truncated)
        out << "# 截断: 是（已达 " << kMaxLines << " 行 / " << kMaxBytes << " 字节上限）\n";
    out << "# 脱敏: 账号与凭据 → ***；门户查询参数 → ?<已脱敏>；"
           "IP → a.b.x.x；MAC 与 12 位数字 → ***\n";
    out << "# " << std::string(56, '-') << "\n";

    size_t bytes = 0;
    for (const auto& line : lines) {
        if (bytes + line.size() > kMaxBytes) {
            r.truncated = true;
            break;
        }
        out << line << "\n";
        bytes += line.size() + 1;
        ++r.lines;
    }

    if (r.truncated) {
        // 头部已经写了"截断"，这里再补一行尾注：读的人是先看到头部还是先看到结尾不确定
        out << "# （输出已截断，仅保留前 " << r.lines << " 行）\n";
    }

    if (!out) {
        r.error = "写入导出文件失败";
        return r;
    }

    r.ok = true;
    r.path = out_path.u8string();
    return r;
}

}  // namespace cnk
