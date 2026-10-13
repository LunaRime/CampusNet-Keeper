#include "doctest.h"
#include "core/Url.h"

TEST_CASE("UrlEncode 编码保留字符") {
    CHECK(cnk::UrlEncode("abc123-_.~") == "abc123-_.~");
    CHECK(cnk::UrlEncode("a b") == "a%20b");
    CHECK(cnk::UrlEncode("a@b") == "a%40b");
    CHECK(cnk::UrlEncode("a&b=c") == "a%26b%3Dc");
    CHECK(cnk::UrlEncode("") == "");
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
