#include "core/Scheduler.h"
#include "core/Log.h"
#include "core/Encoding.h"
#include "core/LocalIp.h"
#include "core/PortalPage.h"
#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace cnk {

namespace {

using Clock = std::chrono::steady_clock;

long long NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 终止态（AuthFailed / NotCampusNetwork）下的巡检间隔：低频确认网络是否恢复，
// 既不打扰用户，也不彻底放弃感知「接入校园网后自动恢复」
constexpr int kStoppedProbeIntervalSeconds = 60;

const char* NetStateName(NetState s) {
    switch (s) {
        case NetState::Online:         return "online";
        case NetState::PortalDetected: return "portal";
        case NetState::Offline:        return "offline";
        default:                       return "unknown";
    }
}

// AC 复核（第 2 层）只对 Dr.COM 有意义：其它厂商的门户没有 /drcom/chkstatus，
// 多发一次请求只会白白拖慢探测（接口不存在时自然返回 -1，不干预判定）。
// vendor="auto" 按 Dr.COM 处理——口径与 IspSuffix 一致，否则本部署（vendor=auto）
// 永远走不到复核这一步。
bool WantsAcRecheck(const Profile& p) {
    if (p.auth_url.empty()) return false;
    return p.vendor.empty() || p.vendor == "auto" || p.vendor == "drcom";
}

}  // namespace

const char* RunStateName(RunState s) {
    switch (s) {
        case RunState::Disconnected:     return "disconnected";
        case RunState::Probing:          return "probing";
        case RunState::PortalDetected:   return "portal";
        case RunState::Online:           return "online";
        case RunState::Authenticating:   return "authenticating";
        case RunState::AuthFailed:       return "auth_failed";
        case RunState::NotCampusNetwork: return "not_campus";
    }
    return "unknown";
}

struct Scheduler::Impl {
    AppConfig cfg;
    RunState state = RunState::Disconnected;
    std::string message = "尚未开始";
    long long last_success = 0;
    long long last_probe = 0;
    int consecutive_auth_failures = 0;
    int total_auth_attempts = 0;
    size_t backoff_index = 0;
    bool ever_succeeded = false;
    bool stop_retrying = false;
    // 只有「会话卡死（连续 3 次注销无效）」这种停手态不允许地址变更通告解除：
    // 它是 AC 侧的会话记录，本机 ARP/地址抖动不会把它变好。凭据错误、非校园网
    // 等停手态仍应在换网后自动恢复。
    bool session_stuck = false;
    // 托盘「暂停自动重连」：暂停只挡自动登录，探测与状态显示照常。
    // atomic 而不是普通 bool：托盘/IPC 线程写、调度线程读，必须无竞争。
    std::atomic<bool> paused{false};
    // ForceReconnect（用户主动点「立即重连」）放行一次，不受暂停影响。
    bool manual_override = false;

    // 「会话被占 / 僵尸会话」的处置计数（2026-10-30）。
    // 为什么要单独计数：这两种失败**重试无效**，只能靠「注销 → 重登」清理。
    // 但注销也不能无限打（会打草惊蛇、也可能与门户的风控冲突），故限次 + 冷却，
    // 到上限就停手并明确告诉用户去门户手动处理，绝不容许空转。
    int inuse_logout_count = 0;
    long long last_logout_unix = 0;

    Clock::time_point next_probe{};
    // 上次向 AC（认证服务器）复核会话的结果与时间。探测间隔与 AC 复核节流都对齐
    // router 端的 30 秒：一轮最多发 3~4 个 HTTP，间隔过短会压 AC 并放大 DNS 抖动。
    // ⚠ 节流期间必须**复用上次结论**，不能当「查不出来」：AC 说会话没了是确定性结论，
    // 丢掉它就会在下一轮退回只靠出网验证，把刚判死的会话又判回在线。
    // 登录成功/网络变更/用户手动重连时无条件失效，避免旧结论压住新状态。
    Clock::time_point last_ac_check{};
    int last_ac_verdict = -1;   // -1=未知 1=在线 0=不在线
    static constexpr int kAcRecheckIntervalSeconds = 30;

    // 最近一轮探测的分层证据（1=通过 0=失败 -1=无证据），随快照发布给界面。
    // 只认**本轮**取到的证据：上一轮的"通过"不留用到这一轮（W3）。
    int layer_local = -1;
    int layer_gateway = -1;
    int layer_portal = -1;
    int layer_internet = -1;
    // 由网络监听线程置位、主线程在 Tick 中消费。
    // 跨线程通信只允许经由这两个 atomic（网络线程不得触碰普通成员，防数据竞争）。
    std::atomic<bool> probe_immediately{true};
    std::atomic<bool> network_changed{false};

    // ---- W2：跨线程只经由「命令队列 + 快照」交换 ----
    //
    // 为什么需要：IPC 线程（DaemonCommands）和托盘定时器都在读 Scheduler，以前它们
    // 逐个调 getter 直接摸这里的 std::string / int，而调度线程正在 DoProbe/DoLogin 里
    // 改写 —— 读 std::string 与写并发是 UB，而且一次 get_status 拼出来的字段可能
    // 半新半旧。现在：写者只有调度线程（Tick），它在每轮结束时发布整份快照。
    mutable std::mutex snap_mu_;
    SchedulerSnapshot snap_;

    // 命令队列：SetConfig 会拷整份配置（含 std::string），只能由调度线程写入 cfg。
    struct Command {
        enum class Kind { SetConfig, ForceReconnect } kind;
        AppConfig cfg;
    };
    std::mutex cmd_mu_;
    std::deque<Command> cmds_;

    // 保护 cfg 的跨线程拷贝（RunProbeNow 要一份 settings）。只拷数据，探测在锁外做。
    mutable std::mutex cfg_mu_;

    // worker 的等待：睡到下次探测时间，或被命令 / Stop 唤醒（不靠高频轮询）。
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    std::thread worker;
    std::atomic<bool> worker_stop{false};
    std::atomic<bool> worker_wake{false};

    // 本机到门户方向的源地址与下一跳。由 DoProbe 在「结论变化 / 还没有值」时重算，
    // 供 get_status 与认证载荷使用（此前是永远为空的 TODO，界面「本机 IP」一直是空白）。
    std::string local_ip;
    LocalRoute local_route;
    // 上一次写进日志的探测结论（-1 = 还没记过）：只在变化时记一行，避免刷屏
    int last_logged_probe_state = -1;
    // 持续故障（offline/unknown）时上次记下的 message：message 变化也要记——
    // 否则「DNS 失败」变「连接超时」这种故障层次迁移会被去重吞掉，事后只剩一行 offline。
    std::string last_logged_fault_message;
    std::string cached_portal_body;
    std::string cached_portal_url;
    std::string cached_server_header;

    HttpResponse last_probe_response;   // DoProbe 中赋值，DoLogin 读取

    void ResetCache() {
        // 注意：**不**清 local_ip/local_route。它们描述的是"本机长什么样"，
        // 不是门户页面缓存；在线成功后清掉会让界面本机 IP 永远空白
        //（这正是此前 get_status.local_ip 恒空的另一半原因）。
        // 网络环境变化时才该失效，见 Tick 中的 network_changed 分支。
        cached_portal_body.clear();
        cached_portal_url.clear();
        cached_server_header.clear();
    }

    void ScheduleNext(int seconds) {
        next_probe = Clock::now() + std::chrono::seconds(seconds);
    }

    // ---------------- 快照与命令（W2） ----------------

    /// 把当前状态打包成快照并发布。写者只有调度线程，读方永远看到自洽的一组值。
    void Publish() {
        SchedulerSnapshot s;
        s.state = state;
        s.message = message;
        s.local_ip = local_ip;
        s.gateway = local_route.gateway;
        s.gateway_arp = local_route.gateway_arp;
        s.last_success = last_success;
        s.last_probe = last_probe;
        s.consecutive_auth_failures = consecutive_auth_failures;
        s.ineffective_logouts = inuse_logout_count;
        s.portal_body = cached_portal_body;
        s.paused = paused.load();
        s.layer_local = layer_local;
        s.layer_gateway = layer_gateway;
        s.layer_portal = layer_portal;
        s.layer_internet = layer_internet;
        std::lock_guard<std::mutex> lk(snap_mu_);
        snap_ = std::move(s);
    }

    /// 只取快照里的一个字段（加锁极短，不碰网络）。
    template <typename F>
    auto Read(F f) const -> decltype(f(snap_)) {
        std::lock_guard<std::mutex> lk(snap_mu_);
        return f(snap_);
    }

    void Submit(Command c) {
        {
            std::lock_guard<std::mutex> lk(cmd_mu_);
            cmds_.push_back(std::move(c));
        }
        worker_wake.store(true);
        wait_cv_.notify_all();
    }

    void ApplySetConfig(const AppConfig& c) {
        {
            std::lock_guard<std::mutex> lk(cfg_mu_);
            cfg = c;
        }
        stop_retrying = false;
        session_stuck = false;
        inuse_logout_count = 0;
        last_logout_unix = 0;
        total_auth_attempts = 0;
        consecutive_auth_failures = 0;
        backoff_index = 0;
        probe_immediately.store(true);
        // 换了配置（可能连门户地址都改了）：AC 复核重新问一次
        last_ac_check = Clock::time_point{};
        last_ac_verdict = -1;
    }

    void ApplyForceReconnect() {
        // 用户主动触发：即使处于「暂停自动重连」也放行这一次登录
        manual_override = true;
        probe_immediately.store(true);
        stop_retrying = false;
        session_stuck = false;
        total_auth_attempts = 0;
        consecutive_auth_failures = 0;
        backoff_index = 0;
        // 用户主动点「立即重连」：AC 复核也立刻重问一次，别让 30 秒节流把第一次判定吃掉
        last_ac_check = Clock::time_point{};
        last_ac_verdict = -1;
    }

    void DrainCommands() {
        std::deque<Command> local;
        {
            std::lock_guard<std::mutex> lk(cmd_mu_);
            local.swap(cmds_);
        }
        for (auto& c : local) {
            if (c.kind == Command::Kind::SetConfig) ApplySetConfig(c.cfg);
            else ApplyForceReconnect();
        }
    }

    /// 唯一的状态写线程：睡到下次探测时间，或被命令 / Stop 唤醒，然后跑一轮 Tick。
    void WorkerLoop() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(wait_mu_);
                wait_cv_.wait_until(lk, next_probe, [this] {
                    return worker_stop.load() || worker_wake.exchange(false);
                });
            }
            if (worker_stop.load()) return;
            Tick();
        }
    }

    int CurrentBackoff() const {
        if (cfg.settings.retry_backoff_seconds.empty()) return 60;
        const int v = backoff_index >= cfg.settings.retry_backoff_seconds.size()
            ? cfg.settings.retry_backoff_seconds.back()
            : cfg.settings.retry_backoff_seconds[backoff_index];
        return v > 0 ? v : 1;  // 钳制下限：防配置为 0/负数产生紧凑重试循环
    }

    void AdvanceBackoff() {
        if (backoff_index + 1 < cfg.settings.retry_backoff_seconds.size())
            ++backoff_index;
    }

    void DoProbe();
    void DoLogin();

    // 「终端已在线」型成功的复测次数与间隔：AC 数据面放行有秒级延迟，
    // 单次探测不足以断定会话是死的。
    static constexpr int kVerifyAttempts = 3;
    static constexpr int kVerifyDelayMs = 1200;
    // 注销的冷却与限次：60 秒内最多一次，连续 3 次没效果就停手交给人
    static constexpr long long kLogoutCooldownSeconds = 60;
    static constexpr int kMaxIneffectiveLogouts = 3;

    bool VerifyOnline() {
        const ProbeResult r = ProbeUntilOnline(
            [this] { return ProbeOnce(cfg.settings); }, kVerifyAttempts, kVerifyDelayMs);
        return r.state == NetState::Online;
    }

    void FinishOnline(const std::string& msg) {
        state = RunState::Online;
        message = msg;
        last_success = NowUnix();
        ever_succeeded = true;
        session_stuck = false;
        consecutive_auth_failures = 0;
        inuse_logout_count = 0;
        backoff_index = 0;
        // 刚登录成功：AC 那边的会话是新的，旧的复核结论（可能就是「不在线」）必须作废
        last_ac_verdict = -1;
        last_ac_check = Clock::time_point{};
        ScheduleNext(cfg.settings.probe_interval_seconds);
        ResetCache();
    }

    // 注销一次（受冷却与限次约束）。返回 true 表示**确实发了**注销请求。
    bool TryLogout(IAuth& auth, const Profile& p) {
        const long long now = NowUnix();
        if (inuse_logout_count >= kMaxIneffectiveLogouts) return false;
        if (last_logout_unix != 0 && now - last_logout_unix < kLogoutCooldownSeconds)
            return false;
        last_logout_unix = now;
        ++inuse_logout_count;
        std::string lmsg;
        const bool accepted = auth.Logout(p, lmsg);
        LogInfo(std::string("已发送注销请求（") + (accepted ? "接口接受" : "接口未接受") +
                "）：" + lmsg);
        return true;
    }

    // 「账号会话被占」：服务器说已在线、却没说是本终端。
    // 处置：注销 → 立即重登一次（重登结果是唯一的真相来源）。
    void HandleSessionConflict(IAuth& auth, const Profile& p, const NetContext& ctx) {
        state = RunState::Disconnected;
        if (TryLogout(auth, p)) {
            const LoginResult again = auth.Login(p, ctx);
            LogInfo(std::string("注销后重登：") + (again.success ? "成功" : "失败") +
                    "，信息=" + RedactSecrets(again.message));
            if (again.success && !again.terminal_online) {
                FinishOnline("会话被占：已注销并重新登录");
                return;
            }
            message = "会话被占：已注销并重登，仍未成功（" +
                      std::to_string(inuse_logout_count) + "/" +
                      std::to_string(kMaxIneffectiveLogouts) + "）";
            ScheduleNext(15);
            return;
        }
        if (inuse_logout_count >= kMaxIneffectiveLogouts) {
            state = RunState::AuthFailed;
            stop_retrying = true;
            session_stuck = true;
            message = "账号会话一直被占用（已注销 " +
                      std::to_string(kMaxIneffectiveLogouts) +
                      " 次仍无效）：请到门户手动注销，或断开占用该账号的其它设备后重试";
            LogWarn(message);
            ScheduleNext(kStoppedProbeIntervalSeconds);
            return;
        }
        // 冷却中：按退避等，别把注销打成连发
        const int wait = CurrentBackoff();
        AdvanceBackoff();
        message = "账号会话被占用，等待服务器释放（" + std::to_string(wait) + " 秒后重试）";
        ScheduleNext(wait);
    }

    // 「终端 IP 已在线」但复测不在线 = 僵尸会话（AC 记着在线、转发面已废）。
    // 处置与上一条相同：注销 → 重登；不清掉那条记录，重试多少次都是同一句"已在线"。
    void HandleZombieSession(IAuth& auth, const Profile& p, const NetContext& ctx) {
        LogWarn("登录被判『终端已在线』但复测不在线：判定为僵尸会话，尝试注销后重登");
        state = RunState::Disconnected;
        if (TryLogout(auth, p)) {
            // 注销是否被 AC 确认：chkstatus 明确说「会话还在」才判本次注销无效。
            // 真机 2026-11-15：老式端点回 "Logout succeed."，AC 却仍记着在线，
            // 重登照旧「终端IP已在线！」——只看 200 正文会把假成功当成功。
            // 返回 -1（查不出来 / 非 Dr.COM）时不干预，仍以重登结果为唯一真相。
            const bool still_online = (DrcomAcSessionState(p) == 1);
            if (still_online)
                LogWarn("注销请求被接口接受，但 AC 仍记着会话在线：本次注销无效");
            const LoginResult again = auth.Login(p, ctx);
            if (!still_online && again.success && !again.terminal_online && VerifyOnline()) {
                FinishOnline("僵尸会话已清理，重新登录成功");
                return;
            }
            message = "僵尸会话未清理（已注销 " + std::to_string(inuse_logout_count) + "/" +
                      std::to_string(kMaxIneffectiveLogouts) + "），稍后重试";
            LogWarn(message);
            ScheduleNext(30);
            return;
        }
        if (inuse_logout_count >= kMaxIneffectiveLogouts) {
            state = RunState::AuthFailed;
            stop_retrying = true;
            session_stuck = true;
            message = "服务器侧会话一直清不掉（已注销 " +
                      std::to_string(kMaxIneffectiveLogouts) +
                      " 次）：请在门户手动注销后重试";
            LogWarn(message);
            ScheduleNext(kStoppedProbeIntervalSeconds);
            return;
        }
        const int wait = CurrentBackoff();
        AdvanceBackoff();
        message = "会话记录未清理，等待后重试（" + std::to_string(wait) + " 秒）";
        ScheduleNext(wait);
    }

    void Tick() {
        // 每轮结束（含所有早退路径）都发布一次快照：读方永远拿得到当前状态。
        // 用 RAII 而不是在每个 return 前手写 —— DoProbe 里有十几处 return。
        struct PublishOnExit {
            Impl* self;
            ~PublishOnExit() { self->Publish(); }
        } publish_on_exit{this};

        // 命令先消费：SetConfig / ForceReconnect 必须在同一轮生效，
        // 否则「点了立即重连还要等下一轮」的体验会退回去。
        DrainCommands();

        // 已经请求停止：不再发起新的探测/登录。Stop() 只等"正在跑的那一轮"结束，
        // 不会因为又开了一轮而把退出时间翻倍。
        if (worker_stop.load()) return;

        // 网络变更事件在调度线程消费：重置退避等与网络环境相关的状态。
        // ⚠ 注销限次与「会话卡死」停手态不在此列，见下方注释。
        if (network_changed.exchange(false)) {
            // ⚠ inuse_logout_count 绝不能在这里清零：僵尸会话是 AC 侧那条会话记录，
            // 本机地址通告、ARP 抖动都不会把它变好。真机 2026-11-15 实测
            // NotifyAddrChange 每 10~30 秒触发一次，一清就把「连续 3 次无效即停手」
            // 永远重置，8 分钟内打了 24 次登录 + 7 次注销，直到用户手动改配置。
            // 同理，已因会话卡死而停手的不再由地址通告解除；但凭据错误、非校园网
            // 这类停手态换网后仍要自动恢复，不能一刀切全挡住。
            if (!session_stuck) stop_retrying = false;
            total_auth_attempts = 0;
            consecutive_auth_failures = 0;
            backoff_index = 0;
            // 地址/网关/ARP 一定变了，下一轮探测重算；AC 复核也重置（换了网就该立刻问一次）
            local_ip.clear();
            local_route = LocalRoute{};
            last_ac_check = Clock::time_point{};
            last_ac_verdict = -1;
            // 让下一轮探测必定重记一行（含新的本机地址/网关），否则"换了网"在日志里无痕
            last_logged_probe_state = -1;
            last_logged_fault_message.clear();
        }
        if (probe_immediately.load() || Clock::now() >= next_probe) {
            probe_immediately.store(false);
            DoProbe();
        }
    }
};

void Scheduler::Impl::DoProbe() {
    // 一次性消费手动放行标志：只有本轮探测能用，不会漏到下一轮把暂停态架空
    const bool manual = manual_override;
    manual_override = false;

    const bool was_stopped = stop_retrying;
    const RunState stopped_state = state;

    state = RunState::Probing;
    last_probe = NowUnix();
    // 每轮重新取证：上一轮的"通过"不能留给这一轮用（W3 的分层状态只认本轮证据）
    layer_local = layer_gateway = layer_portal = layer_internet = -1;
    // 探测可能卡几秒（DNS 挂了、门户不回）：进来就先发布一次，界面立刻显示「探测中」。
    // 旧实现只在 Tick 结束时才让人看见状态变化，慢探测期间界面像是"什么都没发生"。
    Publish();

    ProbeResult pr = ProbeOnce(cfg.settings);
    last_probe_response = pr.response;

    // 探测结论只在「状态变化」时记 INFO：出问题时能一眼看出是"根本没网"还是"被劫持"。
    // 2026-10-26 的事故就是缺这一行——日志里只有"网络变更事件"和"连接恢复"，
    // 中间 8 分钟既没网也连不上门户，事后完全看不出当时探测到的是什么。
    // 额外规则：持续故障（offline/unknown）时，message 变化也要记——
    // "DNS 失败"变"连接超时"是故障层次迁移，只按状态去重会把它吞掉。
    const bool state_changed = (int)pr.state != last_logged_probe_state;
    const bool faulting = pr.state == NetState::Offline || pr.state == NetState::Unknown;
    const bool fault_msg_changed = faulting && pr.message != last_logged_fault_message;

    // 门户地址：日志判读、本机地址/网关、AC 复核都走同一个口径
    const Profile* active = ActiveProfile(cfg);
    const std::string portal_url =
        (active && !active->auth_url.empty()) ? active->auth_url : cfg.settings.probe_url;

    // 本机地址：按门户地址取（门户是字面 IP 时不查 DNS），没有值就补一次并缓存。
    // 此前后台从没给 local_ip 赋过值，get_status 里永远是空串。
    if (local_ip.empty()) local_ip = LocalIpForPortal(portal_url);
    layer_local = local_ip.empty() ? -1 : 1;

    // 网关/ARP：只在结论变化时问系统。RouteForUrl 要先解析门户主机名，
    // 域名型门户在 DNS 挂掉时每次都会卡几秒——不能放进每轮探测的必经路径
    //（2026-11-01 那次故障正是「DNS 和门户一起挂」，每 3 秒一次解析会拖死调度）。
    if (state_changed || fault_msg_changed) {
        local_route = RouteForUrl(portal_url);
        last_logged_probe_state = (int)pr.state;
        if (faulting) last_logged_fault_message = pr.message;
        // 顺带记下「到门户时本机长什么样」：没有地址 / 链路本地 / 有地址但网关不通，
        // 这三种在日志里必须能区分开，否则"连不上"仍然是个黑盒。
        LogInfo(std::string("探测结果：") + NetStateName(pr.state) + "（" +
                RedactSecrets(pr.message) + "）｜" + DescribeRoute(local_route));
    }

    /* 网关段：只有 ARP「已解析」才是通过的证据；「未解析」是明确的失败证据；
       直连/回环/查不出来一律未知（-1）——别把"没问"当成"通了"。 */
    layer_gateway = local_route.gateway_arp == 1 ? 1
                  : local_route.gateway_arp == 0 ? 0 : -1;

    if (pr.state == NetState::Online) {
        // 第二层：AC 复核。探测页 200+"Success" 不证明会话还在——本网实测 DNS 被劫持后
        // 探测域名照样回 Success，而 AC 那边会话早就没了。只有问认证服务器自己才分得清。
        // 返回 -1（接口不存在/超时/回包异常）时**不干预**判定：否则非 Dr.COM 环境会被
        // 一律判成断网。节流期内复用上次结论（见 last_ac_verdict 的注释）。
        int ac = -1;
        if (active && WantsAcRecheck(*active)) {
            if (last_ac_check == Clock::time_point{} ||
                Clock::now() - last_ac_check >=
                    std::chrono::seconds(kAcRecheckIntervalSeconds)) {
                last_ac_check = Clock::now();
                last_ac_verdict = DrcomAcSessionState(*active);
            }
            ac = last_ac_verdict;
        }
        layer_portal = ac == 1 ? 1 : (ac == 0 ? 0 : -1);
        if (ac == 0) {
            LogWarn("探测页可通但认证服务器判定不在线（AC 会话多半已失效），按离线处理");
            pr.state = NetState::Offline;
            pr.message = "探测页可通但认证服务器判定不在线（疑似会话已失效）";
            // 落到下面的 Offline 分支
        } else if (const int can = CanReachInternet(); can == 0) {
            layer_internet = 0;
            // 第三层验证：探测页 200+"Success" 不证明真能出网——DNS 被劫持时探测页照样 200，
            // 而实际打不开网页（2026-10-30 router 端实测僵尸会话持续 9 小时无人察觉）。
            // 明确出不了网（有响应但内容不对）才降级离线、触发重连；判不出来维持在线。
            LogWarn("探测判在线但实际出不了网（疑似僵尸会话/劫持），降级为离线");
            pr.state = NetState::Offline;
            pr.message = "探测页可通但实际出不了网（疑似僵尸会话）";
            // 落到下面的 Offline 分支
        } else {
            layer_internet = can == 1 ? 1 : -1;
            state = RunState::Online;
            message = "网络正常";
            ever_succeeded = true;
            backoff_index = 0;
            consecutive_auth_failures = 0;
            inuse_logout_count = 0;
            // 网络已恢复（可能是用户手动登录成功）：解除停止，恢复正常调度
            stop_retrying = false;
            session_stuck = false;
            total_auth_attempts = 0;
            ScheduleNext(cfg.settings.probe_interval_seconds);
            ResetCache();
            return;
        }
    }

    /* 探测页被劫持到门户：这是「没认证」的**证据**（不是猜的），门户段记失败。
       offline / unknown 不构成门户段的证据，保持 -1（界面显示"未知"）。 */
    if (pr.state == NetState::PortalDetected) layer_portal = 0;

    // 终止态巡检：只确认网络是否恢复，不改写状态与消息——
    // 否则「认证失败（待用户改配置）」的显示会被常规探测流程覆盖
    if (was_stopped) {
        state = stopped_state;
        ScheduleNext(kStoppedProbeIntervalSeconds);
        return;
    }

    if (pr.state == NetState::Offline) {
        state = RunState::Disconnected;
        message = pr.message;
        ScheduleNext(3);  // 快速重探，改善恢复延迟
        return;
    }

    if (pr.state == NetState::Unknown) {
        // 尚未配置有效的探测地址；等待用户补齐配置（SetConfig 会重置调度）
        state = RunState::Disconnected;
        message = pr.message;
        ScheduleNext(3);
        return;
    }

    // PortalDetected
    state = RunState::PortalDetected;
    // 暂停时跳过登录：仍显示「需认证」状态，但绝不拿账号去撞门户。
    // 用户手动点「立即重连」时 manual=true，放行这一次。
    if (paused && !manual) {
        message = "已暂停自动重连：探测到门户劫持，未发起登录";
        ScheduleNext(cfg.settings.probe_interval_seconds);
        return;
    }
    message = pr.message;
    // spec §7：识别（DoLogin 中执行）的输入是「Portal 的响应头与页面 HTML」。
    // 真实劫持 302 常不含指纹（本机实测 2026-10-16 为裸 302），须先抓取
    // Location 目标页；抓取失败退回探测响应（尽力而为，不阻断登录链路）。
    PortalPageResult pp = ResolvePortalPage(pr, cfg.settings.probe_url);
    if (pp.fetched) {
        LogInfo("已抓取门户页面：" + pp.portal_url);
    } else if (!pp.fetch_error.empty()) {
        LogInfo("门户页面抓取失败（" + RedactSecrets(pp.fetch_error) +
                "），改用探测响应识别");
    }
    // portal_url 的语义是「探测阶段发现的重定向目标」（spec §4.1）：
    // 3xx 劫持时是 resolve 后的 Location；200 篡改时没有重定向目标，
    // 留空让下游（如通用表单认证）退回用户配置的 auth_url——
    // 绝不能填探测地址本身（那是外部地址，会让下游拼出错误的登录 URL）
    cached_portal_url = pp.portal_url;
    cached_server_header = pp.response.server;
    cached_portal_body = DecodeBody(pp.response);
    last_probe_response = pp.response;  // DoLogin 的识别输入 = Portal 页面响应

    // 服务器下发的免劫持清单：只作为诊断信息（`--detect` 输出），不自动改探测地址。
    // 原因（2026-11-02 复核）：
    //   ① 这段以前的代码是"记一条『采用服务器下发的探测地址』然后继续用旧地址"——
    //      日志在说谎，实际探测一直用 cfg.settings.probe_url；
    //   ② 真要采用也不安全：清单只说明"这些地址免劫持"，不等于"一定可达"（本网实测
    //      msftncsi 曾 0/4）。而自动登录的触发条件是"探测到门户"，若清单里的地址
    //      在未认证时被 AC 直接黑洞掉，探测就只会得到"离线"，永远走不到登录；
    //   ③ 换探测地址等于换判定基准，必须带可验证的失败回退才有资格上线。
    // 需要建议地址时用 `--detect`，那里会真的打一次再给出结论。

    DoLogin();
}

void Scheduler::Impl::DoLogin() {
    const Profile* p = ActiveProfile(cfg);
    if (!p) {
        state = RunState::AuthFailed;
        message = "没有可用的账号配置，请在设置中添加";
        stop_retrying = true;
        return;
    }
    if (stop_retrying) return;

    state = RunState::Authenticating;
    ++total_auth_attempts;
    Publish();   // 登录也可能慢（门户 8 秒超时），别让界面一直停在「探测中」

    Vendor detected = DetectVendor(last_probe_response);

    // 「按识别结果生效的厂商」要写进档案副本：配置默认 vendor="auto"，而 IspSuffix
    // 是按**档案里的** vendor 分支的（auto 被当作 Dr.COM → @dx/@cmcc/@lt）。不改写的话，
    // 锐捷/深澜用户会带着 Dr.COM 的后缀去认证，被服务器判「账号或密码错误」。
    // 策略选择与后缀拼接从此共用同一个生效厂商。
    Profile effective = *p;
    if (effective.vendor == "auto" || effective.vendor.empty())
        effective.vendor = VendorName(detected);

    NetContext ctx;
    ctx.local_ip = local_ip;
    ctx.portal_url = cached_portal_url;
    ctx.portal_body = cached_portal_body;
    ctx.server_header = cached_server_header;

    auto auth = CreateAuth(detected, effective);
    LogInfo(std::string("尝试登录，策略=") + auth->Name() +
            "，识别厂商=" + VendorName(detected));

    LoginResult lr = auth->Login(effective, ctx);

    LogInfo(std::string("登录结果：") + (lr.success ? "成功" : "失败") +
            "，信息=" + RedactSecrets(lr.message));
    LogInfo("请求地址：" + RedactSecrets(lr.request_url));

    if (!lr.success && lr.in_use) {
        HandleSessionConflict(*auth, effective, ctx);
        return;
    }

    if (lr.success) {
        // 「终端 IP 已在线」型成功**没有真正完成认证**：AC 里可能只是个僵尸会话
        //（记着在线、转发面已废）。不复测就会每 5 秒记一次"已重连"、实际还是上不了网
        //（2026-10-30 定位到的假死形态）。
        if (lr.terminal_online && !VerifyOnline()) {
            HandleZombieSession(*auth, effective, ctx);
            return;
        }
        FinishOnline(lr.terminal_online ? "服务器报告该终端已在线（无需重新登录）"
                                        : "已重新登录");
        return;
    }

    // 厂商明确报凭据错误：停止重试，等用户改配置
    if (lr.vendor_code == 2) {
        state = RunState::AuthFailed;
        message = lr.message.empty() ? "账号或密码错误" : lr.message;
        stop_retrying = true;
        LogError("检测到凭据错误，已停止自动重试：" + message);
        ScheduleNext(kStoppedProbeIntervalSeconds);  // 低频巡检，感知环境恢复
        return;
    }

    ++consecutive_auth_failures;

    // 非校园网保护：从未成功过，且连续尝试达到上限
    const int limit = 3;
    if (!ever_succeeded && total_auth_attempts >= limit) {
        state = RunState::NotCampusNetwork;
        message = "多次尝试均失败，当前网络可能不是校园网，已停止自动重试。"
                  "接入校园网后将自动恢复。";
        stop_retrying = true;
        LogWarn(message);
        ScheduleNext(kStoppedProbeIntervalSeconds);
        return;
    }

    state = RunState::Disconnected;
    message = lr.message.empty() ? "登录失败，将重试" : lr.message;
    const int wait = CurrentBackoff();
    AdvanceBackoff();
    LogInfo("登录失败，将在 " + std::to_string(wait) + " 秒后重试");
    ScheduleNext(wait);
}

Scheduler::Scheduler(AppConfig cfg) : impl_(new Impl) {
    impl_->cfg = std::move(cfg);
    // 构造后立刻有一份自洽快照：界面第一次 get_status 不必等第一轮 Tick
    impl_->Publish();
}

Scheduler::~Scheduler() { Stop(); }

void Scheduler::Start() {
    if (impl_->worker.joinable()) return;   // 幂等
    impl_->worker_stop.store(false);
    impl_->worker_wake.store(false);
    Impl* p = impl_.get();                  // 捕获 Impl 而非 this：Stop/join 期间 Scheduler 可能已在析构
    impl_->worker = std::thread([p] { p->WorkerLoop(); });
}

void Scheduler::Stop() {
    impl_->worker_stop.store(true);
    impl_->worker_wake.store(true);
    impl_->wait_cv_.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void Scheduler::Tick() { impl_->Tick(); }

void Scheduler::ForceReconnect() {
    // 命令而不是直接改字段：SetConfig/ForceReconnect 会碰 cfg 与一堆状态，
    // 只能由调度线程写（W2）。
    impl_->Submit({Impl::Command::Kind::ForceReconnect, AppConfig{}});
}

void Scheduler::SetConfig(const AppConfig& cfg) {
    impl_->Submit({Impl::Command::Kind::SetConfig, cfg});
}

void Scheduler::NotifyNetworkChanged() {
    // 本方法可从网络监听线程调用：只允许写 atomic 标志，
    // 其余状态重置推迟到调度线程的 Tick 中消费（避免数据竞争）
    impl_->network_changed.store(true);
    impl_->probe_immediately.store(true);
    impl_->worker_wake.store(true);          // 唤醒 worker：换网后要立刻探测
    impl_->wait_cv_.notify_all();
    LogInfo("收到网络变更事件，立即重新探测");
}

void Scheduler::SetPaused(bool paused) { impl_->paused.store(paused); }
bool Scheduler::IsPaused() const { return impl_->paused.load(); }

SchedulerSnapshot Scheduler::Snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->snap_mu_);
    return impl_->snap_;
}

RunState Scheduler::State() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.state; });
}
std::string Scheduler::LastMessage() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.message; });
}
std::string Scheduler::LocalIp() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.local_ip; });
}
std::string Scheduler::Gateway() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.gateway; });
}
int Scheduler::GatewayArpState() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.gateway_arp; });
}
long long Scheduler::LastSuccessUnix() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.last_success; });
}
long long Scheduler::LastProbeUnix() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.last_probe; });
}
int Scheduler::ConsecutiveAuthFailures() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.consecutive_auth_failures; });
}

int Scheduler::IneffectiveLogoutCount() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.ineffective_logouts; });
}

std::string Scheduler::LastPortalBody() const {
    return impl_->Read([](const SchedulerSnapshot& s) { return s.portal_body; });
}

bool Scheduler::RunProbeNow(ProbeResult& out) {
    // 只把 settings 拷出来（持锁极短），探测本身在锁外做 —— 不持锁做网络请求
    Settings settings;
    {
        std::lock_guard<std::mutex> lk(impl_->cfg_mu_);
        settings = impl_->cfg.settings;
    }
    out = ProbeOnce(settings);
    // 未配置有效探测地址（Unknown）视为未完成探测
    return out.state != NetState::Unknown;
}

}  // namespace cnk
