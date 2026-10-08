#include "doctest.h"
#include "sim/SimServer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {

// 极简 HTTP 客户端：发一个请求，读回全部响应字节。
// 仅用于测试断言，不解析响应（正文匹配用 find 就够）。
std::string RawHttp(int port, const std::string& method,
                    const std::string& path, const std::string& body) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        return {};
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    send(s, req.c_str(), (int)req.size(), 0);

    std::string resp;
    char buf[4096];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);

    closesocket(s);
    WSACleanup();
    return resp;
}

}  // namespace

TEST_CASE("模拟服务器能响应门户首页请求") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    std::string resp = RawHttp(srv.Port(), "GET", "/", "");
    CHECK(resp.find("200") != std::string::npos);
    CHECK(resp.find("DrcomServer1.0") != std::string::npos);
    CHECK(resp.find("Dr.COMWebLoginID_") != std::string::npos);

    srv.Stop();
}

TEST_CASE("模拟服务器对正确凭据返回成功标志") {
    cnk::SimOptions opts;
    opts.valid_user = "alice";
    opts.valid_pass = "pw123";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
        "DDDDD=alice&upass=pw123&R1=0&R2=0&R3=0&R6=0&para=00&0MKKey=123456");
    CHECK(resp.find("Dr.COMWebLoginID_3.htm") != std::string::npos);
    CHECK(srv.LoginCount() == 1);

    srv.Stop();
}

TEST_CASE("模拟服务器对错误凭据返回失败标志") {
    cnk::SimOptions opts;
    opts.valid_user = "alice";
    opts.valid_pass = "pw123";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                               "DDDDD=alice&upass=wrong");
    CHECK(resp.find("Dr.COMWebLoginID_2.htm") != std::string::npos);

    srv.Stop();
}

TEST_CASE("模拟服务器按后缀校验运营商") {
    cnk::SimOptions opts;
    opts.valid_user = "bob";
    opts.valid_pass = "pw";
    opts.isp_suffix = "@cmcc";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                               "DDDDD=bob@cmcc&upass=pw");
    CHECK(resp.find("Dr.COMWebLoginID_3.htm") != std::string::npos);

    srv.Stop();
}

TEST_CASE("登录计数随请求累加") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));
    CHECK(srv.LoginCount() == 0);
    RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    CHECK(srv.LoginCount() == 1);
    RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    CHECK(srv.LoginCount() == 2);
    srv.Stop();
}
