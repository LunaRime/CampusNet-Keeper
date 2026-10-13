#include "doctest.h"
#include "auth/AuthFactory.h"
#include "core/Template.h"
#include "sim/SimServer.h"

TEST_CASE("ExpandTemplate 替换已知占位符") {
    std::map<std::string, std::string> vars = {
        {"username", "alice"}, {"password", "pw"}, {"isp", "@cmcc"}};
    CHECK(cnk::ExpandTemplate("{username}:{password}{isp}", vars)
          == "alice:pw@cmcc");
}

TEST_CASE("ExpandTemplate 保留未知占位符") {
    std::map<std::string, std::string> vars = {{"username", "a"}};
    CHECK(cnk::ExpandTemplate("{username}-{unknown}", vars) == "a-{unknown}");
}

TEST_CASE("ExpandTemplate 对空模板返回空") {
    CHECK(cnk::ExpandTemplate("", {}) == "");
}

TEST_CASE("自定义模板能对模拟服务器登录成功") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "carol";
    o.valid_pass = "pw9";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p;
    p.name = "T";
    p.vendor = "custom";
    p.username = "carol";
    p.password = "pw9";
    p.custom_template.method = "POST";
    p.custom_template.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    p.custom_template.body = "DDDDD={username}&upass={password}";
    p.custom_template.success_regex = "Dr\\.COMWebLoginID_3";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    srv.Stop();
}

TEST_CASE("自定义模板正则不匹配时返回失败") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "carol";
    o.valid_pass = "pw9";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p;
    p.vendor = "custom";
    p.username = "carol";
    p.password = "WRONG";
    p.custom_template.method = "POST";
    p.custom_template.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    p.custom_template.body = "DDDDD={username}&upass={password}";
    p.custom_template.success_regex = "Dr\\.COMWebLoginID_3";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    srv.Stop();
}

TEST_CASE("模板 URL 为空时返回明确失败") {
    cnk::Profile p;
    p.vendor = "custom";
    p.username = "u";
    p.password = "p";
    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("URL") != std::string::npos);
}

TEST_CASE("无效正则不崩溃，按失败处理") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p;
    p.vendor = "custom";
    p.username = "u";
    p.password = "p";
    p.custom_template.method = "POST";
    p.custom_template.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    p.custom_template.body = "DDDDD={username}&upass={password}";
    p.custom_template.success_regex = "([unclosed";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    srv.Stop();
}
