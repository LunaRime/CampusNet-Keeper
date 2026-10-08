#include "doctest.h"
#include "sim/SimServer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // MultiByteToWideChar / CP_UTF8；必须排在 winsock2.h 之后
#include <cstdlib>
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

// 用系统 API 把 GBK(936) 字节解成 UTF-8，用于端到端验证服务端真的发了 GBK。
// 返回空串表示解码失败（即字节不是合法 GBK）。
std::string GbkToUtf8(const std::string& gbk) {
    if (gbk.empty()) return {};
    int wn = MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(), nullptr, 0);
    if (wn <= 0) return {};
    std::wstring wide((size_t)wn, L'\0');
    MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(), wide.data(), wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return {};
    std::string utf8((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wn, utf8.data(), un, nullptr, nullptr);
    return utf8;
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

TEST_CASE("门户首页按 GBK 发送，中文运营商名可读") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    std::string resp = RawHttp(srv.Port(), "GET", "/", "");
    size_t hdr_end = resp.find("\r\n\r\n");
    REQUIRE(hdr_end != std::string::npos);
    // 声明侧：头部与 meta 都按真实门户声明 GBK
    CHECK(resp.find("charset=gbk") != std::string::npos);
    CHECK(resp.find("charset=\"gb2312\"") != std::string::npos);

    std::string body = resp.substr(hdr_end + 4);
    // 正文长度必须按转码后的字节算，否则客户端会截断或读超
    size_t cl_pos = resp.find("Content-Length: ");
    REQUIRE(cl_pos != std::string::npos);
    size_t content_length = (size_t)std::strtoul(resp.c_str() + cl_pos + 16, nullptr, 10);
    CHECK(content_length == body.size());

    // 字节侧：用系统 API 按 GBK 解码，中文运营商名必须可读
    std::string utf8 = GbkToUtf8(body);
    CHECK(utf8.find("服务类型") != std::string::npos);
    CHECK(utf8.find("校园用户") != std::string::npos);
    CHECK(utf8.find("校园电信") != std::string::npos);
    CHECK(utf8.find("校园联通") != std::string::npos);
    // 负向：正文不得是 UTF-8 字节序列，证明真的转码了而非两边都能读
    CHECK(body.find("校园用户") == std::string::npos);
    CHECK(body.find("校园电信") == std::string::npos);

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
