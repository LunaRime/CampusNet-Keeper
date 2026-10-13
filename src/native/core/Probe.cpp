#include "core/Probe.h"
#include "core/Encoding.h"
#include "core/Url.h"

namespace cnk {

namespace {

// 取 URL 的主机部分（含端口）："http://host[:port]/path..." → "host[:port]"。
// 含端口比较是刻意的：Portal 与本机同 IP、不同端口时也必须判为异主机。
std::string HostOf(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    size_t start = scheme + 3;
    size_t end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos
                                                      : end - start);
}

}  // namespace

ProbeResult ProbeOnce(const Settings& s) {
    ProbeResult r;
    if (s.probe_url.empty()) {
        r.state = NetState::Unknown;
        r.message = "未配置探测地址";
        return r;
    }

    HttpRequest req;
    req.url = s.probe_url;
    req.method = "GET";
    req.timeout_ms = (s.probe_timeout_seconds > 0 ? s.probe_timeout_seconds : 3) * 1000;
    req.follow_redirects = false;

    std::string err;
    if (!HttpPerform(req, r.response, err)) {
        r.state = NetState::Offline;
        r.message = "探测失败：" + err;
        return r;
    }

    const int status = r.response.status;

    if (status == 204) {
        r.state = NetState::Online;
        r.message = "HTTP 204";
        return r;
    }

    if (status >= 300 && status < 400) {
        // Location 可能是**相对路径**（真实 Portal 常用 `/portal/login` 这种形式），
        // 必须先解析成绝对地址再比主机。否则相对跳转会让 HostOf 返回空，
        // 落进下面的分支判成 Offline —— 结果是永不触发登录、每轮空转重探，
        // 「劫持检测 → 自动登录」整链静默失效。
        const std::string loc_abs = ResolveUrl(r.response.location, s.probe_url);
        const std::string loc_host = HostOf(loc_abs);
        const std::string probe_host = HostOf(s.probe_url);

        if (!loc_host.empty() && loc_host != probe_host) {
            r.state = NetState::PortalDetected;
            r.message = "被重定向到 " + loc_abs;
            return r;
        }
        // 同主机重定向：能收到重定向本身就说明目标服务可达，网络是通的
        // （Portal 劫持的典型形态是重定向到内网 Portal 地址，即不同主机）。
        r.state = NetState::Online;
        r.message = "重定向但目标主机相同，判定为在线：" + loc_abs;
        return r;
    }

    if (status == 200) {
        const std::string body = DecodeBody(r.response);
        const bool has_expect = !s.probe_expect.empty() &&
                                body.find(s.probe_expect) != std::string::npos;
        const bool too_long = body.size() > 8192;
        if (has_expect && !too_long) {
            r.state = NetState::Online;
            r.message = "正文匹配预期标识";
            return r;
        }
        r.state = NetState::PortalDetected;
        r.message = has_expect ? "正文长度异常（" + std::to_string(body.size()) + " 字节）"
                               : "正文不含预期标识，疑似 Portal 劫持";
        return r;
    }

    r.state = NetState::PortalDetected;
    r.message = "HTTP " + std::to_string(status);
    return r;
}

}  // namespace cnk
