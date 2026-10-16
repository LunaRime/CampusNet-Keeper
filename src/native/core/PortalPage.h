#pragma once
#include <string>
#include "core/Http.h"
#include "core/Probe.h"

namespace cnk {

// 抓取门户页面（GET，不跟随重定向）。失败返回 false 并填 error。
// 不跟随是有意的：拿到的 3xx 也照常交给识别（部分门户存在装饰性跳转链）。
// timeout_ms 为单步超时；总时限沿用 HttpPerform 默认（max(2*step, 8s)）。
//
// 信任边界（审阅 P2-3 注记）：url 来自服务器下发的 Location，本函数对其直接
// 发 GET——有意为之（浏览器同样会跟随），但已加约束：不跟随重定向、不带任何
// 凭据（仅 UA / Content-Type / Cache-Control）、正文上限 4MB、总时限有界。
bool FetchPortalPage(const std::string& url, HttpResponse& out, std::string& error,
                     int timeout_ms = 8000);

// 「Portal 页面响应」的选择结果：识别（DetectVendor）与 NetContext 的输入。
struct PortalPageResult {
    HttpResponse response;    // 识别与 NetContext（portal_body / server_header）用
    std::string portal_url;   // resolve 后的重定向目标；无重定向时为空（语义同 spec §4.1）
    bool fetched = false;     // 是否成功抓取到重定向目标页
    std::string fetch_error;  // 抓取失败原因；未尝试或成功时为空
};

// spec §7：识别的输入是 Portal 的响应头与页面 HTML。劫持形态分两类：
//  - 3xx 重定向：30x 本身可能无指纹（本机实测 2026-10-16 为裸 302、空正文），
//    必须抓取 Location 目标页（a79.htm）后再识别；
//  - 200 篡改页：探测响应本身即 Portal 页面，直接使用；
// 目标页抓取失败时退回探测响应（尽力而为），原因记入 fetch_error，绝不阻断调用方。
PortalPageResult ResolvePortalPage(const ProbeResult& pr, const std::string& probe_url,
                                   int fetch_timeout_ms = 8000);

}  // namespace cnk
