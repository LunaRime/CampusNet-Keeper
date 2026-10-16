#include "doctest.h"

#include "core/ConfigForUi.h"
#include "store/Config.h"
#include "json.hpp"

namespace {

cnk::AppConfig MakeConfig(bool with_password) {
    cnk::AppConfig cfg;
    cfg.active_profile = "默认";
    cnk::Profile p;
    p.name = "默认";
    p.auth_url = "http://10.0.0.1/";
    p.username = "202400000001@cmcc";
    p.vendor = "auto";
    p.isp.preset = "cmcc";
    if (with_password) {
        p.password = "旧密码";
        p.has_password = true;
    }
    cfg.profiles.push_back(p);
    return cfg;
}

}  // namespace

TEST_CASE("给 UI 的配置绝不包含 password_enc，只给 has_password") {
    // 安全红线：password_enc 是 DPAPI 密文，而同为当前用户的 UI 进程能解开它，
    // 直接透传等于把密码交出去。UI 只需要知道"有没有密码"。
    const auto cfg = MakeConfig(true);
    const std::string json = cnk::ConfigForUiJson(cfg);

    CHECK(json.find("password_enc") == std::string::npos);
    CHECK(json.find("旧密码") == std::string::npos);  // 明文更不能出现
    const auto j = nlohmann::json::parse(json);
    REQUIRE(j.contains("profiles"));
    REQUIRE(j["profiles"].size() == 1);
    CHECK(j["profiles"][0].value("has_password", false) == true);
    CHECK(j["profiles"][0].value("username", std::string{}) == "202400000001@cmcc");
}

TEST_CASE("没有密码的档案 has_password=false") {
    const std::string json = cnk::ConfigForUiJson(MakeConfig(false));
    const auto j = nlohmann::json::parse(json);
    CHECK(j["profiles"][0].value("has_password", true) == false);
}

TEST_CASE("UI 送来新密码时采用新值") {
    cnk::AppConfig existing = MakeConfig(true);
    cnk::AppConfig out;
    std::string err;
    const std::string ui = R"({"active_profile":"默认","settings":{},"profiles":[
        {"name":"默认","auth_url":"http://10.0.0.1/","username":"u","vendor":"auto",
         "isp":{"preset":"cmcc","custom":""},"password_plain":"新密码"}]})";
    REQUIRE(cnk::MergeUiConfig(ui, existing, out, err));
    REQUIRE(out.profiles.size() == 1);
    CHECK(out.profiles[0].password == "新密码");
}

TEST_CASE("UI 不带新密码时沿用旧密码（按档案名匹配）") {
    cnk::AppConfig existing = MakeConfig(true);
    cnk::AppConfig out;
    std::string err;
    const std::string ui = R"({"active_profile":"默认","settings":{},"profiles":[
        {"name":"默认","auth_url":"http://10.0.0.1/","username":"u","vendor":"auto",
         "isp":{"preset":"cmcc","custom":""}}]})";
    REQUIRE(cnk::MergeUiConfig(ui, existing, out, err));
    REQUIRE(out.profiles.size() == 1);
    CHECK(out.profiles[0].password == "旧密码");
}

TEST_CASE("password_plain 为空串同样视为不改密码") {
    cnk::AppConfig existing = MakeConfig(true);
    cnk::AppConfig out;
    std::string err;
    const std::string ui = R"({"active_profile":"默认","profiles":[
        {"name":"默认","password_plain":""}]})";
    REQUIRE(cnk::MergeUiConfig(ui, existing, out, err));
    CHECK(out.profiles[0].password == "旧密码");
}

TEST_CASE("UI 新增的档案没有旧密码可沿用") {
    cnk::AppConfig existing = MakeConfig(true);
    cnk::AppConfig out;
    std::string err;
    const std::string ui = R"({"active_profile":"新的","profiles":[
        {"name":"新的","auth_url":"http://a/","username":"u2"}]})";
    REQUIRE(cnk::MergeUiConfig(ui, existing, out, err));
    REQUIRE(out.profiles.size() == 1);
    CHECK(out.profiles[0].name == "新的");
    CHECK(out.profiles[0].password.empty());
}

TEST_CASE("非法输入返回 false 并给出可读错误（不抛异常）") {
    cnk::AppConfig existing = MakeConfig(false);
    cnk::AppConfig out;
    std::string err;

    CHECK_FALSE(cnk::MergeUiConfig("not json", existing, out, err));
    CHECK(err.size() > 0);

    CHECK_FALSE(cnk::MergeUiConfig("[1,2,3]", existing, out, err));
    CHECK(err.size() > 0);

    // 缺 profiles：不是合法配置
    CHECK_FALSE(cnk::MergeUiConfig(R"({"active_profile":"默认"})", existing, out, err));
    CHECK(err.size() > 0);
}
