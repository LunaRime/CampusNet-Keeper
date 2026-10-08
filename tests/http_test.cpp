#include "doctest.h"
#include "core/Http.h"
#include "sim/SimServer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // 必须排在 winsock2.h 之后
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

// 慢滴服务器：响应头立刻回，正文每 interval_ms 发一字节，共 bytes 字节。
// 专用于验证"单步超时挡不住"的场景——每次读都能在单步超时内拿到一点数据，
// 只有总时限能让它停下来。
struct DripServer {
    SOCKET listen_sock = INVALID_SOCKET;
    int port = 0;
    // 连接线程会被 detach，故用 shared_ptr 保证 Stop 后标志仍然有效
    std::shared_ptr<std::atomic<bool>> run = std::make_shared<std::atomic<bool>>(true);
    std::thread worker;

    DripServer(int interval_ms, int bytes) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int one = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 让系统分配
        bind(listen_sock, (sockaddr*)&addr, sizeof(addr));
        int len = sizeof(addr);
        getsockname(listen_sock, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
        listen(listen_sock, SOMAXCONN);

        auto stop = run;
        worker = std::thread([this, stop, interval_ms, bytes] {
            while (stop->load()) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(listen_sock, &rfds);
                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = 50000;  // 50ms：Stop 不必等满全程
                if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET client = accept(listen_sock, nullptr, nullptr);
                if (client == INVALID_SOCKET) continue;
                std::thread([client, stop, interval_ms, bytes] {
                    char buf[2048];
                    recv(client, buf, sizeof(buf), 0);  // 吃掉请求即可，不解析
                    std::string head = "HTTP/1.1 200 OK\r\nServer: DripSrv\r\nContent-Length: " +
                                       std::to_string(bytes) + "\r\nConnection: close\r\n\r\n";
                    send(client, head.c_str(), (int)head.size(), 0);
                    for (int i = 0; i < bytes && stop->load(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                        if (!stop->load()) break;
                        if (send(client, "x", 1, 0) <= 0) break;
                    }
                    closesocket(client);
                }).detach();
            }
        });
    }

    DripServer(const DripServer&) = delete;
    DripServer& operator=(const DripServer&) = delete;

    ~DripServer() {
        run->store(false);
        if (worker.joinable()) worker.join();
        if (listen_sock != INVALID_SOCKET) closesocket(listen_sock);
    }

    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port); }
};

}  // namespace

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
    CHECK(resp.status == 0);  // 契约：返回 false 时 status 归零，只看 status 也不会误判
}

// 这两条是"不跟随重定向"的护栏：WinHTTP 默认会跟随（同协议最多 10 跳），
// 若 Http.cpp 里显式关闭重定向的代码被删掉，劫持检测会静默失效。
// 用两台服务器：hijack 一律 302，终点 portal 记录自己收到了几个请求。
// 有了它们，"有没有跟过去"是直接断言出来的，而不是靠状态码间接推断。
TEST_CASE("HttpPerform 默认不跟随重定向，能看见 302 与 Location") {
    cnk::SimServer portal, hijack;
    std::string err;

    cnk::SimOptions popts;  // 终点：正常服务器，只用来数请求
    REQUIRE(portal.Start(popts, err));

    cnk::SimOptions hopts;
    hopts.fail_mode = 3;
    hopts.redirect_location = portal.BaseUrl() + "/eportal/";
    REQUIRE(hijack.Start(hopts, err));

    cnk::HttpRequest req;
    req.url = hijack.BaseUrl() + "/generate_204";
    cnk::HttpResponse resp;
    REQUIRE(cnk::HttpPerform(req, resp, err));
    CHECK(resp.status == 302);
    CHECK(resp.location == hopts.redirect_location);
    CHECK(portal.LoginCount() == 0);  // 没跟过去：终点服务器一个请求都没收到
    hijack.Stop();
    portal.Stop();
}

TEST_CASE("HttpPerform 跟随重定向时取回终点内容与终址") {
    cnk::SimServer portal, hijack;
    std::string err;

    cnk::SimOptions popts;
    REQUIRE(portal.Start(popts, err));

    cnk::SimOptions hopts;
    hopts.fail_mode = 3;
    hopts.redirect_location = portal.BaseUrl() + "/eportal/";
    REQUIRE(hijack.Start(hopts, err));

    cnk::HttpRequest req;
    req.url = hijack.BaseUrl() + "/generate_204";
    req.follow_redirects = true;
    cnk::HttpResponse resp;
    REQUIRE(cnk::HttpPerform(req, resp, err));
    CHECK(resp.status == 200);                          // 重定向已被跟随，只剩终点响应
    CHECK(portal.LoginCount() == 1);                    // 确实跟到了终点服务器
    CHECK(resp.effective_url == hopts.redirect_location);  // 终址，而不是 req.url
    hijack.Stop();
    portal.Stop();
}

TEST_CASE("HttpPerform 慢滴响应由总时限兜底，不返回半截正文") {
    DripServer srv(1500, 5);  // 每 1.5 秒 1 字节，共 5 字节：滴完要 7.5 秒

    cnk::HttpRequest req;
    req.url = srv.BaseUrl() + "/drip";
    req.timeout_ms = 3000;        // 单步超时：每次读都能按时拿到数据，永远不触发
    req.total_timeout_ms = 2500;  // 显式覆盖，且故意小于单步超时——叫停的只能是总时限
    cnk::HttpResponse resp;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = cnk::HttpPerform(req, resp, err);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();

    CHECK_FALSE(ok);
    CHECK(err.find("总时限") != std::string::npos);  // 是总时限叫停的，不是别的错误
    CHECK(resp.status == 0);                         // 失败即 status 归零
    CHECK(resp.body.empty());                        // 不返回半截正文
    CHECK(ms < 6000);                                // 远早于"滴完"的 7.5 秒
}
