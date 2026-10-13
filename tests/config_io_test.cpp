#include "doctest.h"
#include "store/Config.h"
#include <filesystem>
#include <fstream>
#include <windows.h>

namespace fs = std::filesystem;

namespace {
fs::path FreshDir(const char* name) {
    auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
}

TEST_CASE("保存后能原样读回，且密码可用") {
    auto dir = FreshDir("cnk_io_1");
    auto path = (dir / "config.json").string();

    cnk::AppConfig cfg;
    cfg.active_profile = "A";
    cnk::Profile p;
    p.name = "A";
    p.username = "user1";
    p.password = "pw1";
    p.has_password = true;
    cfg.profiles.push_back(p);

    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));

    cnk::AppConfig back;
    REQUIRE(cnk::ConfigLoadFrom(path, back, err));
    REQUIRE(back.profiles.size() == 1);
    CHECK(back.profiles[0].password == "pw1");
    CHECK(back.profiles[0].has_password);
}

TEST_CASE("磁盘上的密码不是明文") {
    auto dir = FreshDir("cnk_io_2");
    auto path = (dir / "config.json").string();
    cnk::AppConfig cfg;
    cnk::Profile p;
    p.name = "A";
    p.username = "user1";
    p.password = "SUPER_SECRET_VALUE";
    cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));

    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content.find("SUPER_SECRET_VALUE") == std::string::npos);
    CHECK(content.find("user1") != std::string::npos);  // 用户名不加密
}

TEST_CASE("损坏的配置文件被备份且返回失败，不崩溃") {
    auto dir = FreshDir("cnk_io_3");
    auto path = (dir / "config.json").string();
    {
        std::ofstream ofs(path);
        ofs << "{ broken json !!!";
    }
    cnk::AppConfig cfg;
    std::string err;
    CHECK_FALSE(cnk::ConfigLoadFrom(path, cfg, err));
    CHECK(err.size() > 0);
    CHECK(fs::exists(path + ".bak"));       // 已备份
    CHECK(cfg.profiles.empty());            // 回落到干净默认值
    CHECK(cfg.settings.probe_interval_seconds == 5);
}

TEST_CASE("文件不存在时返回失败并给出干净默认值") {
    auto dir = FreshDir("cnk_io_4");
    cnk::AppConfig cfg;
    std::string err;
    CHECK_FALSE(cnk::ConfigLoadFrom((dir / "nope.json").string(), cfg, err));
    CHECK(cfg.profiles.empty());
}

TEST_CASE("保存不留下临时文件") {
    auto dir = FreshDir("cnk_io_5");
    auto path = (dir / "config.json").string();
    cnk::AppConfig cfg;
    cnk::Profile p; p.name = "A"; cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));
    CHECK_FALSE(fs::exists(path + ".tmp"));
}

TEST_CASE("保存到不可写路径返回失败而不崩溃") {
    cnk::AppConfig cfg;
    std::string err;
    // Windows 下用非法路径字符触发失败
    CHECK_FALSE(cnk::ConfigSaveTo("Z:\\nonexistent\\dir\\config.json", cfg, err));
    CHECK(err.size() > 0);
}

TEST_CASE("替换失败时保留旧配置且不残留临时文件") {
    // 任务 6 审阅 P1 的回归测试：rename 失败绝不能删旧配置，且必须清理 .tmp
    auto dir = FreshDir("cnk_io_6");
    auto path = (dir / "config.json").string();

    cnk::AppConfig cfg;
    cnk::Profile p;
    p.name = "A";
    p.username = "keepme";
    cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));

    // 独占打开（不共享删除），使后续对目标的替换与删除必然失败
    HANDLE h = CreateFileW(fs::u8path(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);

    cfg.profiles[0].username = "newval";
    const bool ok = cnk::ConfigSaveTo(path, cfg, err);
    CloseHandle(h);

    CHECK_FALSE(ok);                          // 替换失败，明确报错
    CHECK(err.size() > 0);
    CHECK_FALSE(fs::exists(path + ".tmp"));   // 不残留临时文件
    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content.find("keepme") != std::string::npos);   // 旧配置原样保留
    CHECK(content.find("newval") == std::string::npos);
}
