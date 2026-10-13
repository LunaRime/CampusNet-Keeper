#include "doctest.h"
#include "core/ProbeTarget.h"
#include <algorithm>
#include <fstream>

// 与实测环境一致的 visit_blacklist
static const char* kPage =
    "<script>visit_blacklist=[\"1.1.1.1\",\"www.msftncsi.com\",\"land.xiaomi.net\","
    "\"detectportal.firefox.com\",\"www.apple.com\",\"generate_204\",\"ncsi.txt\","
    "\"success.txt\"];</script>";

TEST_CASE("解析实测 loadConfig 的 JSONP 黑名单") {
    // 真实响应是 dr1003({"visit_blacklist":[...]}) 的 JSONP：键值之间是冒号。
    // 早期实现只认 '='，对真实数据返回 false（审阅实测），此用例是回归保护。
    std::ifstream in(std::string(CNK_EVIDENCE_DIR) + "/loadconfig_185_801.json",
                     std::ios::binary);
    REQUIRE(in);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::vector<std::string> bl;
    REQUIRE(cnk::ParseVisitBlacklist(body, bl));
    CHECK(bl.size() == 15);
    cnk::ProbeTarget t;
    REQUIRE(cnk::SelectProbeTarget(bl, t));
    CHECK(t.url == "http://www.msftncsi.com/ncsi.txt");
    CHECK(t.expect == "Microsoft NCSI");
    CHECK(t.url.find("connect.rom.miui.com") == std::string::npos);
}

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

TEST_CASE("清单仅含 ncsi.txt 时仍选出 msftncsi 地址") {
    // spec §6.1：msftncsi 系的域名或路径条目都指向 msftncsi 探测地址
    std::vector<std::string> bl = {"ncsi.txt"};
    cnk::ProbeTarget t;
    REQUIRE(cnk::SelectProbeTarget(bl, t));
    CHECK(t.url.find("www.msftncsi.com") != std::string::npos);
}

TEST_CASE("选择失败时清空输出，不残留上次结果") {
    cnk::ProbeTarget t{"http://stale/", "stale"};
    CHECK_FALSE(cnk::SelectProbeTarget({}, t));
    CHECK(t.url.empty());
    CHECK(t.expect.empty());
}

TEST_CASE("仿冒域名不误命中候选") {
    // evil-msftncsi.com 不是 msftncsi 域名（边界匹配不命中 '-' 前缀）
    std::vector<std::string> bl = {"evil-msftncsi.com", "notsuccess.txt"};
    cnk::ProbeTarget t;
    CHECK_FALSE(cnk::SelectProbeTarget(bl, t));
}

TEST_CASE("无 visit_blacklist 时解析返回 false") {
    std::vector<std::string> bl;
    CHECK_FALSE(cnk::ParseVisitBlacklist("<html>none</html>", bl));
}

TEST_CASE("DefaultProbeTarget 返回内置默认值") {
    auto t = cnk::DefaultProbeTarget();
    CHECK(t.url.find("www.msftncsi.com") != std::string::npos);
}
