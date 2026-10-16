#include "doctest.h"
#include "auth/AuthFactory.h"
#include "core/Encoding.h"
#include "core/Http.h"
#include "core/HtmlForm.h"
#include "sim/SimServer.h"

namespace {

// 一个典型的表单式 Portal 页面（含 hidden、下拉、提交按钮）
const char* kFormPage = R"HTML(
<html><body>
<form name="login" method="post" action="/doLogin">
  <input type="hidden" name="nasip" value="10.0.0.1">
  <input type="hidden" name="wlanuserip" value="10.0.0.5">
  <input type="text" name="username" value="">
  <input type="password" name="password" value="">
  <select name="domain"><option value="@cmcc">移动</option></select>
  <input type="submit" value="登录">
</form>
</body></html>
)HTML";

cnk::Profile MakeGeneric(const std::string& url, const std::string& u,
                         const std::string& p, const std::string& preset) {
    cnk::Profile pr;
    pr.auth_url = url;
    pr.username = u;
    pr.password = p;
    pr.vendor = "generic";
    pr.isp.preset = preset;
    return pr;
}

}  // namespace

TEST_CASE("识别并填充标准表单字段（含隐藏字段与下拉）") {
    cnk::Profile p = MakeGeneric("http://10.0.0.1/", "alice", "pw", "cmcc");
    cnk::NetContext ctx;
    ctx.portal_body = kFormPage;

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    CHECK(std::string(auth->Name()) == "GenericFormAuth");

    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("/doLogin") != std::string::npos);
    CHECK(r.request_body.find("username=alice") != std::string::npos);
    CHECK(r.request_body.find("password=pw") != std::string::npos);
    // generic 厂商下 IspSuffix(cmcc) = "cmcc"（不带 @）
    CHECK(r.request_body.find("domain=cmcc") != std::string::npos);
    // 隐藏字段必须原样保留
    CHECK(r.request_body.find("nasip=10.0.0.1") != std::string::npos);
    CHECK(r.request_body.find("wlanuserip=10.0.0.5") != std::string::npos);
    // 提交按钮不是固定字段，不该进请求体
    CHECK(r.request_body.find("submit") == std::string::npos);
}

TEST_CASE("页面无表单时返回明确失败并提示自定义模板") {
    cnk::Profile p = MakeGeneric("http://10.0.0.1/", "u", "p", "campus");
    cnk::NetContext ctx;
    ctx.portal_body = "<html><body>no form here</body></html>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("自定义模板") != std::string::npos);
}

TEST_CASE("空 portal_body 时返回明确失败") {
    cnk::Profile p = MakeGeneric("http://10.0.0.1/", "u", "p", "campus");
    cnk::NetContext ctx;

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.size() > 0);
}

TEST_CASE("相对 action 在 portal_url 为空时退回 auth_url") {
    cnk::Profile p = MakeGeneric("http://10.0.0.1/portal/", "u", "p", "campus");
    cnk::NetContext ctx;
    ctx.portal_body = kFormPage;

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("10.0.0.1") != std::string::npos);
    CHECK(r.request_url.find("/doLogin") != std::string::npos);
}

TEST_CASE("相对 action 优先相对 portal_url 解析（审阅注记）") {
    // spec §4.2：relative action 相对 Portal **页面** URL 解析。
    // ⚠ 已知边界：ResolveUrl 对不以 '/' 开头的 path-relative action 按 origin 拼接，
    // 不是 RFC 3986 目录语义 —— 根相对 action（真实 Portal 形态）正确，子路径 action
    // 会丢目录前缀（见 core/Url.h 与计划任务 21 的注记，留给后续扩展）。
    cnk::Profile p = MakeGeneric("http://auth.example/", "u", "p", "campus");
    cnk::NetContext ctx;
    ctx.portal_body = kFormPage;
    ctx.portal_url = "http://page.example/portal/index.html";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    // 用 portal_url 的 origin（page.example），而不是 auth_url（auth.example）
    CHECK(r.request_url.find("page.example") != std::string::npos);
    CHECK(r.request_url.find("auth.example") == std::string::npos);
}

TEST_CASE("DDDDD 用户名变体也能识别") {
    cnk::Profile p = MakeGeneric("http://10.0.0.1/", "carol", "pw", "campus");
    cnk::NetContext ctx;
    ctx.portal_body =
        "<form action='/l' method='post'>"
        "<input name='DDDDD' value=''><input name='upass' value=''></form>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_body.find("DDDDD=carol") != std::string::npos);
    CHECK(r.request_body.find("upass=pw") != std::string::npos);
}

TEST_CASE("隐藏字段不会被误当账号字段（包含匹配跳过 hidden）") {
    // 只有 hidden 的 wlanuserip 与普通 username：必须选 username
    cnk::Profile p = MakeGeneric("http://10.0.0.1/", "dave", "pw", "campus");
    cnk::NetContext ctx;
    ctx.portal_body =
        "<form action='/l' method='post'>"
        "<input type='hidden' name='wlanuserip' value='10.0.0.9'>"
        "<input type='text' name='account_no'>"
        "<input type='password' name='passwd'></form>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_body.find("account_no=dave") != std::string::npos);
    CHECK(r.request_body.find("passwd=pw") != std::string::npos);
    CHECK(r.request_body.find("wlanuserip=10.0.0.9") != std::string::npos);
}

TEST_CASE("GET 表单把字段拼到查询串") {
    cnk::Profile p = MakeGeneric("http://10.0.0.1/", "u", "p", "campus");
    cnk::NetContext ctx;
    ctx.portal_body =
        "<form action='/login' method='get'>"
        "<input name='username' value=''><input name='password' value=''></form>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("/login?") != std::string::npos);
    CHECK(r.request_url.find("username=u") != std::string::npos);
}

TEST_CASE("HtmlForm：解析属性边界与 select 选中项") {
    cnk::HtmlForm f;
    REQUIRE(cnk::ParseFirstForm(kFormPage, f));
    CHECK(f.action == "/doLogin");
    CHECK(f.method == "post");
    REQUIRE(f.fields.size() == 5);  // 2 hidden + 2 文本 + 1 select（submit 无 name 且被跳过）
    CHECK(f.fields[0].name == "nasip");
    CHECK(f.fields[0].type == "hidden");
    const std::string isp_field =
        cnk::FindFieldByNames(f, {"domain", "service", "isp", "operator", "type"});
    CHECK(isp_field == "domain");

    // 属性名必须落在边界：'wlanuserip' 里含 'userip'，但 name 不应被它干扰
    cnk::HtmlForm g;
    REQUIRE(cnk::ParseFirstForm(
        "<form action='/x'><input name='wlanuserip' value='1'></form>", g));
    CHECK(g.fields.size() == 1);
    CHECK(g.fields[0].name == "wlanuserip");

    CHECK_FALSE(cnk::ParseFirstForm("<html>no form</html>", g));
}

TEST_CASE("HtmlForm：注释里的旧表单不算表单（审阅 P0）") {
    cnk::HtmlForm f;
    REQUIRE(cnk::ParseFirstForm(
        "<!-- <form action='/old'><input name='olduser'></form> -->"
        "<form action='/new' method='post'><input name='username'>"
        "<input name='password'></form>",
        f));
    CHECK(f.action == "/new");
    CHECK(cnk::FindFieldByNames(f, {"username", "user", "account", "name"}) == "username");
}

TEST_CASE("HtmlForm：hidden 字段不参与账号/密码字段匹配（审阅 P0）") {
    cnk::HtmlForm f;
    REQUIRE(cnk::ParseFirstForm("<form action='/x'>"
                                "<input type='hidden' name='user' value=''>"
                                "<input type='text' name='user_account' value=''>"
                                "<input type='hidden' name='pass'>"
                                "<input type='password' name='user_password'></form>",
                                f));
    CHECK(cnk::FindFieldByNames(
              f, {"username", "user", "account", "name", "DDDDD", "user_account",
                  "user_name", "login_name"}) == "user_account");
    CHECK(cnk::FindFieldByNames(
              f, {"password", "pwd", "pass", "upass", "user_password", "passwd"}) ==
          "user_password");
    // 隐藏字段本身仍会被收集并原样提交（只是不做字段名匹配）
    CHECK(f.fields[0].name == "user");
    CHECK(f.fields[0].type == "hidden");
}

TEST_CASE("HtmlForm：HTML 实体被解码（审阅 P0）") {
    cnk::HtmlForm f;
    REQUIRE(cnk::ParseFirstForm("<form action='/eportal/?c=Portal&amp;a=login' method='post'>"
                                "<input name='username' value='a&amp;b'></form>",
                                f));
    CHECK(f.action == "/eportal/?c=Portal&a=login");
    REQUIRE(f.fields.size() == 1);
    CHECK(f.fields[0].value == "a&b");
}

TEST_CASE("HtmlForm：包含匹配不使用过短候选词（审阅 P1）") {
    cnk::HtmlForm f;
    REQUIRE(cnk::ParseFirstForm(
        "<form action='/x'><input name='userip'><input name='pass_count'></form>", f));
    // 短词（user/pass）只在精确轮生效；包含轮要求候选长度 ≥5，
    // 因此 userip / pass_count 不该被当作账号/密码字段
    CHECK(cnk::FindFieldByNames(f, {"username", "user", "account", "name"}).empty());
    CHECK(cnk::FindFieldByNames(f, {"password", "pwd", "pass", "upass"}).empty());
}

TEST_CASE("通用表单：重定向响应不得判成功（审阅 P1）") {
    // 登录成功后回跳是常见形态，但没有响应体可判 → 如实报「无法判定」交下一次探测。
    // 绝不猜成功：Scheduler 拿到 success 会直接置 Online 并 ResetCache。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "generic";
    o.valid_user = "u";
    o.valid_pass = "p";
    o.isp_suffix = "cmcc";
    o.fail_mode = 3;  // 全局 302 旋钮：所有路径都跳走
    o.redirect_location = "http://example.invalid/after";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Profile p = MakeGeneric(srv.BaseUrl() + "/", "u", "p", "cmcc");
    cnk::NetContext ctx;
    ctx.portal_url = srv.BaseUrl() + "/";
    ctx.portal_body =
        "<form action='/doLogin' method='post'>"
        "<input name='username'><input name='password'></form>";

    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("重定向") != std::string::npos);
    srv.Stop();
}

TEST_CASE("通用表单：模拟门户端到端（识别 unknown → 兜底策略）") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "generic";
    o.valid_user = "alice";
    o.valid_pass = "pw";
    o.isp_suffix = "cmcc";  // generic 厂商下 IspSuffix(cmcc) = "cmcc"
    std::string err;
    REQUIRE(srv.Start(o, err));

    // 产品里这一步由 ResolvePortalPage 完成：抓取门户页作为识别与填表输入
    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/";
    req.method = "GET";
    req.timeout_ms = 5000;
    req.follow_redirects = false;
    cnk::HttpResponse resp;
    std::string herr;
    REQUIRE(cnk::HttpPerform(req, resp, herr));

    cnk::NetContext ctx;
    ctx.portal_body = cnk::DecodeBody(resp);
    ctx.portal_url = srv.BaseUrl() + "/";

    cnk::Profile p = MakeGeneric(srv.BaseUrl() + "/", "alice", "pw", "cmcc");
    auto auth = cnk::CreateAuth(cnk::Vendor::Unknown, p);
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    CHECK(srv.LoggedIn());
    srv.Stop();
}
