#include "core/LocalIp.h"
#include "core/Log.h"
#include "core/Url.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <string>

namespace cnk {

namespace {

// 把 host:port 解析成 IPv4 的 sockaddr_in（不发包）。失败返回 false。
bool ResolveV4(const std::string& host, int port, sockaddr_in& out) {
    if (host.empty() || port <= 0 || port > 65535) return false;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    const std::string port_str = std::to_string(port);
    addrinfo* res = nullptr;
    bool ok = false;
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) == 0 && res) {
        if (res->ai_addr && res->ai_addrlen >= (int)sizeof(sockaddr_in)) {
            out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
            ok = true;
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return ok;
}

// 查网关的邻居（ARP）状态：1=已解析（可达），0=未解析（二层断，网关不代答 ARP），
// -1=查不到（无网关，或邻居表里没有这条，无从判断）。
// 对应 router 端 link_gw_status：只有"网关 ARP 已解析"才说明三层通，
// 否则「登录重试必然无效」——这正是 2026-11-01 那次二层故障的判据。
int GatewayArpState(const std::string& gateway) {
    if (gateway.empty()) return -1;

    in_addr gw{};
    if (inet_pton(AF_INET, gateway.c_str(), &gw) != 1) return -1;

    PMIB_IPNET_TABLE2 table = nullptr;
    if (GetIpNetTable2(AF_INET, &table) != NO_ERROR || !table) return -1;

    int result = -1;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPNET_ROW2& row = table->Table[i];
        if (row.Address.Ipv4.sin_addr.s_addr != gw.s_addr) continue;
        // 未解析：Incomplete（正在解析但没结果）/ Unreachable（明确不可达）
        result = (row.State == NlnsIncomplete || row.State == NlnsUnreachable) ? 0 : 1;
        break;
    }
    FreeMibTable(table);
    return result;
}

}  // namespace

std::string LocalIpFor(const std::string& host, int port) {
    if (host.empty() || port <= 0 || port > 65535) return {};

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return {};

    std::string result;
    addrinfo hints{};
    hints.ai_family = AF_INET;        // 校园网按 IPv4 认证（wlan_user_ip 是 v4 字段）
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) == 0 && res) {
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s != INVALID_SOCKET) {
            if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
                sockaddr_in local{};
                int len = (int)sizeof(local);
                if (getsockname(s, (sockaddr*)&local, &len) == 0 &&
                    local.sin_family == AF_INET) {
                    char buf[INET_ADDRSTRLEN] = {};
                    if (inet_ntop(AF_INET, &local.sin_addr, buf, (DWORD)sizeof(buf)))
                        result = buf;
                }
            }
            closesocket(s);
        }
        freeaddrinfo(res);
    }

    WSACleanup();
    return result;
}

std::string LocalIpForUrl(const std::string& url) {
    std::string scheme_host, port;
    if (!SplitHostPort(url, scheme_host, port, "")) return {};

    // 只取主机部分（SplitHostPort 返回的是 scheme://host）
    const size_t scheme_end = scheme_host.find("://");
    if (scheme_end == std::string::npos) return {};
    const std::string host = scheme_host.substr(scheme_end + 3);
    if (host.empty()) return {};

    int p = 0;
    if (!port.empty()) {
        try {
            p = std::stoi(port);
        } catch (...) {
            return {};
        }
    } else {
        p = scheme_host.compare(0, 5, "https") == 0 ? 443 : 80;
    }
    return LocalIpFor(host, p);
}

std::string ResolveClientIp(const std::string& ctx_local_ip,
                            const std::string& auth_url) {
    if (!ctx_local_ip.empty()) return ctx_local_ip;
    const std::string derived = LocalIpForUrl(auth_url);
    if (!derived.empty()) return derived;
    // 再退一步：完全不依赖 DNS —— 用默认路由的目的地探一次源地址
    //（auth_url 写域名、且认证前 DNS 不可用时会走到这里；总好过静默提交 0.0.0.0）
    const std::string via_route = LocalIpFor("1.1.1.1", 53);
    if (!via_route.empty()) {
        LogWarn("无法解析认证网址的主机名，改用默认路由的源地址 " + via_route +
                " 作为客户端 IP");
        return via_route;
    }
    return "0.0.0.0";
}

LocalRoute RouteForUrl(const std::string& url) {
    LocalRoute r;
    r.ip = LocalIpForUrl(url);

    // 目标地址：复用 SplitHostPort 的解析口径（与 LocalIpForUrl 一致）
    std::string scheme_host, port_str;
    if (!SplitHostPort(url, scheme_host, port_str, "")) return r;
    const size_t scheme_end = scheme_host.find("://");
    if (scheme_end == std::string::npos) return r;
    const std::string host = scheme_host.substr(scheme_end + 3);
    if (host.empty()) return r;

    int port = 0;
    if (!port_str.empty()) {
        try {
            port = std::stoi(port_str);
        } catch (...) {
            return r;
        }
    } else {
        port = scheme_host.compare(0, 5, "https") == 0 ? 443 : 80;
    }

    sockaddr_in dest{};
    if (!ResolveV4(host, port, dest)) return r;

    // 问系统：去这个目标会走哪条路由、下一跳是谁。
    // GetBestRoute 已废弃但仍可用（不需要管理员权限，也不会触发流量）。
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(dest.sin_addr.s_addr, 0, &row) == NO_ERROR) {
        in_addr gw{};
        gw.s_addr = row.dwForwardNextHop;
        if (gw.s_addr != 0) {
            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &gw, buf, (DWORD)sizeof buf)) r.gateway = buf;
        }
    }
    r.gateway_arp = GatewayArpState(r.gateway);
    return r;
}

std::string DescribeRoute(const LocalRoute& r) {
    std::string s;
    if (r.ip.empty()) {
        // 没有到门户的路由：没插网线、网卡没拿到地址、或地址与门户不同网段且无网关
        return "本机地址=无（没有到门户的路由）";
    }
    if (r.ip.rfind("169.254.", 0) == 0) {
        s = "本机地址=" + r.ip + "（链路本地 169.254/16：没拿到 DHCP 地址）";
    } else if (r.ip.rfind("127.", 0) == 0) {
        s = "本机地址=" + r.ip + "（回环）";
    } else {
        s = "本机地址=" + r.ip;
    }
    s += r.gateway.empty() ? "，网关=直连（无下一跳）" : "，网关=" + r.gateway;
    if (!r.gateway.empty()) {
        if (r.gateway_arp == 1)
            s += "（ARP 已解析）";
        else if (r.gateway_arp == 0)
            s += "（ARP 未解析：二层断，网关不代答）";
        else
            s += "（ARP 未知）";
    }
    return s;
}

}  // namespace cnk
