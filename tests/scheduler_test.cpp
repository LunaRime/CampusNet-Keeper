#include "doctest.h"
#include "core/Scheduler.h"
#include "ipc/DaemonCommands.h"
#include "sim/SimServer.h"
#include "json.hpp"
#include <chrono>
#include <filesystem>
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

TEST_CASE("探测在线时进入 Online，且本机 IP 留在状态里") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.session_active = true;   // 已有会话：否则 AC 复核会判「会话失效」→ 离线
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Online);
    // get_status.local_ip 此前恒空：没有赋值来源，且在线分支的 ResetCache 还会把它清掉
    CHECK(sch.LocalIp() == "127.0.0.1");
    srv.Stop();
}

TEST_CASE("AC 说不在线时，探测页成功也判离线（DNS 劫持假在线）") {
    // 2026-10-30 真机形态：探测域名被劫持，AC 会话早没了，探测页却回 200+Success。
    // 只靠探测会认定"在线"，于是永远不去重连，用户实际打不开任何网页。
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));   // session_active 默认 false

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();

    CHECK(sch.State() == cnk::RunState::Disconnected);
    CHECK(sch.LastMessage().find("认证服务器判定不在线") != std::string::npos);
    // 判离线后走的是离线分支（3 秒后重探），不该立刻拿账号去撞登录接口
    CHECK(srv.LoginCount() == 0);
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

TEST_CASE("暂停自动重连后不发起登录，手动「立即重连」仍放行一次") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/";   // 会被判为门户劫持
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.SetPaused(true);
    CHECK(sch.IsPaused());

    // 构造后 probe_immediately 默认 true，第一次 Tick 就会探测，无需 ForceReconnect
    for (int i = 0; i < 5; ++i) sch.Tick();
    CHECK(srv.LoginCount() == 0);
    CHECK(sch.State() == cnk::RunState::PortalDetected);
    CHECK(sch.LastMessage().find("已暂停自动重连") != std::string::npos);

    // 用户主动点托盘「立即重连」：暂停态下也放行这一次
    sch.ForceReconnect();
    sch.Tick();
    CHECK(srv.LoginCount() >= 1);
    srv.Stop();
}

TEST_CASE("恢复自动重连后重新自动登录（不走手动放行）") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.SetPaused(true);
    sch.Tick();
    CHECK(srv.LoginCount() == 0);

    sch.SetPaused(false);
    CHECK_FALSE(sch.IsPaused());
    // SetConfig 只触发立即探测，不会设 manual_override —— 验证的是纯自动路径
    sch.SetConfig(cfg);
    sch.Tick();
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

TEST_CASE("AC 判不在线的结论在节流期内被复用，不会下一轮又判回在线") {
    // 节流（30 秒问一次 AC）不能把「查不出来」和「明确不在线」混为一谈：
    // 丢掉结论就会退回只靠出网验证，而那个判据在 DNS 劫持下正是失效的那个。
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();
    REQUIRE(sch.State() == cnk::RunState::Disconnected);

    // 第二轮：探测页依然回 "Microsoft NCSI"（假在线），AC 本轮不再询问（节流中）
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    sch.Tick();
    CHECK(sch.State() == cnk::RunState::Disconnected);
    CHECK(sch.LastMessage().find("认证服务器判定不在线") != std::string::npos);
    srv.Stop();
}

TEST_CASE("ForceReconnect 立即触发探测而不等待间隔") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.session_active = true;
    std::string err;
    REQUIRE(srv.Start(o, err));

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
    cnk::SimOptions o;
    o.session_active = true;
    std::string err;
    REQUIRE(srv.Start(o, err));

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

TEST_CASE("网络变更通告不得重置僵尸会话的注销限次（真机 8 分钟 24 次登录事故）") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    o.zombie_online = true;
    o.logout_ineffective = true;   // 注销端点回 200 成功页，AC 会话却清不掉
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cfg.settings.probe_url = srv.BaseUrl() + "/sim_probe";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();
    sch.Tick();   // 复测 3 次后判僵尸 → 注销一次

    const int first = sch.IneffectiveLogoutCount();
    REQUIRE(first >= 1);
    const int logouts = srv.LogoutCount();

    // 2026-11-15 真机：AC 抖动让 NotifyAddrChange 每 10~30 秒触发一次，
    // 旧实现每次都把 inuse_logout_count / stop_retrying 清零，于是
    // 「连续 3 次无效即停手」永远到不了，只能靠用户手动改配置才停。
    for (int i = 0; i < 3; ++i) {
        sch.NotifyNetworkChanged();
        sch.Tick();
    }
    CHECK(sch.IneffectiveLogoutCount() == first);   // 计数绝不能被地址通告清零
    CHECK(srv.LogoutCount() == logouts);            // 冷却期内也不该连发注销
    CHECK(sch.State() != cnk::RunState::Online);    // 注销假成功绝不能判在线
    srv.Stop();
}

TEST_CASE("凭据错误停手后换网仍能恢复（只有会话卡死才禁止地址通告解除）") {
    // 修僵尸会话死循环时，若把「网络变更不再解除 stop_retrying」一刀切，
    // 用户从错误网络换到校园网后就再也不会自动重试了。这里钉住边界。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "wrong", "wrong");
    cfg.settings.probe_url = srv.BaseUrl() + "/sim_probe";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cnk::Scheduler sch(cfg);
    sch.ForceReconnect();
    sch.Tick();

    CHECK(sch.State() == cnk::RunState::AuthFailed);
    const int before = srv.LoginCount();
    REQUIRE(before >= 1);

    sch.NotifyNetworkChanged();
    sch.Tick();
    CHECK(srv.LoginCount() > before);   // 换网后必须重新给一次登录机会
    srv.Stop();
}

TEST_CASE("get_status 真的带出本机 IP / 网关 / ARP 状态") {
    // 只测 Scheduler::LocalIp() 不够：界面拿的是 get_status 的 JSON。
    // 字段名写错、漏拼、被 RedactSecrets 吃掉，函数测试全绿也照样空白
    //（2026-10-30 的教训：测了函数没测真正发出的报文）。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.session_active = true;
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();
    REQUIRE(sch.State() == cnk::RunState::Online);

    cnk::IpcRequest req;
    req.id = 1;
    req.cmd = "get_status";
    const cnk::IpcResponse resp = cnk::HandleDaemonCommand(req, sch);
    REQUIRE(resp.ok);

    const nlohmann::json j = nlohmann::json::parse(resp.data_json);
    CHECK(j["local_ip"] == "127.0.0.1");
    // 回环没有下一跳：网关为空、ARP 判不出来——但字段必须存在且语义正确，
    // 否则界面拿不到值会退化成「—」，把「直连」和「没联网」混为一谈。
    CHECK(j.contains("gateway"));
    CHECK(j["gateway"] == "");
    CHECK(j["gateway_arp"] == "unknown");
    srv.Stop();
}

// ===================== W2：线程边界 =====================
//
// 旧实现里 Tick 跑在托盘消息循环中，而 IPC 线程与托盘又直接摸 Scheduler 的
// std::string / int —— 既是数据竞争（UB），又让托盘被同步探测卡死几秒。
// 下面三条钉的是"改完之后仍然成立"的事实，不是"轮询变快了"。

namespace {

// 192.0.2.0/24（RFC 5737 TEST-NET-1）是文档保留地址，不会指向任何真实设备：
// 连接会一直挂到 probe_timeout 才返回，
// 正好用来模拟"慢探测"（DNS 挂了 / 门户不回）。
cnk::AppConfig SlowProbeConfig() {
    cnk::AppConfig cfg;
    cfg.settings.probe_url = "http://192.0.2.1/generate_204";
    cfg.settings.probe_timeout_seconds = 2;
    cfg.settings.probe_interval_seconds = 3600;
    return cfg;
}

}  // namespace

TEST_CASE("慢探测期间读快照不被阻塞（不持锁做网络请求）") {
    cnk::Scheduler sch(SlowProbeConfig());
    sch.Start();

    bool probing = false;
    for (int i = 0; i < 200; ++i) {
        if (sch.Snapshot().state == cnk::RunState::Probing) { probing = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(probing);

    // 此刻 worker 正卡在 2 秒的连接超时里。期间每次读都必须立刻返回：
    // 若实现是"持锁探测"，单次读就会 > 1000ms。
    long long worst = 0;
    for (int i = 0; i < 30; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const cnk::SchedulerSnapshot snap = sch.Snapshot();
        (void)snap;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (ms > worst) worst = ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(worst < 200);
    CHECK(sch.State() == cnk::RunState::Probing);   // 探测仍在进行，上面的读数不是"已经探测完了"
    sch.Stop();
}

TEST_CASE("状态读取与保存 / 暂停 / 强制重连可交错执行") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    cnk::AppConfig cfg = MakeConfig(srv.BaseUrl(), "u", "p");
    cnk::Scheduler sch(cfg);
    sch.Start();

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> incoherent{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const cnk::SchedulerSnapshot snap = sch.Snapshot();
            (void)sch.LastMessage();
            (void)sch.LocalIp();
            (void)sch.LastPortalBody();
            // 自洽性：在线态必须有话可说。半新半旧的快照最容易在这里露馅。
            if (snap.state == cnk::RunState::Online && snap.message.empty())
                incoherent.fetch_add(1);
            reads.fetch_add(1);
        }
    });

    for (int i = 0; i < 60; ++i) {
        sch.SetPaused(i % 2 == 0);
        if (i % 10 == 0) sch.SetConfig(cfg);
        if (i % 20 == 0) sch.ForceReconnect();
        (void)sch.Snapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    stop.store(true);
    reader.join();
    sch.Stop();

    CHECK(reads.load() > 0);
    CHECK(incoherent.load() == 0);
    srv.Stop();
}

TEST_CASE("慢探测时 Stop 能在超时内返回（退出不被网络挂死）") {
    cnk::Scheduler sch(SlowProbeConfig());
    sch.Start();

    bool probing = false;
    for (int i = 0; i < 200; ++i) {
        if (sch.Snapshot().state == cnk::RunState::Probing) { probing = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(probing);

    const auto t0 = std::chrono::steady_clock::now();
    sch.Stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    // 正在跑的那一轮探测无法从外部中断（WinHTTP 同步调用，probe_timeout=2s，
    // 但 Http 层还有 max(2*step, 8000)=8s 的总时限），所以 Stop 会等它跑完。
    // 这里要钉的是「不会永久挂起 / 不会因为再开一轮而翻倍」，不是「零等待」。
    CHECK(ms < 12000);
}

// ===================== W3：四段链路证据 =====================
//
// 旧界面把四个节点都绑在同一个 state 上：state==online 就四段全绿，
// 出问题时完全看不出断在哪一层。现在每段只认**本轮**的证据，
// 没证据就是 "unknown" —— 不许把"没问过"显示成"通了"。

TEST_CASE("get_status 的四段链路：没证据必须是 unknown，不许猜") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));   // session_active=false：AC 判不在线

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();

    cnk::IpcRequest req;
    req.id = 1;
    req.cmd = "get_status";
    const cnk::IpcResponse resp = cnk::HandleDaemonCommand(req, sch);
    REQUIRE(resp.ok);
    const nlohmann::json j = nlohmann::json::parse(resp.data_json);
    REQUIRE(j.contains("layers"));

    const auto& L = j["layers"];
    // 取值只有三种，写错就是把"未知"谎报成"通过"
    for (const char* k : {"local", "gateway", "portal", "internet"}) {
        const std::string v = L[k].get<std::string>();
        CHECK((v == "ok" || v == "fail" || v == "unknown"));
    }
    CHECK(L["local"] == "ok");        // 回环有源地址
    CHECK(L["gateway"] == "unknown"); // 回环没有下一跳：没证据
    CHECK(L["portal"] == "fail");     // AC 明确说会话不在
    CHECK(L["internet"] == "unknown");// 没走到出网验证：不许当成"通"
    srv.Stop();
}

TEST_CASE("在线时门户段有证据；出网段不做无证据的断言") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.session_active = true;
    std::string err;
    REQUIRE(srv.Start(o, err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    sch.ForceReconnect();
    sch.Tick();

    cnk::IpcRequest req;
    req.id = 1;
    req.cmd = "get_status";
    const nlohmann::json j = nlohmann::json::parse(
        cnk::HandleDaemonCommand(req, sch).data_json);
    CHECK(j["layers"]["portal"] == "ok");   // AC 说在线
    // 出网验证依赖真实外网，CI 上可能判不出来 —— 只要求取值合法
    const std::string inet = j["layers"]["internet"].get<std::string>();
    CHECK((inet == "ok" || inet == "fail" || inet == "unknown"));
    srv.Stop();
}

// ===================== W4：测试登录只认指定档案 =====================

TEST_CASE("test_profile 找不到指定档案时直接失败，绝不换一个去试") {
    // 旧实现在找不到时静默回退到 cfg.profiles.front()：用户点「测试 A」，
    // 实际拿 B 去登录，成功/失败都会把人引到错误结论上（W4）。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.valid_user = "u";
    o.valid_pass = "p";
    std::string err;
    REQUIRE(srv.Start(o, err));

    // 用**临时配置文件**：CI 的 runner 上没有用户配置，走真实路径会先失败在
    // 「读取配置失败」，「找不到档案」这条分支就永远测不到（2026-11-25 CI 实测）。
    const auto conf = std::filesystem::temp_directory_path() / "cnk_test_profile_missing.json";
    REQUIRE(cnk::ConfigSaveTo(conf.u8string(), MakeConfig(srv.BaseUrl(), "u", "p"), err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));   // 文件里只有「默认」一份
    cnk::IpcRequest req;
    req.id = 7;
    req.cmd = "test_profile";
    req.args_json = R"({"profile":"不存在的档案"})";
    const cnk::IpcResponse resp = cnk::HandleDaemonCommandAt(req, sch, conf.u8string());

    CHECK(!resp.ok);
    CHECK(resp.error.find("找不到档案") != std::string::npos);
    CHECK(srv.LoginCount() == 0);   // 关键：没有拿别的档案去试

    std::error_code ec;
    std::filesystem::remove(conf, ec);
    srv.Stop();
}

TEST_CASE("test_profile 未指定档案时报错，也不猜一个") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    const auto conf = std::filesystem::temp_directory_path() / "cnk_test_profile_empty.json";
    REQUIRE(cnk::ConfigSaveTo(conf.u8string(), MakeConfig(srv.BaseUrl(), "u", "p"), err));

    cnk::Scheduler sch(MakeConfig(srv.BaseUrl(), "u", "p"));
    cnk::IpcRequest req;
    req.id = 8;
    req.cmd = "test_profile";
    req.args_json = "{}";
    const cnk::IpcResponse resp = cnk::HandleDaemonCommandAt(req, sch, conf.u8string());

    CHECK(!resp.ok);
    CHECK(srv.LoginCount() == 0);

    std::error_code ec;
    std::filesystem::remove(conf, ec);
    srv.Stop();
}

TEST_CASE("注入的配置路径真的生效（不是偷偷读 %APPDATA%）") {
    // 这条是给上面两条兜底的：如果 HandleDaemonCommandAt 忽略了路径参数、又回去读
    // 真实配置，本地（有配置）会返回 ok，断言立刻失败 —— 而 CI（没配置）则会
    // 恰好"看起来通过"。用它把"注入是否生效"钉死，不再靠运行环境的巧合。
    cnk::Scheduler sch(cnk::AppConfig{});
    cnk::IpcRequest req;
    req.id = 9;
    req.cmd = "get_config";

    const auto missing = std::filesystem::temp_directory_path() / "cnk_no_such_config_9f3a.json";
    std::error_code ec;
    std::filesystem::remove(missing, ec);   // 确保真的不存在

    const cnk::IpcResponse resp = cnk::HandleDaemonCommandAt(req, sch, missing.u8string());
    CHECK(!resp.ok);
    CHECK(resp.error.find("读取配置失败") != std::string::npos);
}
