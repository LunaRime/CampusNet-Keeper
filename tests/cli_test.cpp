#include "doctest.h"
#include "core/Cli.h"

TEST_CASE("解析 --probe-now") {
    auto opts = cnk::ParseCli({"--probe-now"});
    CHECK(opts.mode == cnk::CliMode::ProbeNow);
}

TEST_CASE("解析 --test-login") {
    auto opts = cnk::ParseCli({"--test-login"});
    CHECK(opts.mode == cnk::CliMode::TestLogin);
}

TEST_CASE("解析 --detect") {
    auto opts = cnk::ParseCli({"--detect"});
    CHECK(opts.mode == cnk::CliMode::Detect);
}

TEST_CASE("解析 --version 与 --help") {
    CHECK(cnk::ParseCli({"--version"}).mode == cnk::CliMode::Version);
    CHECK(cnk::ParseCli({"--help"}).mode == cnk::CliMode::Help);
    CHECK(cnk::ParseCli({"-h"}).mode == cnk::CliMode::Help);
}

TEST_CASE("无参数时为守护模式") {
    CHECK(cnk::ParseCli({}).mode == cnk::CliMode::Daemon);
}

TEST_CASE("解析 key=value 形式的参数") {
    auto opts = cnk::ParseCli({"--config=C:\\tmp\\a.json",
                               "--simulate=drcom"});
    CHECK(opts.config_path == "C:\\tmp\\a.json");
    CHECK(opts.simulate_vendor == "drcom");
}

TEST_CASE("解析 --config 空格分隔形式") {
    auto opts = cnk::ParseCli({"--config", "C:\\tmp\\b.json"});
    CHECK(opts.config_path == "C:\\tmp\\b.json");
}

TEST_CASE("未知参数被收集但不影响模式判定") {
    auto opts = cnk::ParseCli({"--probe-now", "--unknown-thing"});
    CHECK(opts.mode == cnk::CliMode::ProbeNow);
    CHECK(opts.unknown.size() == 1);
}

TEST_CASE("帮助文本非空且包含关键开关") {
    std::string h = cnk::CliHelpText();
    CHECK(h.find("--probe-now") != std::string::npos);
    CHECK(h.find("--test-login") != std::string::npos);
    CHECK(h.find("--config") != std::string::npos);
}

TEST_CASE("解析模拟模式的三个子参数") {
    auto opts = cnk::ParseCli({"--simulate=drcom",
                               "--simulate-user=alice",
                               "--simulate-pass=pw",
                               "--simulate-fail=badcred"});
    CHECK(opts.simulate_vendor == "drcom");
    CHECK(opts.simulate_user == "alice");
    CHECK(opts.simulate_pass == "pw");
    CHECK(opts.simulate_fail == "badcred");
}

TEST_CASE("--simulate-user 不被 --simulate 抢先匹配") {
    auto opts = cnk::ParseCli({"--simulate-user=bob"});
    CHECK(opts.simulate_user == "bob");
    CHECK(opts.simulate_vendor.empty());
}

TEST_CASE("末尾的 --config 无值时安全忽略而不崩溃") {
    auto opts = cnk::ParseCli({"--probe-now", "--config"});
    CHECK(opts.mode == cnk::CliMode::ProbeNow);
    CHECK(opts.config_path.empty());
}

TEST_CASE("带值参数的空格分隔、无值安全与前缀吞噬回归") {
    // 空格分隔形式
    auto o1 = cnk::ParseCli({"--simulate-user", "bob", "--config", "a.json"});
    CHECK(o1.simulate_user == "bob");
    CHECK(o1.config_path == "a.json");

    // 末尾无值：安全忽略
    auto o2 = cnk::ParseCli({"--probe-now", "--simulate-user"});
    CHECK(o2.mode == cnk::CliMode::ProbeNow);
    CHECK(o2.simulate_user.empty());

    // 前缀吞噬回归：--configuration 不得被当作 --config
    auto o3 = cnk::ParseCli({"--version", "--configuration=x"});
    CHECK(o3.config_path.empty());
    CHECK(o3.unknown.size() == 1);
}

TEST_CASE("解析 --set-password 与 --user") {
    auto o1 = cnk::ParseCli({"--set-password"});
    CHECK(o1.mode == cnk::CliMode::SetPassword);

    auto o2 = cnk::ParseCli({"--set-password", "--user=2024147xxxx"});
    CHECK(o2.mode == cnk::CliMode::SetPassword);
    CHECK(o2.user == "2024147xxxx");

    auto o3 = cnk::ParseCli({"--set-password", "--user", "stu001"});
    CHECK(o3.user == "stu001");
}
