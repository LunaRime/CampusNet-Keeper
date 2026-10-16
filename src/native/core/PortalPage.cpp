#include "core/PortalPage.h"
#include "core/Url.h"

namespace cnk {

bool FetchPortalPage(const std::string& url, HttpResponse& out, std::string& error,
                     int timeout_ms) {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.follow_redirects = false;
    req.timeout_ms = timeout_ms > 0 ? timeout_ms : 8000;
    return HttpPerform(req, out, error);
}

PortalPageResult ResolvePortalPage(const ProbeResult& pr, const std::string& probe_url,
                                   int fetch_timeout_ms) {
    PortalPageResult r;
    r.response = pr.response;

    // 仅劫持态且探测给出了重定向目标才抓取；其余形态（200 篡改页、在线、离线）
    // 探测响应本身即识别输入
    if (pr.state != NetState::PortalDetected || pr.response.location.empty())
        return r;

    // 与探测/调度一致的解析基准：Location 可能是相对路径（/a79.htm、//host/x）
    r.portal_url = ResolveUrl(pr.response.location, probe_url);

    HttpResponse page;
    std::string err;
    if (FetchPortalPage(r.portal_url, page, err, fetch_timeout_ms)) {
        r.response = std::move(page);
        r.fetched = true;
    } else {
        // 尽力而为：抓不到目标页就退回探测响应（可能识别为 unknown，退化为旧行为），
        // 绝不因一次抓取失败而阻断登录链路
        r.fetch_error = err;
    }
    return r;
}

}  // namespace cnk
