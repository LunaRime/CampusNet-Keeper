#include "core/LocalIp.h"
#include "core/Log.h"
#include "core/Url.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

namespace cnk {

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

}  // namespace cnk
