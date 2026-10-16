#include "doctest.h"
#include "core/LocalIp.h"

TEST_CASE("LocalIpFor 对回环地址返回 127.0.0.1") {
    // UDP connect + getsockname：不依赖 DNS，结果确定
    CHECK(cnk::LocalIpFor("127.0.0.1", 80) == "127.0.0.1");
    CHECK(cnk::LocalIpFor("127.0.0.1", 65535) == "127.0.0.1");
}

TEST_CASE("LocalIpFor 对非法入参返回空串而不抛异常") {
    CHECK(cnk::LocalIpFor("", 80).empty());
    CHECK(cnk::LocalIpFor("127.0.0.1", 0).empty());
    CHECK(cnk::LocalIpFor("127.0.0.1", -1).empty());
    CHECK(cnk::LocalIpFor("127.0.0.1", 70000).empty());
}

TEST_CASE("LocalIpForUrl 按 scheme 缺省端口解析认证网址") {
    CHECK(cnk::LocalIpForUrl("http://127.0.0.1/") == "127.0.0.1");
    CHECK(cnk::LocalIpForUrl("http://127.0.0.1:18080/eportal/") == "127.0.0.1");
    CHECK(cnk::LocalIpForUrl("").empty());
    CHECK(cnk::LocalIpForUrl("not-a-url").empty());
}

TEST_CASE("ResolveClientIp 优先级：上下文 → 现算 → 默认路由 → 0.0.0.0 兜底") {
    // 上层给了就用上层的
    CHECK(cnk::ResolveClientIp("10.1.2.3", "http://127.0.0.1/") == "10.1.2.3");
    // 上层为空（M1 现状）时按认证网址现算
    CHECK(cnk::ResolveClientIp("", "http://127.0.0.1/") == "127.0.0.1");
    // 网址不可解析：先退「默认路由的源地址」（不依赖 DNS），最后才是 0.0.0.0。
    // 无论走到哪一步都不返回空串——空串会让调用方提交空字段。
    const std::string fb = cnk::ResolveClientIp("", "");
    CHECK_FALSE(fb.empty());
    CHECK(fb != "0.0.0.0");  // 本机有默认路由，应能算出真实源地址
}
