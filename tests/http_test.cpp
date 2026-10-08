#include "doctest.h"
#include "core/Http.h"
#include "sim/SimServer.h"
#include <chrono>

TEST_CASE("HttpPerform 能取回正文与 Server 头") {
    cnk::SimServer srv;
    std::string err;
    cnk::SimOptions opts;
    REQUIRE(srv.Start(opts, err));

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/";
    cnk::HttpResponse resp;
    REQUIRE(cnk::HttpPerform(req, resp, err));
    CHECK(resp.status == 200);
    CHECK(resp.server == "DrcomServer1.0");
    CHECK(resp.content_type.find("charset=gbk") != std::string::npos);
    // doctest 无法分解带 || 的表达式，先算成 bool 再断言
    bool body_has_marker = resp.body.find("DrcomServer") != std::string::npos
                        || resp.body.find("Dr.COM") != std::string::npos;
    CHECK(body_has_marker);
    srv.Stop();
}

TEST_CASE("HttpPerform 能发送 POST 正文") {
    cnk::SimServer srv;
    std::string err;
    cnk::SimOptions opts;
    opts.valid_user = "u1";
    opts.valid_pass = "p1";
    REQUIRE(srv.Start(opts, err));

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/eportal/?c=ACSetting&a=Login";
    req.method = "POST";
    req.body = "DDDDD=u1&upass=p1";
    req.content_type = "application/x-www-form-urlencoded";
    cnk::HttpResponse resp;
    REQUIRE(cnk::HttpPerform(req, resp, err));
    CHECK(srv.LastLoginBody().find("DDDDD=u1") != std::string::npos);
    CHECK(srv.LastLoginBody().find("upass=p1") != std::string::npos);
    srv.Stop();
}

TEST_CASE("HttpPerform 超时返回失败而不挂起") {
    cnk::SimServer srv;
    std::string err;
    cnk::SimOptions opts;
    opts.fail_mode = 2;  // 挂起
    REQUIRE(srv.Start(opts, err));

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/";
    req.timeout_ms = 800;
    cnk::HttpResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = cnk::HttpPerform(req, resp, err);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(ok);
    // 上界不用 4000：WinHTTP 的超时粒度约 4 秒，800ms 的配置实测落在 3.93-4.02 秒，
    // 曾测到 4016ms。8000ms 仍足以证明"不挂起"——服务器要挂起 10 秒才关闭。
    CHECK(ms < 8000);
    CHECK(err.size() > 0);
    srv.Stop();
}

TEST_CASE("HttpPerform 连接不存在的端口时快速失败") {
    cnk::HttpRequest req;
    req.url = "http://127.0.0.1:1/";
    req.timeout_ms = 1000;
    cnk::HttpResponse resp;
    std::string err;
    bool ok = cnk::HttpPerform(req, resp, err);
    CHECK_FALSE(ok);
    CHECK(err.size() > 0);
}

TEST_CASE("HttpPerform 对畸形 URL 返回明确错误") {
    cnk::HttpRequest req;
    req.url = "not-a-url";
    cnk::HttpResponse resp;
    std::string err;
    CHECK_FALSE(cnk::HttpPerform(req, resp, err));
    CHECK(err.size() > 0);
}
