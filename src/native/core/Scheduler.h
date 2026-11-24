#pragma once
#include <string>
#include <memory>
#include <chrono>
#include "store/Config.h"
#include "core/Probe.h"

namespace cnk {

enum class RunState {
    Disconnected,
    Probing,
    PortalDetected,
    Online,
    Authenticating,
    AuthFailed,
    NotCampusNetwork,
};

const char* RunStateName(RunState s);

/// 一致性快照：调度器状态 + 对外展示字段的一次性拷贝。
///
/// 存在的理由（W2）：以前 IPC 线程和托盘各调一堆 getter，逐个去摸调度器内部
/// 的 std::string / int；而调度主线程正在 Tick 里改写它们 —— 那是数据竞争（UB）。
/// 现在写者只有调度线程，它在每轮 Tick 结束时**一次性发布**这份快照，
/// 读方（IPC / 托盘）只拿快照，永远看到自洽的一组值。
struct SchedulerSnapshot {
    RunState state = RunState::Disconnected;
    std::string message;
    std::string local_ip;
    std::string gateway;
    int gateway_arp = -1;
    long long last_success = 0;
    long long last_probe = 0;
    int consecutive_auth_failures = 0;
    int ineffective_logouts = 0;
    std::string portal_body;
    bool paused = false;

    // 链路四段的证据（W3）：1 = 本轮拿到了"通过"的证据，0 = 明确失败，
    // -1 = 没有证据。界面只允许把 1 显示成"成功"，-1 必须显示"未知" ——
    // 旧版四段共用一个总体状态，看起来"本机/网关/门户/互联网全绿"其实只是
    // 一个 state==online，出问题时完全看不出断在哪一层。
    int layer_local = -1;
    int layer_gateway = -1;
    int layer_portal = -1;
    int layer_internet = -1;
};

class Scheduler {
public:
    explicit Scheduler(AppConfig cfg);
    ~Scheduler();

    // ---- 线程模型（W2）----
    // Start() 起一个内部线程，它是**唯一**调用 Tick 的线程，也就是唯一的状态写者。
    // IPC 与托盘只能 Submit* 命令 + 读 Snapshot()，不得自己 Tick。
    // 未 Start() 时（单测、--once/--probe 之类的一次性命令）仍可手动 Tick，语义不变。
    void Start();
    void Stop();

    void Tick();

    // 命令：任意线程可调，由调度线程在下一轮 Tick 开头消费。
    void ForceReconnect();
    void SetConfig(const AppConfig& cfg);

    // 托盘「暂停/恢复自动重连」：暂停时仍探测、仍显示状态，只是不自动发起登录。
    // 用户主动点「立即重连」不受暂停影响（ForceReconnect 会放行一次）。
    // paused 是 atomic 控制位：写方立即生效，不需要等一个 Tick 才看见。
    void SetPaused(bool paused);
    bool IsPaused() const;

    // 系统网络状态发生变化（地址变更、睡眠唤醒、网卡上下线）时调用。
    // 本方法可从任意线程调用（只写原子标志，调度线程 Tick 中消费）。
    void NotifyNetworkChanged();

    /// 当前一致快照（一次加锁拷贝）。get_status 用它，避免拼出半新半旧的一组字段。
    SchedulerSnapshot Snapshot() const;

    RunState State() const;
    std::string LastMessage() const;
    // 本机到门户方向的源地址（探测时取，取不到为空）。
    std::string LocalIp() const;
    // 同一方向的下一跳与它的 ARP 状态（1=已解析 0=未解析 -1=未知）。
    // 与 local_ip 一起用于把「连不上」区分成二层断 / 三层通但认证坏。
    std::string Gateway() const;
    int GatewayArpState() const;
    long long LastSuccessUnix() const;
    long long LastProbeUnix() const;
    int ConsecutiveAuthFailures() const;
    // 连续无效的注销次数（诊断/回归用）：达到上限后调度器必须停手，
    // 且不得被网络变更通告清零。
    int IneffectiveLogoutCount() const;

    // 最近一次探测取回的门户页面（已转 UTF-8）。
    // 供 IPC 的 get_carrier_options 解析服务器下发的运营商列表。
    std::string LastPortalBody() const;

    bool RunProbeNow(ProbeResult& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
