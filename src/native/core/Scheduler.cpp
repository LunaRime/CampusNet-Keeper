#include "core/Scheduler.h"
#include "core/Log.h"
#include "core/Encoding.h"
#include "core/Url.h"
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

    Clock::time_point next_probe{};
    // 由网络监听线程置位、主线程在 Tick 中消费。
    // 跨线程通信只允许经由这两个 atomic（网络线程不得触碰普通成员，防数据竞争）。
    std::atomic<bool> probe_immediately{true};
    std::atomic<bool> network_changed{false};

    std::string local_ip;  // TODO：待接入网卡信息查询（GetAdaptersAddresses）后填充
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

    void Tick() {
        // 网络变更事件在主线程消费：重置退避与停止标志
        //（网络环境变了，之前的失败计数不再有参考价值）
        if (network_changed.exchange(false)) {
            stop_retrying = false;
            total_auth_attempts = 0;
            consecutive_auth_failures = 0;
            backoff_index = 0;
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

    if (pr.state == NetState::Online) {
        state = RunState::Online;
        message = "网络正常";
        ever_succeeded = true;
        backoff_index = 0;
        consecutive_auth_failures = 0;
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
    // portal_url 的语义是「探测阶段发现的重定向目标」（spec §4.1）：
    // 3xx 劫持时是 resolve 后的 Location；200 篡改时没有重定向目标，
    // 留空让下游（如通用表单认证）退回用户配置的 auth_url——
    // 绝不能填探测地址本身（那是外部地址，会让下游拼出错误的登录 URL）
    cached_portal_url = pr.response.location.empty()
        ? std::string{}
        : ResolveUrl(pr.response.location, cfg.settings.probe_url);
    cached_server_header = pr.response.server;
    cached_portal_body = DecodeBody(pr.response);

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

    NetContext ctx;
    ctx.local_ip = local_ip;
    ctx.portal_url = cached_portal_url;
    ctx.portal_body = cached_portal_body;
    ctx.server_header = cached_server_header;

    auto auth = CreateAuth(detected, *p);
    LogInfo(std::string("尝试登录，策略=") + auth->Name() +
            "，识别厂商=" + VendorName(detected));

    LoginResult lr = auth->Login(*p, ctx);

    LogInfo(std::string("登录结果：") + (lr.success ? "成功" : "失败") +
            "，信息=" + RedactSecrets(lr.message));
    LogInfo("请求地址：" + RedactSecrets(lr.request_url));

    if (lr.success) {
        state = RunState::Online;
        message = "已重新登录";
        last_success = NowUnix();
        ever_succeeded = true;
        consecutive_auth_failures = 0;
        backoff_index = 0;
        ScheduleNext(cfg.settings.probe_interval_seconds);
        ResetCache();
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
