#include "core/ProbePlan.h"

#include <algorithm>
#include <string>

namespace cnk {

namespace {

using Clock = std::chrono::steady_clock;

// 备用可用后沿用多久再回试主：短了每轮白等一次超时，长了主地址修好了也不回去
constexpr int kStickySeconds = 300;
// 备用至少能拿到的时间：主地址吃满预算时，别让它只有几十毫秒去连
constexpr int kMinCandidateMs = 1200;

int BudgetMs(const Settings& s) {
    int secs = s.probe_timeout_seconds;
    if (secs <= 0) secs = 3;
    if (secs > 600) secs = 600;
    return secs * 1000;
}

// 能连上并且给出了结论（在线，或明确被门户拦截）就算"可用"。
// 只有 Offline/Unknown 才算不可用 —— 这时候才值得去试备用。
bool Usable(NetState st) {
    return st == NetState::Online || st == NetState::PortalDetected;
}

ProbeAttempt RunCandidate(const Settings& s, const std::string& url, const std::string& expect,
                          int budget_ms, bool fallback) {
    ProbeAttempt a;
    a.url = url;
    a.fallback = fallback;
    const auto t0 = Clock::now();
    a.result = ProbeOnceAt(s, url, expect, budget_ms);
    a.elapsed_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - t0).count();
    return a;
}

const ProbeAttempt* LastOf(const ProbeOutcome& out, bool fallback) {
    for (auto it = out.attempts.rbegin(); it != out.attempts.rend(); ++it)
        if (it->fallback == fallback) return &*it;
    return nullptr;
}

// 主备都失败时，把两边的失败原因都说清楚（日志/界面要能区分
// 「已切备用」和「所有探测目标均不可用」）。
std::string AllFailedMessage(const ProbeOutcome& out) {
    const ProbeAttempt* p = LastOf(out, false);
    const ProbeAttempt* b = LastOf(out, true);
    if (p && b)
        return "所有探测目标均不可用（主：" + p->result.message +
               "；备用：" + b->result.message + "）";
    if (p) return "主探测地址不可用：" + p->result.message;
    if (b) return "备用探测地址不可用：" + b->result.message;
    return "没有可用的探测地址";
}

}  // namespace

ProbeTarget PickFallbackTarget(const std::string& primary_url) {
    // 顺序按本网实测可达性排；全部与主地址异域才有意义。
    //   detectportal.firefox.com  200/"success"  ~945ms
    //   msftncsi                  200/"Microsoft NCSI"（历史上曾 0/4 不可达，排后）
    //   hicloud generate_204      204（默认主地址就是它，通常会被异域检查跳过）
    static const struct {
        const char* url;
        const char* expect;
    } kCandidates[] = {
        {"http://detectportal.firefox.com/success.txt", "success"},
        {"http://www.msftncsi.com/ncsi.txt", "Microsoft NCSI"},
        {"http://connectivitycheck.platform.hicloud.com/generate_204", ""},
    };

    const std::string primary_host = HostOf(primary_url);
    for (const auto& c : kCandidates) {
        if (primary_host.empty()) return ProbeTarget{c.url, c.expect};
        if (HostOf(c.url) != primary_host) return ProbeTarget{c.url, c.expect};
    }
    return ProbeTarget{};
}

namespace {

ProbeOutcome RunWithFallback(const Settings& s, FallbackState& st, const ProbeTarget& fb) {
    ProbeOutcome out;
    const auto t0 = Clock::now();
    const int budget = BudgetMs(s);

    const auto spent = [&t0] {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - t0).count();
    };
    const auto remaining = [&] {
        return std::max(kMinCandidateMs, budget - spent());
    };
    const auto finish = [&] {
        out.total_ms = spent();
        return out;
    };

    // 压根没配主地址：这不是"主地址坏了"，不能拿备用地址替用户决定网络状态。
    // （否则清空 probe_url 的配置会神奇地"在线"—— 语义完全错位。）
    if (s.probe_url.empty()) {
        out.attempts.push_back(RunCandidate(s, std::string(), std::string(), budget, false));
        out.result = out.attempts.front().result;
        out.used_url.clear();
        return finish();
    }

    const bool have_fb = !fb.url.empty();
    const bool sticky = st.sticky && have_fb && Clock::now() < st.retry_primary_at;

    if (sticky) {
        // 沿用备用：先打它，不再每轮先等一次主地址超时
        ProbeAttempt b = RunCandidate(s, fb.url, fb.expect, budget, true);
        out.attempts.push_back(b);
        if (Usable(b.result.state)) {
            out.result = b.result;
            out.used_url = b.url;
            out.used_fallback = true;
            return finish();
        }

        // 备用也不行了：立刻回试主（可能已经恢复），别等 sticky 到期
        ProbeAttempt p = RunCandidate(s, s.probe_url, s.probe_expect, remaining(), false);
        out.attempts.push_back(p);
        if (Usable(p.result.state)) {
            st.Reset();
            out.result = p.result;
            out.used_url = p.url;
            out.primary_usable = true;
            return finish();
        }
        st.Reset();
        out.result = p.result;
        out.result.message = AllFailedMessage(out);
        out.used_url = p.url;
        out.all_failed = true;
        return finish();
    }

    // 正常路径：主地址优先
    ProbeAttempt p = RunCandidate(s, s.probe_url, s.probe_expect, budget, false);
    out.attempts.push_back(p);
    if (Usable(p.result.state)) {
        st.Reset();
        out.result = p.result;
        out.used_url = p.url;
        out.primary_usable = true;
        return finish();
    }

    // 主地址不可用（DNS/连接失败、超时、非门户的 4xx/5xx）→ 同一轮试备用。
    // 注意：主地址若是"明确门户"，上面就已经返回了，走不到这里 ——
    // 门户证据不能被备用地址的 204 覆盖成在线。
    if (have_fb) {
        ProbeAttempt b = RunCandidate(s, fb.url, fb.expect, remaining(), true);
        out.attempts.push_back(b);
        if (Usable(b.result.state)) {
            st.sticky = true;
            st.retry_primary_at = Clock::now() + std::chrono::seconds(kStickySeconds);
            out.result = b.result;
            out.used_url = b.url;
            out.used_fallback = true;
            return finish();
        }
    }

    // 两个都不行：以主地址的结论为准（它的失败原因更能代表"网络怎么了"），
    // 但 message 要说清是"所有目标都不可用"。
    out.result = p.result;
    out.result.message = have_fb ? AllFailedMessage(out) : ("主探测地址不可用：" + p.result.message);
    out.used_url = p.url;
    out.all_failed = true;
    return finish();
}

}  // namespace

ProbeOutcome ProbeWithFallback(const Settings& s, FallbackState& st) {
    return RunWithFallback(s, st, PickFallbackTarget(s.probe_url));
}

ProbeOutcome ProbeWithFallbackUsing(const Settings& s, FallbackState& st,
                                    const ProbeTarget& fallback) {
    return RunWithFallback(s, st, fallback);
}

ProbeOutcome ProbeSingle(const Settings& s, const std::string& url,
                         const std::string& expect) {
    ProbeOutcome out;
    const auto t0 = Clock::now();
    out.attempts.push_back(RunCandidate(s, url, expect, BudgetMs(s), false));
    out.result = out.attempts.front().result;
    out.used_url = url;
    out.total_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - t0).count();
    out.all_failed = !Usable(out.result.state);
    return out;
}

}  // namespace cnk
