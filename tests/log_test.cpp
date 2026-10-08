#include "doctest.h"
#include "core/Log.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("RedactSecrets 屏蔽密码类参数") {
    CHECK(cnk::RedactSecrets("upass=secret123&DDDDD=alice")
          == "upass=***&DDDDD=alice");
    CHECK(cnk::RedactSecrets("password=abc&x=1") == "password=***&x=1");
    CHECK(cnk::RedactSecrets("pwd=p&user_password=q")
          == "pwd=***&user_password=***");
}

TEST_CASE("RedactSecrets 对无凭据字符串原样返回") {
    CHECK(cnk::RedactSecrets("http://a/b?c=1") == "http://a/b?c=1");
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
