#include "doctest.h"
#include "store/Dpapi.h"
#include "store/Config.h"
#include <filesystem>

TEST_CASE("DPAPI 加解密往返") {
    std::string blob, plain;
    REQUIRE(cnk::DpapiProtect("my-secret-pw", blob));
    CHECK(blob != "my-secret-pw");
    CHECK(blob.size() > 10);
    REQUIRE(cnk::DpapiUnprotect(blob, plain));
    CHECK(plain == "my-secret-pw");
}

TEST_CASE("DPAPI 解密垃圾数据返回失败而不崩溃") {
    std::string plain;
    CHECK_FALSE(cnk::DpapiUnprotect("not-a-valid-blob", plain));
}

TEST_CASE("IspSuffix 按厂商映射") {
    cnk::Profile p;
    p.vendor = "drcom";
    p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(p) == "@dx");
    p.isp.preset = "cmcc";
    CHECK(cnk::IspSuffix(p) == "@cmcc");
    p.isp.preset = "campus";
    CHECK(cnk::IspSuffix(p) == "");
    p.isp.preset = "custom";
    p.isp.custom = "@xyz";
    CHECK(cnk::IspSuffix(p) == "@xyz");

    p.vendor = "srun";
    p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(p) == "@telecom");
    p.vendor = "eportal";
    p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(p) == "telecom");
}

TEST_CASE("AppConfig JSON 往返保持一致") {
    cnk::AppConfig cfg;
    cfg.active_profile = "宿舍";
    cfg.settings.probe_interval_seconds = 7;
    cnk::Profile p;
    p.name = "宿舍";
    p.auth_url = "http://10.0.0.1";
    p.username = "u";
    p.password = "pw";
    p.has_password = true;
    p.vendor = "drcom";
    p.isp.preset = "cmcc";
    cfg.profiles.push_back(p);

    std::string json_out, err;
    REQUIRE(cnk::ConfigToJson(cfg, json_out));
    cnk::AppConfig back;
    REQUIRE(cnk::ConfigFromJson(json_out, back, err));
    CHECK(back.active_profile == "宿舍");
    CHECK(back.settings.probe_interval_seconds == 7);
    REQUIRE(back.profiles.size() == 1);
    CHECK(back.profiles[0].username == "u");
    CHECK(back.profiles[0].password == "pw");
    CHECK(back.profiles[0].has_password);
    CHECK(back.profiles[0].isp.preset == "cmcc");
}

TEST_CASE("损坏的 JSON 返回失败而不崩溃") {
    cnk::AppConfig back;
    std::string err;
    CHECK_FALSE(cnk::ConfigFromJson("{ this is not json", back, err));
    CHECK(err.size() > 0);
}

TEST_CASE("类型错误的 JSON 返回失败而不崩溃") {
    // 用户手改配置写错类型时，nlohmann 的 value() 会抛 type_error。
    // 必须被捕获为普通失败，而不是让进程崩溃（Review Focus #3）。
    cnk::AppConfig back;
    std::string err;
    CHECK_FALSE(cnk::ConfigFromJson(R"({"version":"x"})", back, err));
    CHECK(err.size() > 0);
    CHECK_FALSE(cnk::ConfigFromJson(R"({"profiles":[{"name":123}]})", back, err));
    CHECK(err.size() > 0);
    CHECK_FALSE(cnk::ConfigFromJson(R"({"settings":{"probe_interval_seconds":"5"}})",
                                    back, err));
}

TEST_CASE("缺字段的 JSON 使用默认值") {
    cnk::AppConfig back;
    std::string err;
    REQUIRE(cnk::ConfigFromJson("{}", back, err));
    CHECK(back.version == 1);
    CHECK(back.settings.probe_interval_seconds == 5);
    CHECK(back.profiles.empty());
}

TEST_CASE("ActiveProfile 按名字查找，找不到返回第一个") {
    cnk::AppConfig cfg;
    cnk::Profile a; a.name = "A";
    cnk::Profile b; b.name = "B";
    cfg.profiles = {a, b};
    cfg.active_profile = "B";
    CHECK(cnk::ActiveProfile(cfg)->name == "B");
    cfg.active_profile = "不存在";
    CHECK(cnk::ActiveProfile(cfg)->name == "A");

    cnk::AppConfig empty;
    CHECK(cnk::ActiveProfile(empty) == nullptr);
}
