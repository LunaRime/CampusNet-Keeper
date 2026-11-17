#pragma once
#include <string>
#include <vector>

namespace cnk {

struct ProbeTarget {
    std::string url;
    std::string expect;
};

// 从页面文本（UTF-8）中提取 visit_blacklist=[...]。找不到返回 false。
bool ParseVisitBlacklist(const std::string& utf8_body,
                         std::vector<std::string>& out);

// 按优先级从清单中选择探测地址。清单为空或无命中返回 false。
// 只从服务器声明的免劫持清单里选（spec §6.1），绝不回退到清单外地址。
// 优先级按本网实测可达性/时延排（captive.apple → detectportal → msftncsi），
// 但**清单不保证可达**：调用方拿到结果后应实打一次再采用（见 `--detect`）。
bool SelectProbeTarget(const std::vector<std::string>& blacklist, ProbeTarget& out);

// 内置默认探测地址（hicloud generate_204，probe_expect 为空）。
// 与 store/Config.h 的 Settings 默认值一致；captive.apple.com 在本网被 DNS 劫持，已弃用。
ProbeTarget DefaultProbeTarget();

}  // namespace cnk
