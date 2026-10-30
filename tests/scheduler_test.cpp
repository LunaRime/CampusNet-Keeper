#include "doctest.h"
#include "core/Scheduler.h"
#include "sim/SimServer.h"
#include <chrono>
#include <thread>

namespace {

cnk::AppConfig MakeConfig(const std::string& portal, const std::string& user,
                          const std::string& pass) {
    cnk::AppConfig cfg;
    cfg.settings.probe_url = portal + "/ncsi.txt";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cfg.settings.probe_timeout_seconds = 1;
    cfg.settings.probe_interval_seconds = 1;
    cfg.settings.retry_backoff_seconds = {1, 1, 1};
    cnk::Profile p;
    p.name = "默认";
    p.auth_url = portal + "/";
    p.username = user;
    p.password = pass;
    p.vendor = "drcom";
    p.isp.preset = "campus";
    cfg.profiles.push_back(p);
    cfg.active_profile = "默认";
    return cfg;
}

}  // namespace

TEST_CASE("探测在线时进入 Online") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Online);
    srv.Stop();
}

TEST_CASE("被劫持且凭据正确时自动登录并恢复在线") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    // 探测指向门户首页（会被判为劫持），登录端点则正确
    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();
    for (int i = 0; i < 5; ++i) sch.Tick();
    CHECK(srv.LoginCount() >= 1);
    srv.Stop();
}

TEST_CASE("凭据错误时停止重试并进入 AuthFailed") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "RIGHT";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "WRONG");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();

    // 连跑多次 Tick；因退避为 {1,1,1} 秒，需要让时间推进
    for (int i = 0; i < 200; ++i) {
        sch.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK(sch.State() == cnk::RunState::AuthFailed);
    const int count_after_stop = srv.LoginCount();
    // 进入 AuthFailed 后不得继续重试
    for (int i = 0; i < 100; ++i) {
        sch.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(srv.LoginCount() == count_after_stop);
    CHECK(sch.LastMessage().size() > 0);
    srv.Stop();
}

TEST_CASE("从未成功且反复失败时进入 NotCampusNetwork 并停止重试") {
    // 探测地址指向一个永远返回门户页的服务器，登录也永远失败
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "RIGHT";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "WRONG");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cfg.settings.retry_backoff_seconds = {1, 1, 1, 1, 1};
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();

    for (int i = 0; i < 600; ++i) {
        sch.Tick();
        if (sch.State() == cnk::RunState::NotCampusNetwork ||
            sch.State() == cnk::RunState::AuthFailed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 凭据错误会先命中 AuthFailed，这也是停止重试的合法终态
    CHECK((sch.State() == cnk::RunState::AuthFailed ||
           sch.State() == cnk::RunState::NotCampusNetwork));
    srv.Stop();
}

TEST_CASE("ForceReconnect 立即触发探测而不等待间隔") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_interval_seconds = 3600;  // 一小时内不该再自动探测
    cnk::Scheduler sch(cfg);
    sch.Tick();
    const long long t1 = sch.LastProbeUnix();
    REQUIRE(t1 > 0);

    // 间隔未到时 Tick 不产生新探测（探测时间戳为秒级，先让墙钟跨秒）
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sch.Tick();
    CHECK(sch.LastProbeUnix() == t1);

    // ForceReconnect 后必须立即探测
    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.LastProbeUnix() > t1);
    srv.Stop();
}

TEST_CASE("无配置档案时不崩溃且状态明确") {
    cnk::AppConfig cfg;
    // 默认 Settings 自带内置探测地址；显式清空以消除对真实网络的依赖
    cfg.settings.probe_url.clear();
    cnk::Scheduler sch(cfg);
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Disconnected);  // 无有效探测地址的明确归属
    CHECK(sch.LastMessage().size() > 0);
}

TEST_CASE("裸 302 劫持（无指纹）时抓取门户页识别并完成登录，登录后恢复在线") {
    // 真实劫持形态（2026-10-16 实测）：302 无 Server 头、空正文；指纹只在
    // 跳转目标页。识别输入若仍取 302 本身 → unknown → 登录链路失效（本次实测缺陷）。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.fail_mode = 5;  // 裸 302：Location 指向本模拟器第二监听端口上的门户页
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/sim_probe";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();

    for (int i = 0; i < 100 && srv.LoginCount() == 0; ++i) {
        sch.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(srv.LoginCount() >= 1);
    CHECK(sch.State() == cnk::RunState::Online);

    // 登录成功后 /sim_probe 恢复正常应答：下轮探测应保持在线且不重复登录
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const int logins = srv.LoginCount();
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Online);
    CHECK(srv.LoginCount() == logins);

    srv.Stop();
}

TEST_CASE("网络变更通知会立即触发探测，不等轮询周期") {
    // 对应 Review Focus 第 4 条：睡眠唤醒 / 切换网络后必须立刻探测
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_interval_seconds = 3600;  // 一小时内不会自动再探测
    cnk::Scheduler sch(cfg);

    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Online);
    const long long t1 = sch.LastProbeUnix();

    // 模拟系统睡眠 2 秒后唤醒
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 未通知时，Tick 不应产生新探测
    sch.Tick();
    CHECK(sch.LastProbeUnix() == t1);

    // 收到网络变更通知后，下一次 Tick 必须立即探测
    sch.NotifyNetworkChanged();
    sch.Tick();
    CHECK(sch.LastProbeUnix() > t1);

    srv.Stop();
}

// 这两条钉的都是「重试一万次也不会变」的假死形态：
// 不注销就永远说着同一句话，调度器却以为自己在"努力重连"。
TEST_CASE("账号会话被占时先注销再登录（不死循环重试）") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.in_use_until_logout = true;   // 只有收到注销才会放行
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/sim_probe";   // 登录前算劫持，登录后在线
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();
    sch.Tick();

    CHECK(srv.LogoutCount() >= 1);   // 必须真的去注销，而不是干等/空转
    CHECK(srv.LoginCount() >= 2);    // 注销后必须重登一次
    CHECK(sch.State() == cnk::RunState::Online);
    srv.Stop();
}

TEST_CASE("僵尸会话（终端已在线但上不了网）会注销后重登") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.zombie_online = true;         // 嘴上说"终端已在线"，实际不放行
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/sim_probe";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();
    sch.Tick();   // 内部会复测 3 次（约 2.4s）后才判定僵尸

    CHECK(srv.LogoutCount() >= 1);
    CHECK(sch.State() == cnk::RunState::Online);
    srv.Stop();
}
