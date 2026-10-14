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

// 收尾清理：best-effort，不因残留文件让用例失败（下次 FreshDir 兜底）
void CleanDir(const fs::path& d) {
    std::error_code ec;
    fs::remove_all(d, ec);
}

std::string ReadFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
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

    CleanDir(dir);
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

    const std::string content = ReadFile(path);
    CHECK(content.find("SUPER_SECRET_VALUE") == std::string::npos);
    CHECK(content.find("user1") != std::string::npos);  // 用户名不加密

    CleanDir(dir);
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

    CleanDir(dir);
}

TEST_CASE("文件不存在时返回失败并给出干净默认值") {
    auto dir = FreshDir("cnk_io_4");
    cnk::AppConfig cfg;
    std::string err;
    CHECK_FALSE(cnk::ConfigLoadFrom((dir / "nope.json").string(), cfg, err));
    CHECK(cfg.profiles.empty());

    CleanDir(dir);
}

TEST_CASE("保存不留下临时文件") {
    auto dir = FreshDir("cnk_io_5");
    auto path = (dir / "config.json").string();
    cnk::AppConfig cfg;
    cnk::Profile p; p.name = "A"; cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));
    CHECK_FALSE(fs::exists(path + ".tmp"));

    CleanDir(dir);
}

TEST_CASE("保存到不可写路径返回失败而不崩溃") {
    cnk::AppConfig cfg;
    std::string err;
    // 盘符不存在的路径。若本机恰有 Z: 盘则跳过本段（避免环境相关的假失败）。
    if (!fs::exists("Z:\\")) {
        CHECK_FALSE(cnk::ConfigSaveTo("Z:\\nonexistent\\dir\\config.json", cfg, err));
        CHECK(err.size() > 0);
    }
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

    // 独占打开目标（不共享删除），使后续对目标的替换与删除必然失败
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
    const std::string content = ReadFile(path);
    CHECK(content.find("keepme") != std::string::npos);   // 旧配置原样保留
    CHECK(content.find("newval") == std::string::npos);

    CleanDir(dir);
}

TEST_CASE("临时文件被锁时替换失败且旧配置保留") {
    // 任务 6 审阅 P1 的最直接复现：.tmp 被外部进程锁住（共享读写、不共享删除）。
    // 修复前的「删旧配置再重试」逻辑在此场景会真正删除 config.json（数据丢失）；
    // 修复后必须原样保留旧配置，仅报错返回。
    auto dir = FreshDir("cnk_io_7");
    auto path = (dir / "config.json").string();

    cnk::AppConfig cfg;
    cnk::Profile p;
    p.name = "A";
    p.username = "keepme";
    cfg.profiles.push_back(p);
    std::string err;
    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));

    // 锁住 .tmp：允许读写（ofstream 仍可打开），但不共享删除（rename 源必失败）
    HANDLE h = CreateFileW(fs::u8path(path + ".tmp").c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);

    cfg.profiles[0].username = "newval";
    const bool ok = cnk::ConfigSaveTo(path, cfg, err);
    CloseHandle(h);

    CHECK_FALSE(ok);
    CHECK(err.size() > 0);
    CHECK(fs::exists(path));  // 关键断言：旧配置未被删除（修复前此处会丢失）
    const std::string content = ReadFile(path);
    CHECK(content.find("keepme") != std::string::npos);
    CHECK(content.find("newval") == std::string::npos);

    CleanDir(dir);
}

TEST_CASE("SetProfilePassword 创建档案并加密落盘，回读可解密") {
    auto dir = FreshDir("cnk_io_setpw");
    auto path = (dir / "config.json").string();

    cnk::AppConfig cfg;
    std::string err;
    REQUIRE(cnk::SetProfilePassword(cfg, "stu001", "p@ss!", err));
    REQUIRE(cfg.profiles.size() == 1);
    CHECK(cfg.profiles[0].username == "stu001");
    CHECK(cfg.profiles[0].has_password);
    CHECK(cfg.active_profile == "stu001");   // 首份档案自动激活

    REQUIRE(cnk::ConfigSaveTo(path, cfg, err));
    CHECK(ReadFile(path).find("p@ss!") == std::string::npos);  // 磁盘无明文

    cnk::AppConfig back;
    REQUIRE(cnk::ConfigLoadFrom(path, back, err));
    REQUIRE(back.profiles.size() == 1);
    CHECK(back.profiles[0].password == "p@ss!");
    CHECK(back.profiles[0].has_password);

    CleanDir(dir);
}

TEST_CASE("SetProfilePassword 更新匹配账号/未指定时更新激活档案/未知账号新建") {
    cnk::AppConfig cfg;
    cnk::Profile a;
    a.name = "甲";
    a.username = "stu001";
    cfg.profiles.push_back(a);
    cnk::Profile b;
    b.name = "乙";
    b.username = "stu002";
    cfg.profiles.push_back(b);
    cfg.active_profile = "甲";

    std::string err;
    // 匹配已有账号：更新而非新建
    REQUIRE(cnk::SetProfilePassword(cfg, "stu002", "pw2", err));
    CHECK(cfg.profiles[1].password == "pw2");
    CHECK(cfg.profiles.size() == 2);

    // 未指定账号：更新激活档案
    REQUIRE(cnk::SetProfilePassword(cfg, "", "pw1", err));
    CHECK(cfg.profiles[0].password == "pw1");

    // 未知账号：新建且不改变激活档案
    REQUIRE(cnk::SetProfilePassword(cfg, "stu003", "pw3", err));
    CHECK(cfg.profiles.size() == 3);
    CHECK(cfg.profiles[2].username == "stu003");
    CHECK(cfg.active_profile == "甲");

    // 空密码拒绝
    CHECK_FALSE(cnk::SetProfilePassword(cfg, "stu001", "", err));
    CHECK(err.size() > 0);

    // 无档案且未指定账号 → 明确报错
    cnk::AppConfig empty_cfg;
    CHECK_FALSE(cnk::SetProfilePassword(empty_cfg, "", "x", err));
    CHECK(err.size() > 0);
}
