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

class Scheduler {
public:
    explicit Scheduler(AppConfig cfg);
    ~Scheduler();

    void Tick();
    void ForceReconnect();
    void SetConfig(const AppConfig& cfg);

    // 系统网络状态发生变化（地址变更、睡眠唤醒、网卡上下线）时调用。
    // 下一次 Tick 将立即探测，不等轮询周期。
    // 本方法可从任意线程调用（只写原子标志，主线程 Tick 中消费）。
    void NotifyNetworkChanged();

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

    // 最近一次探测取回的门户页面（已转 UTF-8）。
    // 供 IPC 的 get_carrier_options 解析服务器下发的运营商列表。
    std::string LastPortalBody() const;

    bool RunProbeNow(ProbeResult& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
