#include "doctest.h"
#include "core/DiagExport.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path FreshDir(const char* name) {
    static int counter = 0;
    const std::string uniq = std::string(name) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
        std::to_string(++counter);
    const fs::path d = fs::temp_directory_path() / uniq;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string Fmt(std::time_t t) {
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

// 今天 00:00:00（本地时区）
std::time_t TodayMidnight() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&tm, &now);
#endif
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

void WriteFile(const fs::path& p, const std::vector<std::string>& lines) {
    std::ofstream out(p, std::ios::binary);
    for (const auto& l : lines) out << l << "\n";
}

}  // namespace

TEST_CASE("导出行脱敏：账号/凭据/门户参数/地址/MAC/12 位数字") {
    using cnk::RedactForExport;

    CHECK(RedactForExport("2026-11-25 13:00:00 [INFO] 登录失败 password=Secret123")
          == "2026-11-25 13:00:00 [INFO] 登录失败 password=***");
    CHECK(RedactForExport("userid=20230000000&rcn=abc")
              .find("20230000000") == std::string::npos);
    CHECK(RedactForExport("userid=20230000000&rcn=abc").find("userid=***") != std::string::npos);

    // 门户查询参数整体吃掉（params 里就是加密过的凭据）
    const std::string q = RedactForExport(
        "GET /eportal/portal/login?params=AbC123%3D%3D&rcn=1 HTTP/1.1");
    CHECK(q.find("AbC123") == std::string::npos);
    CHECK(q.find("?<已脱敏>") != std::string::npos);

    // 地址归一化：保留前两段（还能看出是不是同一网段），去掉真实主机
    CHECK(RedactForExport("本机地址=192.0.2.10，网关=192.0.2.1")
          == "本机地址=192.0.x.x，网关=192.0.x.x");

    CHECK(RedactForExport("MAC 00:00:00:00:00:00")
          == "MAC xx-xx-xx-xx-xx-xx");

    // 12 位账号打码（前后非字母数字）
    const std::string acc = RedactForExport("账号 202400000000 已停止重试");
    CHECK(acc.find("202400000000") == std::string::npos);
    CHECK(acc.find("***") != std::string::npos);

    // 时间戳与级别原样保留：诊断要能按时间读
    CHECK(RedactForExport("2026-11-25 13:00:00 [WARN] x")
              .rfind("2026-11-25 13:00:00 [WARN]", 0) == 0);
}

TEST_CASE("默认最近 15 分钟：只取窗口内、且只采 CNK 自己的文件") {
    const fs::path dir = FreshDir("cnk_diag_win");
    const fs::path out = FreshDir("cnk_diag_out");
    const std::time_t now = std::time(nullptr);

    WriteFile(dir / fs::u8path("keeper-" + Ymd(now) + ".log"), {
        Fmt(now - 20 * 60) + " [INFO] 20 分钟前：不该出现",
        Fmt(now - 60) + " [INFO] 一分钟前：该出现",
        Fmt(now - 5) + " [WARN] 五秒前 password=xyz：该出现且脱敏",
    });
    // 别的程序的文件：不许采
    WriteFile(dir / "other.log", {Fmt(now - 10) + " [INFO] 别人家的日志"});

    cnk::DiagRange range;   // 默认 15 分钟
    const auto r = cnk::ExportDiagnostics(dir.u8string(), out.u8string(), "test-build", range);

    REQUIRE(r.ok);
    CHECK(r.lines == 2);
    CHECK(r.sources.size() == 1);
    CHECK(r.sources[0].find("keeper-") == 0);

    std::ifstream in(fs::u8path(r.path), std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("20 分钟前") == std::string::npos);
    CHECK(body.find("一分钟前") != std::string::npos);
    CHECK(body.find("password=xyz") == std::string::npos);
    CHECK(body.find("password=***") != std::string::npos);
    CHECK(body.find("别人家的日志") == std::string::npos);
    CHECK(body.find("# 版本: test-build") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(out, ec);
}

TEST_CASE("按故障时刻取前后各 2 分钟，且能跨午夜") {
    const fs::path dir = FreshDir("cnk_diag_mid");
    const fs::path out = FreshDir("cnk_diag_mid_out");
    const std::time_t mid = TodayMidnight();   // 今天 00:00:00

    // 昨天 23:59:00 与今天 00:01:00 各一行；窗口 ±2 分钟把它们都覆盖
    WriteFile(dir / fs::u8path("keeper-" + Ymd(mid - 3600) + ".log"),
              {Fmt(mid - 60) + " [INFO] 跨日 前一夜"});
    WriteFile(dir / fs::u8path("keeper-" + Ymd(mid) + ".log"),
              {Fmt(mid + 60) + " [INFO] 跨日 当天"});

    cnk::DiagRange range;
    range.around = true;
    range.around_ts = Fmt(mid);
    const auto r = cnk::ExportDiagnostics(dir.u8string(), out.u8string(), "", range);

    REQUIRE(r.ok);
    CHECK(r.lines == 2);
    CHECK(r.sources.size() == 2);
    CHECK(r.missing.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(out, ec);
}

TEST_CASE("窗口涉及的文件缺失时，头部明确标出来") {
    const fs::path dir = FreshDir("cnk_diag_miss");
    const fs::path out = FreshDir("cnk_diag_miss_out");
    const std::time_t mid = TodayMidnight();

    // 只造今天的文件，不造昨天的 → 窗口跨日，昨天算缺失
    WriteFile(dir / fs::u8path("keeper-" + Ymd(mid) + ".log"),
              {Fmt(mid + 60) + " [INFO] 只有当天"});

    cnk::DiagRange range;
    range.around = true;
    range.around_ts = Fmt(mid);
    const auto r = cnk::ExportDiagnostics(dir.u8string(), out.u8string(), "", range);

    REQUIRE(r.ok);
    REQUIRE(r.missing.size() == 1);
    CHECK(r.missing[0] == Ymd(mid - 3600));

    std::ifstream in(fs::u8path(r.path), std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("# 缺失: ") != std::string::npos);
    CHECK(body.find(Ymd(mid - 3600)) != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(out, ec);
}

TEST_CASE("输出超限时截断并标注，不假装完整") {
    const fs::path dir = FreshDir("cnk_diag_cut");
    const fs::path out = FreshDir("cnk_diag_cut_out");
    const std::time_t now = std::time(nullptr);

    std::vector<std::string> many;
    many.reserve(6000);
    for (int i = 0; i < 6000; ++i) many.push_back(Fmt(now - 60) + " [INFO] 第 " + std::to_string(i) + " 行");
    WriteFile(dir / fs::u8path("keeper-" + Ymd(now) + ".log"), many);

    cnk::DiagRange range;
    const auto r = cnk::ExportDiagnostics(dir.u8string(), out.u8string(), "", range);

    REQUIRE(r.ok);
    CHECK(r.truncated);
    CHECK(r.lines == 5000);

    std::ifstream in(fs::u8path(r.path), std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("# 截断: 是") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(out, ec);
}

TEST_CASE("轮转文件一并采集，并按时间排序") {
    const fs::path dir = FreshDir("cnk_diag_rot");
    const fs::path out = FreshDir("cnk_diag_rot_out");
    const std::time_t now = std::time(nullptr);

    // .1 是较旧的一份（轮转把 .log 改名成 .1）
    WriteFile(dir / fs::u8path("keeper-" + Ymd(now) + ".log.1"),
              {Fmt(now - 120) + " [INFO] 轮转里的旧行"});
    WriteFile(dir / fs::u8path("keeper-" + Ymd(now) + ".log"),
              {Fmt(now - 30) + " [INFO] 当前文件的新行"});

    cnk::DiagRange range;
    const auto r = cnk::ExportDiagnostics(dir.u8string(), out.u8string(), "", range);

    REQUIRE(r.ok);
    CHECK(r.lines == 2);
    CHECK(r.sources.size() == 2);

    std::ifstream in(fs::u8path(r.path), std::ios::binary);
    std::string line;
    std::vector<std::string> got;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') got.push_back(line);
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0].find("旧行") != std::string::npos);   // 时间早的在前
    CHECK(got[1].find("新行") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(out, ec);
}
