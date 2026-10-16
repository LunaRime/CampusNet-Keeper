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

// 轮询探测：每轮调用 probe()，一旦判定为在线立即返回；否则等待 delay_ms 后重试，
// 用尽 attempts 次后返回最后一次结果（含 message，便于如实报告原因）。
//
// 存在的理由（2026-10-16 真机实测）：登录接口返回成功后，AC 的数据面放行存在秒级
// 延迟——紧跟登录的那一次复探会误报「仍不在线」（会话其实已经建立，稍后探测即
// Online）。故登录后的复探必须轮询，不能探一次就下结论。
//
// attempts < 1 视为 1；delay_ms <= 0 表示不等待（测试用）。
template <typename ProbeFn>
ProbeResult ProbeWithRetry(const ProbeFn& probe, int attempts, int delay_ms) {
    if (attempts < 1) attempts = 1;
    ProbeResult last;
    for (int i = 0; i < attempts; ++i) {
        last = probe();
        if (last.state == NetState::Online) return last;
        if (i + 1 < attempts && delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return last;
}

}  // namespace cnk
