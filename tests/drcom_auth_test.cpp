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
    // 线上真实载荷（模拟器解密后留存）：账号带 ,0, 前缀
    CHECK(srv.LastLoginBody().find(",0,alice") != std::string::npos);
    CHECK(srv.LastLoginBody().find("\"user_password\":\"pw123\"") != std::string::npos);
    // 展示用请求体必须整体打码：密码、账号（含学号）与 rcn 一律不回显
    //（2026-10-16 审阅 P1：新式载荷是 JSON，旧的 key=value 脱敏对它无效 → 回归）
    CHECK(r.request_body.find(",0,alice") == std::string::npos);
    CHECK(r.request_body.find("pw123") == std::string::npos);
    CHECK(r.request_body.find("\"user_account\":\",0,***\"") != std::string::npos);
    CHECK(r.request_body.find("\"user_password\":\"***\"") != std::string::npos);
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
    // 新式接口对凭据错误回 {"result":0,"msg":"账号或密码错误"}（2026-10-16 实测协议）；
    // 文案命中「账号/密码」→ 归入 vendor_code=2（凭据类），触发调度器的停止重试保护
    CHECK(r.vendor_code == 2);
    CHECK(r.message.find("账号或密码错误") != std::string::npos);
    srv.Stop();
}

TEST_CASE("服务器报「终端IP已在线」时判成功，且不得误判为凭据错误（2026-10-16 线上实测）") {
    // 线上真实响应（本机 10.0.0.1 实测，正确账号 + 正确密码，在线态复登）：
    //   dr1005({"result":0,"msg":"终端IP已在线！","ret_code":2})
    // 语义是「链路已通、无需重新登录」。若判失败，会同时坏两处：
    //   ① --test-login 报误导性的「登录失败」；
    //   ② 调度器走退避重试，连续 3 次后落入「非校园网」保护并停止重试。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "pw";
    o.already_online = true;
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "alice", "pw", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    CHECK(r.vendor_code != 2);  // 绝不可是「凭据错误→停止重试」那一类
    CHECK(r.message.find("已在线") != std::string::npos);
    srv.Stop();
}

TEST_CASE("新式接口在场但登录不可用：老式回退的「凭据错误」不得采信（审阅 P1）") {
    // 本部署老式通道恒回 userid error2（与凭据对错无关）。若新式接口明明在场
    // （loadConfig 正常）却因登录步骤故障而回退，又把 error2 当成密码错误上报，
    // 调度器就会 AuthFailed + stop_retrying=true——正是本项目踩过的那次误诊。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "right";
    o.new_api_login_broken = true;  // 新式登录端点 404，但 loadConfig/chkstatus 正常
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "alice", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.vendor_code != 2);                          // 不得当凭据错误（否则停止重试）
    CHECK(r.message.find("新式") != std::string::npos);  // 必须带上真实原因
    srv.Stop();
}

TEST_CASE("老部署（新式接口不存在）时，老式通道的凭据错误仍归类为凭据错误") {
    // 回退通道的判定并非一律不可信：接口整体不存在（旧部署）时它是唯一依据
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "alice";
    o.valid_pass = "right";
    o.disable_new_api = true;
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "alice", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.vendor_code == 2);
    srv.Stop();
}

TEST_CASE("失败文案分类：只有凭据类才判 vendor_code=2，普通故障文案不得停止重试") {
    // 审阅 P1：「获取用户信息失败，请稍后重试」含「用户」二字，旧启发式会把它当成
    // 密码错误 → 永久停止自动重试。
    auto login_with_msg = [](const std::string& msg, int& code, bool& ok) {
        cnk::SimServer srv;
        cnk::SimOptions o;
        o.new_api_fail_msg = msg;
        std::string err;
        if (!srv.Start(o, err)) return false;
        auto p = MakeProfile(srv.BaseUrl(), "alice", "pw", "campus");
        auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
        cnk::NetContext ctx;
        auto r = auth->Login(p, ctx);
        code = r.vendor_code;
        ok = r.success;
        srv.Stop();
        return true;
    };

    int code = 0;
    bool ok = true;
    REQUIRE(login_with_msg("获取用户信息失败，请稍后重试", code, ok));
    CHECK_FALSE(ok);
    CHECK(code == -1);  // 普通故障：交重试裁决，不得停止重试

    REQUIRE(login_with_msg("账号或密码错误", code, ok));
    CHECK_FALSE(ok);
    CHECK(code == 2);  // 真凭据错误：停止重试
}

TEST_CASE("「账号已在线」不等于本终端在线：不得判成功") {
    // 只有「终端 IP 已在线」才说明本机链路已通；账号在别处在线是另一种情况
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.new_api_fail_msg = "该账号已在线，请先下线";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "alice", "pw", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
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
    // 服务器要求账号必须带 @cmcc 才通过，能成功即证明后缀已拼上；
    // 断言用「线上真实载荷」（模拟器解密留存），而非打码后的展示文本
    CHECK(r.success);
    CHECK(srv.LastLoginBody().find(",0,bob@cmcc") != std::string::npos);
    srv.Stop();
}

TEST_CASE("老协议回退通道：正文包含全部 17 个规定字段") {
    // 新式接口在旧版本部署上不存在时走此通道（disable_new_api 模拟该情形）
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.disable_new_api = true;
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

TEST_CASE("无端口的 auth_url：登录走同源（不再默认 801）") {
    // 2026-10-16 实测修订：老式 c=ACSetting 协议与门户页同源（:801 只回 SPA 外壳），
    // auth_url 未显式写端口时不补默认端口，由 scheme 缺省（http→80 / https→443）
    cnk::Profile p;
    // 回环地址：连接立即被拒，测试无需等超时，也不会把凭据发往外部主机
    p.auth_url = "http://127.0.0.1/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("http://127.0.0.1/eportal/") == 0);
    CHECK(r.request_url.find(":801") == std::string::npos);
    CHECK(r.request_url.find("c=ACSetting") != std::string::npos);
    CHECK(r.request_url.find("a=Login") != std::string::npos);
}

TEST_CASE("auth_url 已带端口时沿用该端口") {
    cnk::Profile p;
    p.auth_url = "http://127.0.0.1:9000/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find(":9000") != std::string::npos);
    CHECK(r.request_url.find(":801") == std::string::npos);
}

TEST_CASE("https 无端口 auth_url：登录同源 https（不再补 802）") {
    cnk::Profile p;
    p.auth_url = "https://127.0.0.1/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.request_url.find("https://127.0.0.1/eportal/") == 0);
    CHECK(r.request_url.find(":802") == std::string::npos);
}

TEST_CASE("认证网址缺 scheme 时直接返回格式错误") {
    cnk::Profile p;
    p.auth_url = "10.0.0.1";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("格式") != std::string::npos);
}

TEST_CASE("空密码不发请求直接返回失败") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "u", "", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("密码") != std::string::npos);
    CHECK(srv.LoginCount() == 0);  // 证明未发出任何请求
    srv.Stop();
}

TEST_CASE("空账号不发请求直接返回失败") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "", "p", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号") != std::string::npos);
    CHECK(srv.LoginCount() == 0);  // 证明未发出任何请求
    srv.Stop();
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

TEST_CASE("响应无成功失败标志时返回 -1 交由探测判定") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.fail_mode = 3;
    o.redirect_location = "http://example.invalid/nowhere";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "u", "p", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.vendor_code == -1);
    CHECK(r.message.size() > 0);
    srv.Stop();
}

TEST_CASE("302 重定向 Location 含成功标记时判定成功") {
    // spec §7.2：响应 URL 或响应体出现成功标志即成功。
    // 老式 c=ACSetting 入口以 302 跳到 3.htm 是常见形态（follow_redirects=false
    // 时由 Location 头承载该信息）。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.fail_mode = 3;
    o.redirect_location = "http://portal/Dr.COMWebLoginID_3.htm";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "u", "p", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    CHECK(r.vendor_code == 3);
    srv.Stop();
}

TEST_CASE("失败响应回显双标志登录页时返回 -1 不误判成功") {
    // 某些 Portal 失败时回吐完整登录页，其框架脚本同时含 authsuccess 与
    // authfail 两个标志（真实首页即如此）。此时不得乐观判成功——否则调度器
    // 会跳过探测复验，并绕过凭据错误的停止重试逻辑。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.fail_mode = 4;
    o.disable_new_api = true;  // 本用例验证老通道的失败形态
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeProfile(srv.BaseUrl(), "u", "wrong", "campus");
    auto auth = cnk::CreateAuth(cnk::Vendor::Drcom, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.vendor_code == -1);
    srv.Stop();
}
