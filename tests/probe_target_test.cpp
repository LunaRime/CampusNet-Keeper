#include "doctest.h"
#include "core/ProbeTarget.h"
#include <algorithm>

// 与实测环境一致的 visit_blacklist
static const char* kPage =
    "<script>visit_blacklist=[\"1.1.1.1\",\"www.msftncsi.com\",\"land.xiaomi.net\","
    "\"detectportal.firefox.com\",\"www.apple.com\",\"generate_204\",\"ncsi.txt\","
    "\"success.txt\"];</script>";

TEST_CASE("解析 visit_blacklist") {
    std::vector<std::string> bl;
    REQUIRE(cnk::ParseVisitBlacklist(kPage, bl));
    CHECK(bl.size() == 8);
    CHECK(std::find(bl.begin(), bl.end(), "www.msftncsi.com") != bl.end());
}

TEST_CASE("优先选中 msftncsi 而非黑名单外地址") {
    std::vector<std::string> bl;
    REQUIRE(cnk::ParseVisitBlacklist(kPage, bl));
    cnk::ProbeTarget t;
    REQUIRE(cnk::SelectProbeTarget(bl, t));
    CHECK(t.url.find("www.msftncsi.com") != std::string::npos);
    CHECK(t.expect == "Microsoft NCSI");
    // 关键：绝不能选出 connect.rom.miui.com 之类不在清单内的地址
    CHECK(t.url.find("connect.rom.miui.com") == std::string::npos);
}

TEST_CASE("无 msftncsi 时回落到 firefox") {
    std::vector<std::string> bl = {"detectportal.firefox.com", "ncsi.txt"};
    cnk::ProbeTarget t;
    REQUIRE(cnk::SelectProbeTarget(bl, t));
    CHECK(t.url.find("detectportal.firefox.com") != std::string::npos);
    CHECK(t.expect == "success");
}

TEST_CASE("清单为空时返回 false") {
    cnk::ProbeTarget t;
    CHECK_FALSE(cnk::SelectProbeTarget({}, t));
}

TEST_CASE("无 visit_blacklist 时解析返回 false") {
    std::vector<std::string> bl;
    CHECK_FALSE(cnk::ParseVisitBlacklist("<html>none</html>", bl));
}

TEST_CASE("DefaultProbeTarget 返回内置默认值") {
    auto t = cnk::DefaultProbeTarget();
    CHECK(t.url.find("www.msftncsi.com") != std::string::npos);
}
