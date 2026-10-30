#include "core/Scheduler.h"
#include "core/Log.h"
#include "core/Encoding.h"
#include "core/LocalIp.h"
#include "core/PortalPage.h"
#include "core/ProbeTarget.h"
#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include <atomic>

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

    // 「会话被占 / 僵尸会话」的处置计数（2026-10-30）。
    // 为什么要单独计数：这两种失败**重试无效**，只能靠「注销 → 重登」清理。
    // 但注销也不能无限打（会打草惊蛇、也可能与门户的风控冲突），故限次 + 冷却，
    // 到上限就停手并明确告诉用户去门户手动处理，绝不容许空转。
    int inuse_logout_count = 0;
    long long last_logout_unix = 0;

    Clock::time_point next_probe{};
    // 由网络监听线程置位、主线程在 Tick 中消费。
    // 跨线程通信只允许经由这两个 atomic（网络线程不得触碰普通成员，防数据竞争）。
    std::atomic<bool> probe_immediately{true};
    std::atomic<bool> network_changed{false};

    std::string local_ip;  // TODO：待接入网卡信息查询（GetAdaptersAddresses）后填充
    // 上一次写进日志的探测结论（-1 = 还没记过）：只在变化时记一行，避免刷屏
    int last_logged_probe_state = -1;
    std::string cached_portal_body;
    std::string cached_portal_url;
    std::string cached_server_header;
    ProbeTarget target;
    bool target_resolved = false;

    HttpResponse last_probe_response;   // DoProbe 中赋值，DoLogin 读取

    void ResetCache() {
        local_ip.clear();
        cached_portal_body.clear();
        cached_portal_url.clear();
        cached_server_header.clear();
    }

    void ScheduleNext(int seconds) {
        next_probe = Clock::now() + std::chrono::seconds(seconds);
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
        consecutive_auth_failures = 0;
        inuse_logout_count = 0;
        backoff_index = 0;
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
            const LoginResult again = auth.Login(p, ctx);
            if (again.success && !again.terminal_online && VerifyOnline()) {
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
        // 网络变更事件在主线程消费：重置退避与停止标志
        //（网络环境变了，之前的失败计数不再有参考价值）
        if (network_changed.exchange(false)) {
            stop_retrying = false;
            total_auth_attempts = 0;
            consecutive_auth_failures = 0;
            backoff_index = 0;
            inuse_logout_count = 0;   // 换了网络环境，"注销无效"的旧结论不再成立
            target_resolved = false;
        }
        if (probe_immediately.load() || Clock::now() >= next_probe) {
            probe_immediately.store(false);
            DoProbe();
        }
    }
};

void Scheduler::Impl::DoProbe() {
    const bool was_stopped = stop_retrying;
    const RunState stopped_state = state;

    state = RunState::Probing;
    last_probe = NowUnix();

    ProbeResult pr = ProbeOnce(cfg.settings);
    last_probe_response = pr.response;

    // 探测结论只在**变化时**记 INFO：出问题时能一眼看出是"根本没网"还是"被劫持"。
    // 2026-10-26 的事故就是缺这一行——日志里只有"网络变更事件"和"连接恢复"，
    // 中间 8 分钟既没网也连不上门户，事后完全看不出当时探测到的是什么。
    if ((int)pr.state != last_logged_probe_state) {
        last_logged_probe_state = (int)pr.state;
        // 顺带记下「到门户时本机长什么样」：没有地址 / 链路本地 / 有地址但网关不通，
        // 这三种在日志里必须能区分开，否则"连不上"仍然是个黑盒。
        const Profile* active = ActiveProfile(cfg);
        const std::string portal_url =
            (active && !active->auth_url.empty()) ? active->auth_url : cfg.settings.probe_url;
        LogInfo(std::string("探测结果：") + NetStateName(pr.state) + "（" +
                RedactSecrets(pr.message) + "）｜" + DescribeRoute(RouteForUrl(portal_url)));
    }

    if (pr.state == NetState::Online) {
        state = RunState::Online;
        message = "网络正常";
        ever_succeeded = true;
        backoff_index = 0;
        consecutive_auth_failures = 0;
        inuse_logout_count = 0;
        // 网络已恢复（可能是用户手动登录成功）：解除停止，恢复正常调度
        stop_retrying = false;
        total_auth_attempts = 0;
        ScheduleNext(cfg.settings.probe_interval_seconds);
        ResetCache();
        return;
    }

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

    // 从门户页面里取运营商与探测地址（仅首次或页面变化时）
    if (!target_resolved) {
        std::vector<std::string> blacklist;
        if (ParseVisitBlacklist(cached_portal_body, blacklist)) {
            ProbeTarget t;
            if (SelectProbeTarget(blacklist, t)) {
                target = t;
                target_resolved = true;
                LogInfo("采用服务器下发的探测地址：" + t.url);
            }
        }
    }

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
}

Scheduler::~Scheduler() = default;

void Scheduler::Tick() { impl_->Tick(); }

void Scheduler::ForceReconnect() {
    impl_->probe_immediately.store(true);
    impl_->stop_retrying = false;
    impl_->total_auth_attempts = 0;
    impl_->consecutive_auth_failures = 0;
    impl_->backoff_index = 0;
    impl_->target_resolved = false;
}

void Scheduler::SetConfig(const AppConfig& cfg) {
    impl_->cfg = cfg;
    impl_->stop_retrying = false;
    impl_->inuse_logout_count = 0;
    impl_->last_logout_unix = 0;
    impl_->total_auth_attempts = 0;
    impl_->consecutive_auth_failures = 0;
    impl_->backoff_index = 0;
    impl_->target_resolved = false;
    impl_->probe_immediately.store(true);
}

void Scheduler::NotifyNetworkChanged() {
    // 本方法可从网络监听线程调用：只允许写 atomic 标志，
    // 其余状态重置推迟到主线程的 Tick 中消费（避免数据竞争）
    impl_->network_changed.store(true);
    impl_->probe_immediately.store(true);
    LogInfo("收到网络变更事件，立即重新探测");
}

RunState Scheduler::State() const { return impl_->state; }
std::string Scheduler::LastMessage() const { return impl_->message; }
std::string Scheduler::LocalIp() const { return impl_->local_ip; }
long long Scheduler::LastSuccessUnix() const { return impl_->last_success; }
long long Scheduler::LastProbeUnix() const { return impl_->last_probe; }
int Scheduler::ConsecutiveAuthFailures() const {
    return impl_->consecutive_auth_failures;
}

std::string Scheduler::LastPortalBody() const {
    return impl_->cached_portal_body;
}

bool Scheduler::RunProbeNow(ProbeResult& out) {
    out = ProbeOnce(impl_->cfg.settings);
    // 未配置有效探测地址（Unknown）视为未完成探测
    return out.state != NetState::Unknown;
}

}  // namespace cnk
