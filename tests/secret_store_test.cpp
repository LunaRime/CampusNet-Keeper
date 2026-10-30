// 密码存储与迁移（任务 27）的回归测试。
//
// 这里钉住的都是**会毁数据**的行为，不是"能跑通"：
//   1. 外来密文（换机/换账号）不得被任何一次保存抹掉；
//   2. 本机密文不得留在会漫游的 config.json 里；
//   3. 密码库里的坏条目不得被删除；
//   4. 导出文件必须口令错就解不开、被改过就解不开。
#include "doctest.h"

#include "core/Crypto.h"
#include "store/Config.h"
#include "store/PortableSecret.h"
#include "store/Secret.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

fs::path FreshDir(const char* name) {
    auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void CleanDir(const fs::path& d) {
    std::error_code ec;
    fs::remove_all(d, ec);
}

std::string ReadFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& path, const std::string& text) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << text;
}

// 一段能 base64 解码、但绝不可能被 DPAPI 解开的"外来密文"
std::string BogusCipherB64() {
    return cnk::Base64Encode(std::string(96, '\x7f'));
}

}  // namespace

TEST_CASE("外来密文不会被任何一次保存抹掉（本次事故的回归）") {
    const auto dir = FreshDir("cnk_secret_1");
    const auto cfgp = (dir / "config.json").string();
    const auto secp = (dir / "secret.json").string();
    const std::string foreign = BogusCipherB64();

    WriteFile(cfgp, std::string(R"({"version":1,"active_profile":"A","profiles":[{"name":"A",)"
                                R"("username":"u","password_enc":")") +
                    foreign + R"("}]})");

    cnk::AppConfig cfg;
    std::string err;
    REQUIRE(cnk::ConfigLoadSplitFrom(cfgp, secp, cfg, err));
    REQUIRE(cfg.profiles.size() == 1);
    CHECK_FALSE(cfg.profiles[0].has_password);
    CHECK(cfg.profiles[0].password_foreign);
    CHECK(cfg.profiles[0].password_enc_raw == foreign);

    // 用户只是在设置界面里点了一次「保存」（改个无关设置）
    cfg.settings.probe_interval_seconds = 9;
    REQUIRE(cnk::ConfigSaveSplitTo(cfgp, secp, cfg, err));

    const std::string after = ReadFile(cfgp);
    CHECK(after.find(foreign) != std::string::npos);   // 修好前这里会是空串
    CHECK(after.find("\"probe_interval_seconds\": 9") != std::string::npos);

    CleanDir(dir);
}

TEST_CASE("本机密文落 secret.json，config.json 不再携带密文") {
    const auto dir = FreshDir("cnk_secret_2");
    const auto cfgp = (dir / "config.json").string();
    const auto secp = (dir / "secret.json").string();

    cnk::AppConfig cfg;
    cfg.active_profile = "A";
    cnk::Profile p;
    p.name = "A";
    p.username = "u";
    p.password = "pw-secret-value";
    p.has_password = true;
    cfg.profiles.push_back(p);

    std::string err;
    REQUIRE(cnk::ConfigSaveSplitTo(cfgp, secp, cfg, err));

    const std::string config_text = ReadFile(cfgp);
    const std::string secret_text = ReadFile(secp);
    CHECK(config_text.find("pw-secret-value") == std::string::npos);
    CHECK(config_text.find("\"password_enc\": \"\"") != std::string::npos);
    CHECK(secret_text.find("\"A\"") != std::string::npos);

    cnk::AppConfig back;
    REQUIRE(cnk::ConfigLoadSplitFrom(cfgp, secp, back, err));
    REQUIRE(back.profiles.size() == 1);
    CHECK(back.profiles[0].password == "pw-secret-value");
    CHECK(back.profiles[0].has_password);
    CHECK_FALSE(back.profiles[0].password_foreign);

    CleanDir(dir);
}

TEST_CASE("旧格式（密文内联）自动迁移到密码库") {
    const auto dir = FreshDir("cnk_secret_3");
    const auto cfgp = (dir / "config.json").string();
    const auto secp = (dir / "secret.json").string();

    cnk::AppConfig cfg;
    cfg.active_profile = "A";
    cnk::Profile p;
    p.name = "A";
    p.username = "u";
    p.password = "old-inline-pw";
    p.has_password = true;
    cfg.profiles.push_back(p);

    std::string err;
    REQUIRE(cnk::ConfigSaveTo(cfgp, cfg, err));   // 自包含形态（v0.1.5 及更早的落盘方式）
    CHECK(ReadFile(cfgp).find("\"password_enc\": \"\"") == std::string::npos);

    cnk::AppConfig loaded;
    REQUIRE(cnk::ConfigLoadSplitFrom(cfgp, secp, loaded, err));
    REQUIRE(loaded.profiles.size() == 1);
    CHECK(loaded.profiles[0].password == "old-inline-pw");

    REQUIRE(cnk::ConfigSaveSplitTo(cfgp, secp, loaded, err));
    CHECK(ReadFile(cfgp).find("\"password_enc\": \"\"") != std::string::npos);
    CHECK(ReadFile(secp).find("\"A\"") != std::string::npos);

    CleanDir(dir);
}

TEST_CASE("密码库里的坏条目不会被删掉（别人的密码不能替我丢）") {
    const auto dir = FreshDir("cnk_secret_4");
    const auto cfgp = (dir / "config.json").string();
    const auto secp = (dir / "secret.json").string();
    const std::string bad = BogusCipherB64();

    WriteFile(cfgp, R"({"version":1,"active_profile":"A","profiles":[)"
                    R"({"name":"A","username":"u"},{"name":"B","username":"v"}]})");
    WriteFile(secp, std::string(R"({"version":1,"profiles":{"A":{"enc":")") + bad +
                    R"("}}})");

    cnk::AppConfig cfg;
    std::string err;
    REQUIRE(cnk::ConfigLoadSplitFrom(cfgp, secp, cfg, err));
    REQUIRE(cfg.profiles.size() == 2);
    CHECK(cfg.profiles[0].password_foreign);
    CHECK_FALSE(cfg.profiles[0].has_password);

    // 给 B 设密码（会让密码库整体重写）→ A 的坏条目必须还在
    REQUIRE(cnk::SetProfilePassword(cfg, "v", "b-new-pw", err));
    REQUIRE(cnk::ConfigSaveSplitTo(cfgp, secp, cfg, err));

    const std::string secret_text = ReadFile(secp);
    CHECK(secret_text.find(bad) != std::string::npos);
    CHECK(secret_text.find("\"B\"") != std::string::npos);

    CleanDir(dir);
}

TEST_CASE("密码库损坏时先备份再继续，不静默清空") {
    const auto dir = FreshDir("cnk_secret_5");
    const auto cfgp = (dir / "config.json").string();
    const auto secp = (dir / "secret.json").string();

    WriteFile(secp, "{ this is not json");
    WriteFile(cfgp, R"({"version":1,"active_profile":"A","profiles":[{"name":"A","username":"u"}]})");

    cnk::AppConfig cfg;
    std::string err;
    REQUIRE(cnk::ConfigLoadSplitFrom(cfgp, secp, cfg, err));  // 读不出来也要能用
    REQUIRE(cnk::SetProfilePassword(cfg, "u", "pw-after-corrupt", err));
    REQUIRE(cnk::ConfigSaveSplitTo(cfgp, secp, cfg, err));

    CHECK(fs::exists(fs::u8path(secp + ".bak")));
    CHECK(ReadFile(secp).find("\"A\"") != std::string::npos);

    CleanDir(dir);
}

TEST_CASE("导出文件：口令对才解得开，被改过就解不开") {
    cnk::PortableSecret src;
    src.profile = "202400000001";
    src.username = "202400000001@cmcc";
    src.password = "p@ss 空格也有";

    std::string json;
    std::string err;
    REQUIRE(cnk::ExportPortableSecret(src, "一句口令-123", json, err));
    CHECK(json.find("p@ss 空格也有") == std::string::npos);   // 文件里不能有明文
    CHECK(json.find("cnk-secret") != std::string::npos);

    cnk::PortableSecret out;
    REQUIRE(cnk::ImportPortableSecret(json, "一句口令-123", out, err));
    CHECK(out.password == src.password);
    CHECK(out.profile == src.profile);
    CHECK(out.username == src.username);

    // 口令错
    cnk::PortableSecret wrong;
    CHECK_FALSE(cnk::ImportPortableSecret(json, "错误口令", wrong, err));
    CHECK(err.find("口令") != std::string::npos);

    // 密文被改一个字符
    std::string tampered = json;
    const size_t pos = tampered.find("\"data\": \"");
    REQUIRE(pos != std::string::npos);
    const size_t ch = pos + 9;
    tampered[ch] = (tampered[ch] == 'A') ? 'B' : 'A';
    CHECK_FALSE(cnk::ImportPortableSecret(tampered, "一句口令-123", out, err));

    // 空口令（导出失败会清空出参，所以不能复用 json 变量）
    std::string ignored;
    CHECK_FALSE(cnk::ExportPortableSecret(src, "", ignored, err));

    // 版本不支持
    std::string other = json;
    const size_t vpos = other.find("\"version\": 1");
    REQUIRE(vpos != std::string::npos);
    other.replace(vpos, 12, "\"version\": 9");
    CHECK_FALSE(cnk::ImportPortableSecret(other, "一句口令-123", out, err));

    // 迭代次数异常（防恶意文件卡死）
    std::string it = json;
    const size_t ipos = it.find("\"iter\": 200000");
    REQUIRE(ipos != std::string::npos);
    it.replace(ipos, 15, "\"iter\": 999999999");
    CHECK_FALSE(cnk::ImportPortableSecret(it, "一句口令-123", out, err));
}
