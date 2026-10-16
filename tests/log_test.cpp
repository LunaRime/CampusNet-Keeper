#include "doctest.h"
#include "core/Log.h"
#include <chrono>
#include <filesystem>
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
    // 2026-10-16 审阅 P1：新式 API 的展示用请求体是 JSON，而 RedactSecrets 只认
    // key=value，导致学号与 rcn 被原样打印（老式通道的 DDDDD 反而是打码的 = 回归）。
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
    // 通用表单的账号字段（任务 21 会用它提交学号）
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
