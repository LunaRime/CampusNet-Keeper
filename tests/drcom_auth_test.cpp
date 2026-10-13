#include "doctest.h"
#include "auth/AuthFactory.h"
#include "sim/SimServer.h"

namespace {
cnk::Profile MakeProfile(const std::string& base, const std::string& user,
                         const std::string& pass, const std::string& preset) {
    cnk::Profile p;
    p.name = "T";
    p.auth_url = base + "/";
    p.username = user;
    p.password = pass;
    p.vendor = "drcom";
    p.isp.preset = preset;
    return p;
}
}

TEST_CASE("正确凭据登录成功") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "pw123";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom,
                                MakeProfile(srv.BaseUrl(), "alice", "pw123", "campus"));
    cnk::NetContext ctx;
    ctx.portal_url = srv.BaseUrl() + "/";
    auto r = auth->Login(MakeProfile(srv.BaseUrl(), "alice", "pw123", "campus"), ctx);
    CHECK(r.success);
    CHECK(r.request_body.find("DDDDD=alice") != std::string::npos);
    CHECK(r.request_body.find("upass=pw123") != std::string::npos);
    srv.Stop();
}

TEST_CASE("错误密码返回失败") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "right";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "alice", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    // 服务器返回明确的失败标志 Dr.COMWebLoginID_2.htm，厂商原始码为 2
    // （计划此处置 -1，但 -1 是「无法从响应判定」保留值，与 spec §4.1
    //  “vendor_code = 厂商返回的原始码”及三级判定语义矛盾，依 spec 裁决为 2）
    CHECK(r.vendor_code == 2);
    srv.Stop();
}

TEST_CASE("运营商后缀被拼到账号后") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "bob";
    o.valid_pass = "pw";
    o.isp_suffix = "@cmcc";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "bob", "pw", "cmcc");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    // 服务器要求账号必须带 @cmcc 才通过，能成功即证明后缀已拼上
    CHECK(r.success);
    CHECK(r.request_body.find("DDDDD=bob%40cmcc") != std::string::npos);
    srv.Stop();
}

TEST_CASE("正文包含全部 17 个规定字段") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "u", "p", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);

    for (const char* field : {"DDDDD=", "upass=", "R1=0", "R2=0", "R3=0", "R6=0",
                              "para=00", "0MKKey=123456", "buttonClicked=",
                              "redirect_url=", "err_flag=", "username=",
                              "password=", "user=", "cmd=", "Login=", "v6ip="})
        CHECK(r.request_body.find(field) != std::string::npos);
    srv.Stop();
}

TEST_CASE("request_url 指向 801 端口的登录路径") {
    cnk::Profile p;
    p.auth_url = "http://10.1.2.3/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("/eportal/") != std::string::npos);
    CHECK(r.request_url.find("c=ACSetting") != std::string::npos);
    CHECK(r.request_url.find("a=Login") != std::string::npos);
    CHECK(r.request_url.find(":801") != std::string::npos);
}

TEST_CASE("auth_url 已带端口时沿用该端口") {
    cnk::Profile p;
    p.auth_url = "http://10.1.2.3:9000/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find(":9000") != std::string::npos);
    CHECK(r.request_url.find(":801") == std::string::npos);
}

TEST_CASE("空密码不发请求直接返回失败") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:1/";
    p.username = "u";
    p.password = "";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("密码") != std::string::npos);
}

TEST_CASE("空账号不发请求直接返回失败") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:1/";
    p.username = "";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号") != std::string::npos);
}

TEST_CASE("服务器无响应时返回失败并带错误信息") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:1/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.size() > 0);
}
