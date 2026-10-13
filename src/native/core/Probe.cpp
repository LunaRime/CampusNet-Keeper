#include "core/Probe.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include <cctype>

namespace cnk {

namespace {

// 取 URL 的主机部分（含非默认端口），用于劫持判定的主机比较。
// 归一化保证等价写法不被误判为异主机：
//   - 剥离 userinfo（RFC 3986 中 '@' 不属于 userinfo 字符集，取最后一个即可）
//   - 剥离与 scheme 默认值相同的端口（host 与 host:80 视为同主机）
//   - 主机名统一小写（域名大小写不敏感）
// 含端口比较是刻意的：Portal 与本机同 IP、不同端口时也必须判为异主机。
std::string HostOf(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    size_t start = scheme + 3;
    size_t end = url.find('/', start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos
                                                                  : end - start);
    if (host.empty()) return {};

    size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);

    std::string scheme_str = url.substr(0, scheme);
    for (char& c : scheme_str) c = (char)std::tolower((unsigned char)c);
    size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find('[') == std::string::npos) {
        const std::string port = host.substr(colon + 1);
        if ((scheme_str == "http" && port == "80") ||
            (scheme_str == "https" && port == "443"))
            host = host.substr(0, colon);
    }

    for (char& c : host) c = (char)std::tolower((unsigned char)c);
    return host;
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
    // 夹取上限防 int 溢出（UI 可能被填入极大值）；<=0 走 3 秒兜底
    int secs = s.probe_timeout_seconds;
    if (secs <= 0) secs = 3;
    if (secs > 600) secs = 600;
    req.timeout_ms = secs * 1000;
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
        r.message = r.response.location.empty()
            ? "3xx 无 Location 头，按可达处理"
            : "重定向但目标主机相同，判定为在线：" + loc_abs;
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

    // 4xx/5xx 等其余状态：spec §6.1 未定义，取偏保守的 PortalDetected——
    // 宁可多试一次登录，也不把可疑响应当在线
    r.state = NetState::PortalDetected;
    r.message = "HTTP " + std::to_string(status);
    return r;
}

}  // namespace cnk
