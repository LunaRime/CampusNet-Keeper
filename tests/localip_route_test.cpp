#include "doctest.h"
#include "core/LocalIp.h"

#include <string>

// 注意：本文件只用「文档占位地址」（10.0.0.5 / 10.0.0.9 等已在 audit_redaction.py 的
// SAFE_QUADS 里登记）。**不要把真机当时的 IP/网关抄进来**——2026-10-26 就是顺手写了
// 真机地址，让 CI 的脱敏审计连挂两次。
//
// 只测「判读文案」这一层：它决定了日志里能不能区分
// 「压根没地址」「链路本地没拿到 DHCP」「有地址但网关不通」——2026-10-26 的事故
// 就是因为日志只有一句超时，事后分不清这三者。
// RouteForUrl 依赖本机真实网络，不做断言（那属于集成验证，见 --probe-now）。

TEST_CASE("DescribeRoute：没有到门户的路由") {
    cnk::LocalRoute r;  // ip/gateway 均为空
    const std::string s = cnk::DescribeRoute(r);
    CHECK(s.find("本机地址=无") != std::string::npos);
    CHECK(s.find("没有到门户的路由") != std::string::npos);
}

TEST_CASE("DescribeRoute：169.254 链路本地要说明是没拿到 DHCP") {
    cnk::LocalRoute r;
    r.ip = "169.254.148.26";
    const std::string s = cnk::DescribeRoute(r);
    CHECK(s.find("169.254.148.26") != std::string::npos);
    CHECK(s.find("链路本地") != std::string::npos);
    CHECK(s.find("DHCP") != std::string::npos);
    // 链路本地地址没有默认网关，文案必须说清是"直连"而不是漏掉网关
    CHECK(s.find("网关=直连") != std::string::npos);
}

TEST_CASE("DescribeRoute：正常地址带网关") {
    cnk::LocalRoute r;
    r.ip = "10.0.0.5";
    r.gateway = "10.0.0.9";
    const std::string s = cnk::DescribeRoute(r);
    CHECK(s.find("本机地址=10.0.0.5") != std::string::npos);
    CHECK(s.find("网关=10.0.0.9") != std::string::npos);
    CHECK(s.find("链路本地") == std::string::npos);
}

TEST_CASE("DescribeRoute：有地址但没有下一跳") {
    cnk::LocalRoute r;
    r.ip = "192.0.2.10";
    const std::string s = cnk::DescribeRoute(r);
    CHECK(s.find("本机地址=192.0.2.10") != std::string::npos);
    CHECK(s.find("网关=直连") != std::string::npos);
}

TEST_CASE("DescribeRoute：回环地址单独标注") {
    cnk::LocalRoute r;
    r.ip = "127.0.0.1";
    const std::string s = cnk::DescribeRoute(r);
    CHECK(s.find("回环") != std::string::npos);
}
