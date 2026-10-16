#include "doctest.h"
#include "core/PortalPage.h"
#include "core/Probe.h"
#include "auth/Detector.h"
#include "sim/SimServer.h"

namespace {

// 直接构造「劫持探测结果」，把 ResolvePortalPage 的行为独立于真实探测来测：
// 真实探测（ProbeOnce）的判定已有 probe_test 覆盖，本文件聚焦「识别输入的选择」。
cnk::ProbeResult HijackedWithLocation(const std::string& location) {
    cnk::ProbeResult pr;
    pr.state = cnk::NetState::PortalDetected;
    pr.message = "被重定向到 " + location;
    pr.response.status = 302;
    pr.response.location = location;
    return pr;
}

}  // namespace

TEST_CASE("抓取成功：302 无指纹时以跳转目标页作为识别输入") {
    // 真实劫持形态（2026-10-16 本机实测）：302 无 Server 头、空正文，
    // 指纹只在跳转目标页（a79.htm）。识别必须抓取目标页，否则退化成 unknown。
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    auto pr = HijackedWithLocation("/a79.htm");  // 相对 Location，须按探测地址 resolve
    auto pp = cnk::ResolvePortalPage(pr, srv.BaseUrl() + "/sim_probe");

    CHECK(pp.fetched);
    CHECK(pp.fetch_error.empty());
    CHECK(pp.portal_url == srv.BaseUrl() + "/a79.htm");
    // 302 本身无指纹，识别结果只能来自抓取到的页面响应
    CHECK(pr.response.server.empty());
    CHECK(cnk::DetectVendor(pp.response) == cnk::Vendor::Drcom);
    CHECK(pp.response.server.find("DrcomServer") != std::string::npos);

    srv.Stop();
}

TEST_CASE("抓取失败：退回探测响应并记录原因，不阻断链路") {
    auto pr = HijackedWithLocation("http://127.0.0.1:1/nowhere");
    auto pp = cnk::ResolvePortalPage(pr, "http://127.0.0.1:9/probe");

    CHECK_FALSE(pp.fetched);
    CHECK_FALSE(pp.fetch_error.empty());
    CHECK(pp.portal_url == "http://127.0.0.1:1/nowhere");
    CHECK(pp.response.status == 302);  // 退回原响应
    // 无指纹的裸 302 退化为旧行为（unknown），但不得崩溃、不得阻断调用方
    CHECK(cnk::DetectVendor(pp.response) == cnk::Vendor::Unknown);
}

TEST_CASE("200 篡改页（无重定向）：不抓取，探测响应本身即识别输入") {
    cnk::ProbeResult pr;
    pr.state = cnk::NetState::PortalDetected;
    pr.response.status = 200;
    pr.response.body = "<html><!--Dr.COMWebLoginID_1.htm--></html>";

    auto pp = cnk::ResolvePortalPage(pr, "http://portal.example/probe");

    CHECK_FALSE(pp.fetched);
    CHECK(pp.fetch_error.empty());
    CHECK(pp.portal_url.empty());
    CHECK(cnk::DetectVendor(pp.response) == cnk::Vendor::Drcom);
}

TEST_CASE("非劫持态：不做抓取，探测响应原样保留") {
    cnk::SimServer srv;  // 真起了服务器仍未抓取，证明「不抓取」不是因为无处可抓
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::ProbeResult pr;
    pr.state = cnk::NetState::Online;
    pr.response.status = 200;
    pr.response.body = "Microsoft NCSI";

    auto pp = cnk::ResolvePortalPage(pr, srv.BaseUrl() + "/probe");

    CHECK_FALSE(pp.fetched);
    CHECK(pp.fetch_error.empty());
    CHECK(pp.portal_url.empty());
    CHECK(pp.response.body == "Microsoft NCSI");

    srv.Stop();
}
