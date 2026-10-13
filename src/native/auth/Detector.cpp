#include "auth/Detector.h"
#include "core/Encoding.h"
#include <algorithm>
#include <cctype>

namespace cnk {

namespace {

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool ContainsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    return it != hay.end();
}

}  // namespace

Vendor DetectVendor(const HttpResponse& resp) {
    const std::string body = DecodeBody(resp);
    const std::string& server = resp.server;
    const std::string& url = resp.effective_url;

    // spec §7 对部分指纹写「页面/URL 含」，两处都查（URL-only 场景曾漏判，见审阅）
    const auto inUrlOrBody = [&](const char* s) {
        return Contains(url, s) || Contains(body, s);
    };

    // 序 1：Server 头含 DrcomServer —— 最强信号
    if (ContainsCI(server, "drcomserver")) return Vendor::Drcom;

    // 序 2：哆点门户特征
    // 注意：不要用 epHTTPPort 做指纹 —— 该变量真实存在，但在首页引用的
    // a41.js 里，不在首页 HTML 内；客户端探测只拿得到 HTML，永远看不到它。
    // 同理 `duodian` 是拼音，真实页面写的是中文「哆点」。
    if (Contains(body, "Dr.COMWebLoginID_") || Contains(body, "0MKKey") ||
        Contains(body, "哆点"))
        return Vendor::Drcom;

    // 序 3：深澜
    if (inUrlOrBody("srun_portal") || inUrlOrBody("get_challenge") ||
        inUrlOrBody("srun_bx1") ||
        (Contains(body, "ac_id") && Contains(body, "callback")))
        return Vendor::Srun;

    // 序 4：锐捷 eportal
    if (Contains(url, "/eportal/") || ContainsCI(body, "c=Portal") ||
        (Contains(body, "wlanuserip") && Contains(body, "nasip")) ||
        Contains(body, "jsVersion"))
        return Vendor::Eportal;

    // 序 5：华为 iPortal。
    // "iPortal" 保持大小写敏感：ContainsCI 会把 multiportal 之类的子串误判为华为。
    if (inUrlOrBody("/portal/login") || Contains(body, "iPortal") ||
        Contains(body, "portalpage"))
        return Vendor::Huawei;

    return Vendor::Unknown;
}

const char* VendorName(Vendor v) {
    switch (v) {
        case Vendor::Drcom:   return "drcom";
        case Vendor::Eportal: return "eportal";
        case Vendor::Srun:    return "srun";
        case Vendor::Huawei:  return "huawei";
        case Vendor::Unknown: return "unknown";
    }
    return "unknown";
}

Vendor VendorFromString(const std::string& s) {
    if (s == "drcom")   return Vendor::Drcom;
    if (s == "eportal") return Vendor::Eportal;
    if (s == "srun")    return Vendor::Srun;
    if (s == "huawei")  return Vendor::Huawei;
    return Vendor::Unknown;
}

}  // namespace cnk
