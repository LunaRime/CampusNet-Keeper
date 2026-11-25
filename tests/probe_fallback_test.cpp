#include "doctest.h"
#include "core/ProbePlan.h"
#include "core/Scheduler.h"
#include "ipc/DaemonCommands.h"
#include "sim/SimServer.h"
#include "store/Config.h"
#include "json.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace {

// ⚠ 这里刻意用**同一个模拟器**的主备地址：
//   · "备用必须异域"是 PickFallbackTarget() 的职责，由下面的纯逻辑用例覆盖；
//   · 本条文件要验的是"主不可用时怎么走"（是否回退、耗时、日志、状态）。
//   SimServer 只监听 IPv4，用 localhost 会因解析到 ::1 而连不上，那是测试环境噪声。
std::string AltUrl(const std::string& base) { return base; }

cnk::AppConfig MakeCfg(const std::string& base, const std::string& probe_path) {
    cnk::AppConfig cfg;
    cfg.settings.probe_url = base + probe_path;
    cfg.settings.probe_expect = "";
    cfg.settings.probe_timeout_seconds = 2;
    cfg.settings.probe_interval_seconds = 3600;
    cnk::Profile p;
    p.name = "默认";
    p.auth_url = base + "/";
    p.username = "u";
    p.password = "p";
    p.vendor = "drcom";
    p.isp.preset = "campus";
    cfg.profiles.push_back(p);
    cfg.active_profile = "默认";
    return cfg;
}

}  // namespace

TEST_CASE("备用地址本身可达（否则后面的回退断言会失真）") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Settings s;
    s.probe_timeout_seconds = 2;
    const auto po = cnk::ProbeSingle(s, AltUrl(srv.BaseUrl()) + "/generate_204", "");
    INFO("备用地址探测结果：" << po.result.message << "（status="
                              << po.result.response.status << "）");
    CHECK(po.result.state == cnk::NetState::Online);
    srv.Stop();
}

TEST_CASE("主地址 404、备用 204：同一轮切备用并判在线") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Settings s;
    s.probe_url = srv.BaseUrl() + "/no-such-path";   // 明确不是门户的 404
    s.probe_expect = "";
    s.probe_timeout_seconds = 3;

    const cnk::ProbeTarget fb{AltUrl(srv.BaseUrl()) + "/generate_204", ""};
    cnk::FallbackState st;
    const auto po = cnk::ProbeWithFallbackUsing(s, st, fb);

    CHECK(po.used_fallback);
    CHECK(po.result.state == cnk::NetState::Online);
    REQUIRE(po.attempts.size() == 2);
    CHECK_FALSE(po.attempts[0].fallback);
    CHECK(po.attempts[0].result.response.status == 404);
    CHECK(po.attempts[1].fallback);
    CHECK(po.attempts[1].result.response.status == 204);
    CHECK(po.total_ms < 3000);      // 整轮不超预算
    CHECK(st.sticky);               // 备用可用 → 暂时沿用
    // 只探测：配置根本没被写
    CHECK(s.probe_url.find("no-such-path") != std::string::npos);
    srv.Stop();
}

TEST_CASE("主地址 DNS 失败、备用 204：切备用") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Settings s;
    s.probe_url = "http://no-such-host.invalid/x";   // DNS 直接失败
    s.probe_expect = "";
    s.probe_timeout_seconds = 2;

    cnk::FallbackState st;
    const auto po = cnk::ProbeWithFallbackUsing(
        s, st, {AltUrl(srv.BaseUrl()) + "/generate_204", ""});

    CHECK(po.used_fallback);
    CHECK(po.result.state == cnk::NetState::Online);
    CHECK(po.attempts.size() == 2);
    srv.Stop();
}

TEST_CASE("主地址超时、备用 204：整轮耗时不是一个候选吃满两次超时") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Settings s;
    s.probe_url = "http://192.0.2.1/x";   // TEST-NET-1：不可路由
    s.probe_expect = "";
    s.probe_timeout_seconds = 1;

    cnk::FallbackState st;
    const auto po = cnk::ProbeWithFallbackUsing(
        s, st, {AltUrl(srv.BaseUrl()) + "/generate_204", ""});

    CHECK(po.used_fallback);
    CHECK(po.result.state == cnk::NetState::Online);
    // 主 1s + 备用（局域回环，毫秒级）。上界取宽松值：WinHTTP 的 connect/step
    // 阶段各有一次超时，主地址可能略超 1s；要抓的是"每个候选都吃满一整轮超时"
    // 那种串行行为（会 ≥ 2×timeout 并随候选数线性增长）。
    INFO("主 " << po.attempts[0].elapsed_ms << "ms，备用 "
               << (po.attempts.size() > 1 ? po.attempts[1].elapsed_ms : -1)
               << "ms，整轮 " << po.total_ms << "ms");
    REQUIRE(po.attempts.size() == 2);
    CHECK(po.attempts[1].elapsed_ms < 1000);   // 备用没被主地址的超时拖慢
    CHECK(po.total_ms < 4000);
    srv.Stop();
}

TEST_CASE("主地址给出明确门户证据时，不拿备用把它覆盖成在线") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));   // session_active=false：/sim_probe 返回门户页

    cnk::Settings s;
    s.probe_url = srv.BaseUrl() + "/sim_probe";    // 200 但不含预期标识 → 明确判门户
    s.probe_expect = "Microsoft NCSI";
    s.probe_timeout_seconds = 2;

    cnk::FallbackState st;
    const auto po = cnk::ProbeWithFallbackUsing(
        s, st, {AltUrl(srv.BaseUrl()) + "/generate_204", ""});

    CHECK(po.result.state == cnk::NetState::PortalDetected);
    CHECK_FALSE(po.used_fallback);
    CHECK(po.attempts.size() == 1);   // 根本没去碰备用
    CHECK_FALSE(st.sticky);
    srv.Stop();
}

TEST_CASE("所有探测目标都不可用时，错误信息要能区分出来") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Settings s;
    s.probe_url = srv.BaseUrl() + "/no-such-a";
    s.probe_expect = "";
    s.probe_timeout_seconds = 2;

    cnk::FallbackState st;
    const auto po = cnk::ProbeWithFallbackUsing(
        s, st, {AltUrl(srv.BaseUrl()) + "/no-such-b", ""});

    CHECK(po.all_failed);
    CHECK_FALSE(po.used_fallback);
    CHECK(po.result.state == cnk::NetState::Offline);
    CHECK(po.result.message.find("所有探测目标均不可用") != std::string::npos);
    CHECK(po.attempts.size() == 2);
    srv.Stop();
}

TEST_CASE("备用可用后暂时沿用，不再每轮先等主地址超时") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Settings s;
    s.probe_url = srv.BaseUrl() + "/no-such-path";
    s.probe_expect = "";
    s.probe_timeout_seconds = 2;
    const cnk::ProbeTarget fb{AltUrl(srv.BaseUrl()) + "/generate_204", ""};

    cnk::FallbackState st;
    const auto first = cnk::ProbeWithFallbackUsing(s, st, fb);
    REQUIRE(first.used_fallback);
    REQUIRE(st.sticky);

    // 第二轮：直接打备用，attempts 只有一条
    const auto second = cnk::ProbeWithFallbackUsing(s, st, fb);
    CHECK(second.used_fallback);
    REQUIRE(second.attempts.size() == 1);
    CHECK(second.attempts[0].fallback);
    CHECK(second.attempts[0].url.find("/generate_204") != std::string::npos);

    // 到期后（把到期时间拨到过去）必须回试主地址
    st.retry_primary_at = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto third = cnk::ProbeWithFallbackUsing(s, st, fb);
    REQUIRE(third.attempts.size() >= 1);
    CHECK_FALSE(third.attempts[0].fallback);   // 先试主
    CHECK(third.used_fallback);                // 主仍坏 → 又回备用
    srv.Stop();
}

TEST_CASE("内置备用必须与主地址异域（同域等于把同一份证据打两遍）") {
    const auto a = cnk::PickFallbackTarget("http://detectportal.firefox.com/success.txt");
    CHECK_FALSE(a.url.empty());
    CHECK(cnk::HostOf(a.url) != "detectportal.firefox.com");

    const auto b =
        cnk::PickFallbackTarget("http://connectivitycheck.platform.hicloud.com/generate_204");
    CHECK_FALSE(b.url.empty());
    CHECK(cnk::HostOf(b.url) != "connectivitycheck.platform.hicloud.com");

    // HostOf 归一化：默认端口与大小写不影响比较，异主机必须判异
    CHECK(cnk::HostOf("http://Example.COM/x") == "example.com");
    CHECK(cnk::HostOf("http://example.com:80/x") == "example.com");
    CHECK(cnk::HostOf("http://127.0.0.1:8080/x") != cnk::HostOf("http://localhost:8080/x"));
}

TEST_CASE("探测全部失败不触发登录，get_status 能看出走了备用") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Scheduler sch(MakeCfg(srv.BaseUrl(), "/no-such-path"));
    sch.SetFallbackOverride({AltUrl(srv.BaseUrl()) + "/generate_204", ""});
    sch.ForceReconnect();
    sch.Tick();

    // 仅仅探测失败/回退，绝不发起认证（2026-11-15 真机 24 次无效登录的教训）
    CHECK(srv.LoginCount() == 0);

    cnk::IpcRequest req;
    req.id = 1;
    req.cmd = "get_status";
    const nlohmann::json j = nlohmann::json::parse(
        cnk::HandleDaemonCommand(req, sch).data_json);
    CHECK(j["probe_fallback"] == true);
    CHECK(j["probe_url"].get<std::string>().find("/generate_204") != std::string::npos);
    srv.Stop();
}

TEST_CASE("测试探测地址命令：只探测、不认证、不改守护的回退状态") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    // 用临时配置，避免读写用户真实配置
    const auto conf = std::filesystem::temp_directory_path() / "cnk_test_probe_target.json";
    REQUIRE(cnk::ConfigSaveTo(conf.u8string(), MakeCfg(srv.BaseUrl(), "/no-such-path"), err));

    cnk::Scheduler sch(MakeCfg(srv.BaseUrl(), "/no-such-path"));
    sch.SetFallbackOverride({AltUrl(srv.BaseUrl()) + "/generate_204", ""});

    cnk::IpcRequest req;
    req.id = 2;
    req.cmd = "probe_target";
    req.args_json = "{}";
    const auto resp = cnk::HandleDaemonCommandAt(req, sch, conf.u8string());
    REQUIRE(resp.ok);

    const nlohmann::json j = nlohmann::json::parse(resp.data_json);
    CHECK(j.contains("url"));
    CHECK(j.contains("elapsed_ms"));
    CHECK(j.contains("state"));
    CHECK(j["used_fallback"] == true);   // 主 404 → 备用 204
    CHECK(j["state"] == "online");
    CHECK(j["elapsed_ms"].get<int>() >= 0);

    // 只探测：没有登录过
    CHECK(srv.LoginCount() == 0);

    std::error_code ec;
    std::filesystem::remove(conf, ec);
    srv.Stop();
}

TEST_CASE("指定 URL 测试时不自动回退（用户测的就是这个地址）") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    const auto conf = std::filesystem::temp_directory_path() / "cnk_test_probe_url.json";
    REQUIRE(cnk::ConfigSaveTo(conf.u8string(), MakeCfg(srv.BaseUrl(), "/no-such-path"), err));

    cnk::Scheduler sch(MakeCfg(srv.BaseUrl(), "/no-such-path"));
    sch.SetFallbackOverride({AltUrl(srv.BaseUrl()) + "/generate_204", ""});

    cnk::IpcRequest req;
    req.id = 3;
    req.cmd = "probe_target";
    req.args_json = std::string("{\"url\":\"") + srv.BaseUrl() + "/generate_204\"}";
    const auto resp = cnk::HandleDaemonCommandAt(req, sch, conf.u8string());
    REQUIRE(resp.ok);

    const nlohmann::json j = nlohmann::json::parse(resp.data_json);
    CHECK(j["state"] == "online");
    CHECK(j["used_fallback"] == false);   // 指定了地址就不该换
    CHECK(j["url"] == srv.BaseUrl() + "/generate_204");

    std::error_code ec;
    std::filesystem::remove(conf, ec);
    srv.Stop();
}
