#include "doctest.h"
#include "auth/AuthFactory.h"
#include "sim/SimServer.h"

namespace {

cnk::Profile MakeP(const std::string& base, const std::string& u, const std::string& p,
                   const std::string& preset) {
    cnk::Profile pr;
    pr.auth_url = base + "/";
    pr.username = u;
    pr.password = p;
    pr.vendor = "eportal";
    pr.isp.preset = preset;
    return pr;
}

}  // namespace

TEST_CASE("锐捷正确凭据登录成功") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "alice";
    o.valid_pass = "pw";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;
    auto p = MakeP(srv.BaseUrl(), "alice", "pw", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    CHECK(std::string(auth->Name()) == "EportalAuth");
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    CHECK(r.request_url.find("c=Portal") != std::string::npos);
    CHECK(r.request_url.find("a=login") != std::string::npos);
    CHECK(r.request_url.find("user_account=alice") != std::string::npos);
    CHECK(r.request_url.find("user_password=pw") != std::string::npos);
    CHECK(srv.LoggedIn());
    srv.Stop();
}

TEST_CASE("锐捷错误凭据返回失败并带服务器消息") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "alice";
    o.valid_pass = "right";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;
    auto p = MakeP(srv.BaseUrl(), "alice", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号或密码错误") != std::string::npos);
    // 跨厂商不得透传厂商码：本项目里码 2 有「凭据错误→停止重试」的约定语义
    CHECK(r.vendor_code != 2);
    CHECK_FALSE(srv.LoggedIn());
    srv.Stop();
}

TEST_CASE("锐捷运营商写入 user_account 的第三段") {
    // IspSuffix 按厂商分支：eportal 下 telecom→telecom（不带 @）
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "bob";
    o.isp_suffix = "telecom";  // 模拟器现在整串比对 user_account（服务段也要对）
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;
    auto p = MakeP(srv.BaseUrl(), "bob", "x", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("user_account=bob%2C0%2Ctelecom") != std::string::npos);
    CHECK(r.success);
    srv.Stop();
}

TEST_CASE("auto 档案必须先换成生效厂商，后缀才取锐捷口径（审阅 P1）") {
    // 配置默认 vendor="auto"，而 IspSuffix 把 auto 当 Dr.COM（telecom→@dx）。
    // Scheduler 与 --test-login 现在会把识别出的厂商写进档案副本再调策略；
    // 否则锐捷/深澜用户会带着 Dr.COM 后缀去认证，被服务器判「账号或密码错误」。
    cnk::Profile auto_p;
    auto_p.vendor = "auto";
    auto_p.isp.preset = "telecom";
    CHECK(cnk::IspSuffix(auto_p) == "@dx");  // 原样 = Dr.COM 口径

    cnk::Profile effective = auto_p;
    effective.vendor = cnk::VendorName(cnk::Vendor::Eportal);
    CHECK(cnk::IspSuffix(effective) == "telecom");   // 改写后 = 锐捷口径
    effective.vendor = cnk::VendorName(cnk::Vendor::Srun);
    CHECK(cnk::IspSuffix(effective) == "@telecom");  // 深澜口径
}

TEST_CASE("锐捷 wlan_user_ip 必须填出真实本机地址而不是 0.0.0.0") {
    // 历史缺口：M1 的 NetContext.local_ip 恒为空，直接取它会提交 0.0.0.0 而被服务器拒绝。
    // 现在按认证网址现算本机源地址（core/LocalIp.h），回环测试下即 127.0.0.1。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "eportal";
    o.valid_user = "alice";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::NetContext ctx;  // 故意不填 local_ip
    auto p = MakeP(srv.BaseUrl(), "alice", "pw", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("wlan_user_ip=127.0.0.1") != std::string::npos);
    CHECK(r.request_url.find("wlan_user_ip=0.0.0.0") == std::string::npos);
    srv.Stop();
}

TEST_CASE("锐捷空账号空密码直接失败且不发请求") {
    cnk::Profile p;
    p.vendor = "eportal";
    p.auth_url = "http://127.0.0.1:1/";
    auto auth = cnk::CreateAuth(cnk::Vendor::Eportal, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号") != std::string::npos);
}
