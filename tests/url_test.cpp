#include "doctest.h"
#include "core/Url.h"

TEST_CASE("UrlEncode 编码保留字符") {
    CHECK(cnk::UrlEncode("abc123-_.~") == "abc123-_.~");
    CHECK(cnk::UrlEncode("a b") == "a%20b");
    CHECK(cnk::UrlEncode("a@b") == "a%40b");
    CHECK(cnk::UrlEncode("a&b=c") == "a%26b%3Dc");
    CHECK(cnk::UrlEncode("") == "");
}

TEST_CASE("UrlEncode 必须转义 base64 的 + / =（新式 API 的 params 全靠它）") {
    // params 是 base64 密文；若 '+' 未被转义，服务器会按 form-urlencoded 解成空格，
    // 密文即损坏。此断言防止将来的「简化」把这条路径改坏（模拟器同样按 + → 空格解码）。
    CHECK(cnk::UrlEncode("a+b/c=") == "a%2Bb%2Fc%3D");
    CHECK(cnk::UrlEncode("+/+/") == "%2B%2F%2B%2F");
}

TEST_CASE("UrlEncode 正确处理中文（按 UTF-8 字节）") {
    CHECK(cnk::UrlEncode("学") == "%E5%AD%A6");
}

TEST_CASE("SplitHostPort 对显式端口的 URL") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("http://10.0.0.1:801/eportal/", host, port, "80"));
    CHECK(host == "http://10.0.0.1");
    CHECK(port == "801");
}

TEST_CASE("SplitHostPort 对无端口的 URL 用 fallback") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("http://10.0.0.1/", host, port, "801"));
    CHECK(host == "http://10.0.0.1");
    CHECK(port == "801");

    std::string host2, port2;
    REQUIRE(cnk::SplitHostPort("http://10.0.0.1/", host2, port2, "80"));
    CHECK(port2 == "80");
}

TEST_CASE("SplitHostPort 保留 scheme") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("https://a.example.com/x?y=1", host, port, "443"));
    CHECK(host == "https://a.example.com");
    CHECK(port == "443");
}

TEST_CASE("SplitHostPort 对畸形 URL 返回 false") {
    std::string host, port;
    CHECK_FALSE(cnk::SplitHostPort("", host, port, "80"));
}

TEST_CASE("SplitHostPort 处理 IPv6 字面量与 userinfo") {
    std::string host, port;
    REQUIRE(cnk::SplitHostPort("http://[::1]:801/x", host, port, "80"));
    CHECK(host == "http://[::1]");
    CHECK(port == "801");

    std::string host2, port2;
    REQUIRE(cnk::SplitHostPort("http://[::1]/x", host2, port2, "80"));
    CHECK(host2 == "http://[::1]");
    CHECK(port2 == "80");

    std::string host3, port3;
    REQUIRE(cnk::SplitHostPort("http://user@10.0.0.1:801/", host3, port3, "80"));
    CHECK(host3 == "http://10.0.0.1");
    CHECK(port3 == "801");

    // 未闭合的 '[' 是畸形，不得静默返回错误结果
    std::string host4, port4;
    CHECK_FALSE(cnk::SplitHostPort("http://[::1", host4, port4, "80"));
}

TEST_CASE("ResolveUrl 处理绝对、根相对与路径相对") {
    CHECK(cnk::ResolveUrl("http://a/b", "http://x/") == "http://a/b");
    CHECK(cnk::ResolveUrl("/doLogin", "http://10.0.0.1/portal/")
          == "http://10.0.0.1/doLogin");
    CHECK(cnk::ResolveUrl("doLogin", "http://10.0.0.1/portal/")
          == "http://10.0.0.1/doLogin");
    CHECK(cnk::ResolveUrl("", "http://10.0.0.1/login")
          == "http://10.0.0.1/login");
}

TEST_CASE("ResolveUrl 处理大小写 scheme 与协议相对地址") {
    // Location 头的写法不受 RFC 约束：scheme 大小写不敏感、可用协议相对形式
    CHECK(cnk::ResolveUrl("HTTP://a/b", "http://x/") == "HTTP://a/b");
    CHECK(cnk::ResolveUrl("Https://a/b", "http://x/") == "Https://a/b");
    CHECK(cnk::ResolveUrl("//evil.com/x", "http://127.0.0.1:9/ncsi.txt")
          == "http://evil.com/x");
    // 协议相对沿用 base 的 scheme
    CHECK(cnk::ResolveUrl("//h/x", "https://a.example/") == "https://h/x");
}
