#include "doctest.h"
#include "core/Probe.h"
#include "sim/SimServer.h"

namespace {
cnk::Settings MakeSettings(const std::string& url, const std::string& expect) {
    cnk::Settings s;
    s.probe_url = url;
    s.probe_expect = expect;
    s.probe_timeout_seconds = 2;
    return s;
}
}

TEST_CASE("登录后复探：轮询到在线即停，且有尝试次数上界") {
    // 2026-10-16 真机实测：登录接口已返回成功，但紧跟着的单次复探得到「仍不在线」
    //（AC 数据面放行滞后数秒），稍后探测才是 Online。复探必须轮询，不能探一次就下结论。
    int calls = 0;
    auto flaky = [&calls] {
        ++calls;
        cnk::ProbeResult r;
        r.state = calls < 3 ? cnk::NetState::PortalDetected : cnk::NetState::Online;
        return r;
    };
    auto r = cnk::ProbeWithRetry(flaky, 5, 0);
    CHECK(r.state == cnk::NetState::Online);
    CHECK(calls == 3);  // 第 3 次成功即停，不浪费探测

    // 一直不在线时必须收敛：次数用尽后返回最后一次结果（含 message，便于如实报告原因）
    calls = 0;
    auto always_hijacked = [&calls] {
        ++calls;
        cnk::ProbeResult r;
        r.state = cnk::NetState::PortalDetected;
        r.message = "被重定向到 http://portal/a79.htm";
        return r;
    };
    auto r2 = cnk::ProbeWithRetry(always_hijacked, 4, 0);
    CHECK(r2.state == cnk::NetState::PortalDetected);
    CHECK(calls == 4);
    CHECK(r2.message.find("重定向") != std::string::npos);

    // 次数参数的下界保护：给 0 也至少探一次
    calls = 0;
    auto once = [&calls] { ++calls; return cnk::ProbeResult{}; };
    (void)cnk::ProbeWithRetry(once, 0, 0);
    CHECK(calls == 1);
}

TEST_CASE("返回 200 且正文匹配时判定为在线") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Online);
    srv.Stop();
}

TEST_CASE("204 判定为在线") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/generate_204", "");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Online);
    srv.Stop();
}

TEST_CASE("含预期标识但正文超长时判为劫持（长度启发式）") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/big_ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("404 判定为 Portal 劫持（保守处理）") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/nonexistent", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("正文不匹配时判定为 Portal 劫持") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("连接失败时判定为离线") {
    auto s = MakeSettings("http://127.0.0.1:1/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Offline);
    CHECK(r.message.size() > 0);
}

TEST_CASE("超时判定为离线") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 2;
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    s.probe_timeout_seconds = 1;
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::Offline);
    srv.Stop();
}

TEST_CASE("未配置预期标识时 200 判为劫持（无法确认内容）") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto s = MakeSettings(srv.BaseUrl() + "/", "");
    auto r = cnk::ProbeOnce(s);
    // expect 为空时任何 200 都算劫持（因为没有可匹配的标识）
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("绝对地址的异主机 Location 判定为劫持") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 3;                                   // 302
    o.redirect_location = "http://portal.example.com/login";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    srv.Stop();
}

TEST_CASE("相对路径的 Location 解析后判在线，绝不能是 Offline") {
    // 真实 Portal 常用相对跳转。若未先解析成绝对地址，HostOf 会返回空，
    // 漏判劫持/在线，还会导致永不触发登录、每轮空转重探。
    // 此处的相对跳转经 ResolveUrl 解析后与探测目标同主机 → 判在线。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 3;
    o.redirect_location = "/portal/login";             // 相对路径
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state != cnk::NetState::Offline);          // 关键：绝不能是 Offline
    CHECK(r.state == cnk::NetState::Online);           // 解析后主机与探测目标相同，判在线
    srv.Stop();
}

TEST_CASE("绝对 Location 同 IP 不同端口按异主机判劫持") {
    // HostOf 比较包含端口：Location 指向 127.0.0.1:1，而探测目标是
    // 127.0.0.1:<srv 端口>，端口不同 → 判劫持（绝非 Offline）。
    // 真实场景中 Portal 与本机同网段不同端口时依赖该区分。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.fail_mode = 3;
    o.redirect_location = "http://127.0.0.1:1/other";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
    auto r = cnk::ProbeOnce(s);
    CHECK(r.state == cnk::NetState::PortalDetected);
    CHECK(r.state != cnk::NetState::Offline);
    srv.Stop();
}

TEST_CASE("协议相对与大写 scheme 的 Location 判为劫持") {
    // Location 头可能用 //host/path（协议相对）或大写 scheme 写的绝对地址——
    // 两种情况都必须识别为异主机，否则劫持会被静默误判为在线
    {
        cnk::SimServer srv;
        cnk::SimOptions o;
        o.fail_mode = 3;
        o.redirect_location = "//evil.example/x";
        std::string err;
        REQUIRE(srv.Start(o, err));
        auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
        auto r = cnk::ProbeOnce(s);
        CHECK(r.state == cnk::NetState::PortalDetected);
        srv.Stop();
    }
    {
        cnk::SimServer srv;
        cnk::SimOptions o;
        o.fail_mode = 3;
        o.redirect_location = "HTTP://EVIL.EXAMPLE/x";
        std::string err;
        REQUIRE(srv.Start(o, err));
        auto s = MakeSettings(srv.BaseUrl() + "/ncsi.txt", "Microsoft NCSI");
        auto r = cnk::ProbeOnce(s);
        CHECK(r.state == cnk::NetState::PortalDetected);
        srv.Stop();
    }
}
