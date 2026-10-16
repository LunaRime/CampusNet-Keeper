#pragma once
#include <string>
#include <chrono>
#include <thread>
#include "core/Http.h"
#include "store/Config.h"

namespace cnk {

enum class NetState { Unknown, Online, PortalDetected, Offline };

struct ProbeResult {
    NetState state = NetState::Unknown;
    HttpResponse response;
    std::string message;
};

// 执行一次探测并按 spec §6.1 的规则判定网络状态。
// 请求带 Cache-Control: no-cache（Http 层统一附加）；不跟随重定向——
// 3xx 的 Location 是判定劫持的关键证据。
ProbeResult ProbeOnce(const Settings& s);

// 通用轮询：每轮调用 probe()，一旦 stop(r) 为真立即返回；否则等待 delay_ms 后重试，
// 用尽 attempts 次后返回最后一次结果（含 message，便于如实报告原因）。
//
// 存在的理由（2026-10-16 真机实测）：本部署的网络判定有两处「单次探测不可信」——
//   ① 登录接口返回成功后，AC 数据面放行有秒级延迟 → 紧跟登录的复探误报「仍不在线」；
//   ② 会话正常时也曾出现单次探测超时（错误码 12002），紧随其后的探测即 Online。
// 故「等在线」与「决策前探测」都需要轮询，但停止条件不同，见下面两个包装。
//
// attempts < 1 视为 1；delay_ms <= 0 表示不等待（测试用）。
template <typename ProbeFn, typename StopFn>
ProbeResult ProbePoll(const ProbeFn& probe, const StopFn& stop, int attempts, int delay_ms) {
    if (attempts < 1) attempts = 1;
    ProbeResult last;
    for (int i = 0; i < attempts; ++i) {
        last = probe();
        if (stop(last)) return last;
        if (i + 1 < attempts && delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return last;
}

// 登录成功后的复探：等 AC 数据面真正放行，故轮询到「在线」为止。
template <typename ProbeFn>
ProbeResult ProbeUntilOnline(const ProbeFn& probe, int attempts, int delay_ms) {
    return ProbePoll(
        probe, [](const ProbeResult& r) { return r.state == NetState::Online; }, attempts,
        delay_ms);
}

// 决策前的探测（是否要登录）：只有「离线」是模糊态——它可能只是一次超时——才重试；
// Online / PortalDetected / Unknown 都是确定结论，立即返回（避免每个注销窗口都白等）。
template <typename ProbeFn>
ProbeResult ProbeUntilDecided(const ProbeFn& probe, int attempts, int delay_ms) {
    return ProbePoll(
        probe, [](const ProbeResult& r) { return r.state != NetState::Offline; }, attempts,
        delay_ms);
}

}  // namespace cnk
