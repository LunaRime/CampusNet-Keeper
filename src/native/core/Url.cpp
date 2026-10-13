#include "core/Url.h"

namespace cnk {

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool SplitHostPort(const std::string& url, std::string& scheme_host,
                   std::string& port, const std::string& fallback_port) {
    scheme_host.clear();
    port.clear();
    if (url.empty()) return false;

    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;

    const size_t host_start = scheme_end + 3;
    const size_t path_start = url.find('/', host_start);
    const std::string authority = url.substr(
        host_start,
        path_start == std::string::npos ? std::string::npos : path_start - host_start);
    if (authority.empty()) return false;

    // authority = [userinfo@]host[:port]，host 可为 [IPv6 字面量]。
    // 先剥 userinfo（RFC 3986 中 '@' 不属于 userinfo 字符集，取最后一个即可）。
    std::string hostport = authority;
    const size_t at = hostport.rfind('@');
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    if (hostport.empty()) return false;

    std::string host, port_str;
    if (hostport.front() == '[') {
        // IPv6 字面量：冒号在括号内不构成端口分隔
        const size_t close = hostport.find(']');
        if (close == std::string::npos) return false;  // 未闭合 = 畸形
        host = hostport.substr(0, close + 1);
        if (close + 1 < hostport.size()) {
            if (hostport[close + 1] != ':') return false;
            port_str = hostport.substr(close + 2);
        }
    } else {
        const size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port_str = hostport.substr(colon + 1);
        } else {
            host = hostport;
        }
    }
    if (host.empty()) return false;

    scheme_host = url.substr(0, host_start) + host;
    port = port_str.empty() ? fallback_port : port_str;
    return true;
}

std::string ResolveUrl(const std::string& action, const std::string& base) {
    if (action.empty()) return base;
    if (action.rfind("http://", 0) == 0 || action.rfind("https://", 0) == 0)
        return action;

    // 取 base 的 origin
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return base + action;
    const size_t host_start = scheme_end + 3;
    const size_t path_start = base.find('/', host_start);
    const std::string origin = base.substr(
        0, path_start == std::string::npos ? base.size() : path_start);

    if (action[0] == '/') return origin + action;
    return origin + "/" + action;
}

}  // namespace cnk
