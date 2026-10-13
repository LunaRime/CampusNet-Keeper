#pragma once
#include <string>
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

}  // namespace cnk
