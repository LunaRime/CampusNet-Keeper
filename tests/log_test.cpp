#include "doctest.h"
#include "core/Log.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("RedactSecrets 屏蔽密码类参数") {
    // DDDDD（Dr.COM 账号字段，内含学号）自任务 15 起与密码同等脱敏
    CHECK(cnk::RedactSecrets("upass=secret123&DDDDD=alice")
          == "upass=***&DDDDD=***");
    CHECK(cnk::RedactSecrets("password=abc&x=1") == "password=***&x=1");
    CHECK(cnk::RedactSecrets("pwd=p&user_password=q")
          == "pwd=***&user_password=***");
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
    auto dir = fs::temp_directory_path() / "cnk_log_test";
    fs::remove_all(dir);
    cnk::LogInit(dir.string(), cnk::LogLevel::Debug, 1024 * 1024, 3);
    cnk::LogWrite(cnk::LogLevel::Info, "hello-marker");
    auto lines = cnk::LogTail(50);
    cnk::LogShutdown();
    bool found = false;
    for (auto& l : lines) if (l.find("hello-marker") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("低于阈值的日志不写入") {
    auto dir = fs::temp_directory_path() / "cnk_log_test2";
    fs::remove_all(dir);
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
