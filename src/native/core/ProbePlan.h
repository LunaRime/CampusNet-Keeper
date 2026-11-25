#pragma once
#include <chrono>
#include <string>
#include <vector>
#include "core/Probe.h"
#include "core/ProbeTarget.h"
#include "store/Config.h"

namespace cnk {

/// 一次候选探测的记录：打了哪个地址、耗时、判定。诊断"到底是主地址坏了还是网坏了"
/// 全靠它，所以每个候选都留一条，不只留最终结果。
struct ProbeAttempt {
    std::string url;
    bool fallback = false;
    int elapsed_ms = 0;
    ProbeResult result;
};

/// 一轮"带回退的探测"的完整结果。
struct ProbeOutcome {
    ProbeResult result;              // 最终判定（含 response，供门户解析/厂商识别）
    std::string used_url;            // **实际**请求的地址（相对跳转必须按它解析）
    bool used_fallback = false;
    bool primary_usable = false;     // 主地址本轮是否给出了可用结论
    bool all_failed = false;         // 所有候选都不可用（日志/界面据此区分态）
    std::vector<ProbeAttempt> attempts;
    int total_ms = 0;                // 整轮耗时（所有候选之和）
};

/// 「备用地址暂时沿用」的运行时状态。
///
/// 为什么需要：主地址挂掉时，每轮都先试主就意味着**每轮先等一次超时**再去备用，
/// 恢复判断被拖慢一倍。这里记一个到期时间：期间直接用备用，到点再回试主。
///
/// ⚠ 只活在进程内存里，**绝不写回配置** —— 自动切换是运行时行为，
/// 用户看到的主地址不能被偷偷改掉。
struct FallbackState {
    bool sticky = false;
    std::chrono::steady_clock::time_point retry_primary_at{};

    void Reset() {
        sticky = false;
        retry_primary_at = std::chrono::steady_clock::time_point{};
    }
};

/// 挑一个与主地址**不同域名**的内置备用。
///
/// 必须异域：同域名只是把同一份证据打两遍，DNS 挂了两个一起挂，
/// 「备用可用」会变成假象。没有可用备选时返回空 url。
ProbeTarget PickFallbackTarget(const std::string& primary_url);

/// 带回退的一次探测。约定：
///   ① 主地址优先（sticky 未过期时除外）；
///   ② 主地址给出**明确结论**（Online 或 PortalDetected）就不再碰备用 ——
///      明确的门户证据不能被"备用地址回 204"覆盖成在线；
///   ③ 整轮共享一个时间预算，备用只拿剩余时间（不少于 1.2 秒），
///      不让两个候选各吃满一次 probe_timeout；
///   ④ 只探测，不登录、不注销、不写配置。
ProbeOutcome ProbeWithFallback(const Settings& s, FallbackState& st);

/// 同上，但用**指定**的备用目标（测试与「测试探测地址」用）。
/// 生产路径请用上面那个：它会按异域规则自己挑备用。
ProbeOutcome ProbeWithFallbackUsing(const Settings& s, FallbackState& st,
                                    const ProbeTarget& fallback);

/// 只测一个地址，不回退（「测试探测地址」里用户填了 url 时用它：
/// 用户测的是 A，就不能拿 B 的结果糊弄他）。
ProbeOutcome ProbeSingle(const Settings& s, const std::string& url,
                         const std::string& expect);

}  // namespace cnk
