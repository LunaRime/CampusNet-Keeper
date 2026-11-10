#include "doctest.h"
#include "core/Log.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <ctime>
#include <string>

namespace fs = std::filesystem;

namespace {

// 唯一目录名 + 非抛出调用：机器上可能残留同名目录（含被沙箱加固 ACL、当前令牌删不掉
// 的），固定名字会让 remove_all 抛异常、整例假失败。
fs::path FreshLogDir(const char* name) {
    static int counter = 0;
    const std::string uniq =
        std::string(name) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "_" + std::to_string(++counter);
    auto dir = fs::temp_directory_path() / uniq;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

void Touch(const fs::path& dir, const char* name) {
    std::ofstream out(dir / name);
    out << "x";
}

}  // namespace

TEST_CASE("RedactSecrets 屏蔽密码类参数") {
    // DDDDD（Dr.COM 账号字段，内含学号）自任务 15 起与密码同等脱敏
    CHECK(cnk::RedactSecrets("upass=secret123&DDDDD=alice")
          == "upass=***&DDDDD=***");
    CHECK(cnk::RedactSecrets("password=abc&x=1") == "password=***&x=1");
    CHECK(cnk::RedactSecrets("pwd=p&user_password=q")
          == "pwd=***&user_password=***");
}

TEST_CASE("RedactSecrets 必须能脱敏 JSON 载荷（新式 API 的请求体是 JSON）") {
    const std::string body =
        "{\"login_method\":1,\"user_account\":\",0,202400000000@cmcc\","
        "\"user_password\":\"pw-secret\",\"wlan_user_mac\":\"000000000000\","
        "\"rcn\":\"Zx9Qw2Er\"}";
    const std::string out = cnk::RedactSecrets(body);
    CHECK(out.find("pw-secret") == std::string::npos);
    CHECK(out.find("202400000000") == std::string::npos);
    CHECK(out.find("Zx9Qw2Er") == std::string::npos);
    CHECK(out.find("\"user_password\":\"***\"") != std::string::npos);
    CHECK(out.find("\"user_account\":\"***\"") != std::string::npos);
    CHECK(out.find("\"rcn\":\"***\"") != std::string::npos);
    // 非敏感字段保持原样，便于诊断
    CHECK(out.find("\"login_method\":1") != std::string::npos);
    CHECK(out.find("\"wlan_user_mac\":\"000000000000\"") != std::string::npos);
}

TEST_CASE("RedactSecrets 对无凭据字符串原样返回") {
    CHECK(cnk::RedactSecrets("http://a/b?c=1") == "http://a/b?c=1");
}

TEST_CASE("RedactSecrets 覆盖内嵌查询串") {
    CHECK(cnk::RedactSecrets("url=http://x?upass=y") == "url=http://x?upass=***");
    CHECK(cnk::RedactSecrets("url=http://x?a=1&b=2") == "url=http://x?a=1&b=2");
    CHECK(cnk::RedactSecrets("next=/p?pwd=1&k=2") == "next=/p?pwd=***&k=2");
}

TEST_CASE("RedactSecrets 键名大小写不敏感") {
    CHECK(cnk::RedactSecrets("UPASS=secret") == "UPASS=***");
    CHECK(cnk::RedactSecrets("PassWord=abc") == "PassWord=***");
    CHECK(cnk::RedactSecrets("User_Password=q") == "User_Password=***");
    CHECK(cnk::RedactSecrets("PASSWD=q") == "PASSWD=***");
    CHECK(cnk::RedactSecrets("ddddd=alice") == "ddddd=***");
    CHECK(cnk::RedactSecrets("username=stu001&domain=cmcc")
          == "username=***&domain=cmcc");
}

TEST_CASE("日志写入后可被 LogTail 读回") {
    auto dir = FreshLogDir("cnk_log_test");
    cnk::LogInit(dir.string(), cnk::LogLevel::Debug, 1024 * 1024, 3);
    cnk::LogWrite(cnk::LogLevel::Info, "hello-marker");
    auto lines = cnk::LogTail(50);
    cnk::LogShutdown();
    bool found = false;
    for (auto& l : lines) if (l.find("hello-marker") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("低于阈值的日志不写入") {
    auto dir = FreshLogDir("cnk_log_test2");
    cnk::LogInit(dir.string(), cnk::LogLevel::Warn, 1024 * 1024, 3);
    cnk::LogWrite(cnk::LogLevel::Info, "should-not-appear");
    cnk::LogWrite(cnk::LogLevel::Warn, "should-appear");
    auto lines = cnk::LogTail(50);
    cnk::LogShutdown();
    std::string joined;
    for (auto& l : lines) joined += l;
    CHECK(joined.find("should-not-appear") == std::string::npos);
    CHECK(joined.find("should-appear") != std::string::npos);
}

TEST_CASE("按天文件名格式固定") {
    CHECK(cnk::LogFileNameForDate("2026-11-10") == "keeper-2026-11-10.log");
    CHECK(cnk::LogFileNameForDate("2026-01-02") == "keeper-2026-01-02.log");
}

TEST_CASE("日志真的写进当天文件，不再有 keeper.log") {
    auto dir = FreshLogDir("cnk_log_daily");
    cnk::LogInit(dir.string(), cnk::LogLevel::Info, 1024 * 1024, 3);
    cnk::LogWrite(cnk::LogLevel::Info, "daily-marker-xyz");
    cnk::LogShutdown();

    // 期望文件名在测试里独立拼出来，不复用被测的 LogFileNameForDate
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    const std::string expected = std::string("keeper-") + buf + ".log";

    std::ifstream in(dir / expected);
    REQUIRE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(content.find("daily-marker-xyz") != std::string::npos);
    CHECK_FALSE(fs::exists(dir / "keeper.log"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("清理只保留最近 3 天，轮转备份同样按日期清，无关文件不动") {
    auto dir = FreshLogDir("cnk_log_cleanup");
    std::error_code ec;
    fs::create_directories(dir, ec);
    REQUIRE(!ec);
    for (int d = 1; d <= 10; ++d) {
        char name[64];
        std::snprintf(name, sizeof(name), "keeper-2026-11-%02d.log", d);
        Touch(dir, name);
    }
    Touch(dir, "keeper-2026-11-01.log.1");   // 日内轮转备份
    Touch(dir, "keeper.log");                // 旧格式：不动
    Touch(dir, "notes.txt");                 // 无关文件：不动
    Touch(dir, "keeper-not-a-date.log");     // 解析不出日期：不动

    const int removed = cnk::CleanupOldLogs(dir.string(), "2026-11-10", 3);
    // 保留 08/09/10；删 01..07 共 7 个 + 01.log.1 共 1 个
    CHECK(removed == 8);
    CHECK(fs::exists(dir / "keeper-2026-11-08.log"));
    CHECK(fs::exists(dir / "keeper-2026-11-09.log"));
    CHECK(fs::exists(dir / "keeper-2026-11-10.log"));
    CHECK_FALSE(fs::exists(dir / "keeper-2026-11-07.log"));
    CHECK_FALSE(fs::exists(dir / "keeper-2026-11-01.log.1"));
    CHECK(fs::exists(dir / "keeper.log"));
    CHECK(fs::exists(dir / "notes.txt"));
    CHECK(fs::exists(dir / "keeper-not-a-date.log"));
    fs::remove_all(dir, ec);
}

TEST_CASE("keep_days 非法值不删任何文件") {
    auto dir = FreshLogDir("cnk_log_badkeep");
    std::error_code ec;
    fs::create_directories(dir, ec);
    Touch(dir, "keeper-2020-01-01.log");
    CHECK(cnk::CleanupOldLogs(dir.string(), "2026-11-10", 0) == 0);
    CHECK(fs::exists(dir / "keeper-2020-01-01.log"));
    fs::remove_all(dir, ec);
}

TEST_CASE("HasAnyLogFile 认新旧两种日志，忽略无关文件") {
    auto dir = FreshLogDir("cnk_log_hasany");
    std::error_code ec;
    fs::create_directories(dir, ec);
    CHECK_FALSE(cnk::HasAnyLogFile(dir.string()));
    Touch(dir, "notes.txt");
    CHECK_FALSE(cnk::HasAnyLogFile(dir.string()));
    Touch(dir, "keeper.log");
    CHECK(cnk::HasAnyLogFile(dir.string()));
    fs::remove(dir / "keeper.log", ec);
    CHECK_FALSE(cnk::HasAnyLogFile(dir.string()));
    Touch(dir, "keeper-2026-11-10.log");
    CHECK(cnk::HasAnyLogFile(dir.string()));
    fs::remove_all(dir, ec);
}
