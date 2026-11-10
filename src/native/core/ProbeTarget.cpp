#include "core/ProbeTarget.h"

namespace cnk {

namespace {

struct Candidate {
    const char* key;    // 在黑名单里需要出现的子串
    const char* url;
    const char* expect;
};

// 优先级顺序即数组顺序。排序依据是**本网实测**（2026-11-02，已认证态各测 2 次）：
//   captive.apple  200 / 134–204ms  ← 最快，且未认证时会给出可判读的劫持响应
//   detectportal   200 / ~945ms
//   msftncsi       200 / ~880ms     ← 历史上曾 0/4 不可达（M1 假失败根因），放最后
// 注意：清单只说明"这些地址免劫持"，**不保证可达**。所以 Scheduler 不拿它去改探测
// 地址（见 DoProbe 里的说明），只有 `--detect` 会给建议，且建议前会真的打一次。
//
// 2026-11-10：追加 router 端实证可用的 hicloud generate_204（204 / 0.038s / 无重定向）。
// 放在队尾是刻意的：Windows 端还没对这份清单里的 hicloud 条目做过耗时实测，
// 而 apple/firefox/msftncsi 的优先级有真机数据支撑；默认探测地址则由
// DefaultProbeTarget() 直接给 hicloud（那里是已实证的结论）。
const Candidate kCandidates[] = {
    {"captive.apple", "http://captive.apple.com/hotspot-detect.html",  "Success"},
    {"detectportal",  "http://detectportal.firefox.com/success.txt",   "success"},
    {"success.txt",   "http://detectportal.firefox.com/success.txt",   "success"},
    {"msftncsi",      "http://www.msftncsi.com/ncsi.txt",              "Microsoft NCSI"},
    {"ncsi.txt",      "http://www.msftncsi.com/ncsi.txt",              "Microsoft NCSI"},
    {"hicloud",       "http://connectivitycheck.platform.hicloud.com/generate_204", ""},
};

// 域名/标识符边界匹配：needle 必须位于串首或 '.'、'/' 之后，且后随 '.' 或串尾。
// 纯子串匹配会让 evil-msftncsi.com 误命中 msftncsi。
bool ListContains(const std::vector<std::string>& list, const std::string& needle) {
    for (const auto& s : list) {
        size_t p = s.find(needle);
        while (p != std::string::npos) {
            const bool left_ok = (p == 0) || (s[p - 1] == '.') || (s[p - 1] == '/');
            const size_t end = p + needle.size();
            const bool right_ok = (end == s.size()) || (s[end] == '.');
            if (left_ok && right_ok) return true;
            p = s.find(needle, p + 1);
        }
    }
    return false;
}

}  // namespace

bool ParseVisitBlacklist(const std::string& utf8_body,
                         std::vector<std::string>& out) {
    out.clear();
    static const char kKey[] = "visit_blacklist";
    size_t pos = utf8_body.find(kKey);
    if (pos == std::string::npos) return false;
    size_t lb = utf8_body.find('[', pos);
    if (lb == std::string::npos) return false;

    // 键与 '[' 之间只允许分隔符、引号与空白 —— 兼容两种真实形态：
    //   JSONP/JSON： "visit_blacklist":[ ... ]    （loadConfig 响应，实测证据）
    //   JS 赋值：    visit_blacklist=[ ... ]      （页面内嵌形式）
    for (size_t k = pos + sizeof(kKey) - 1; k < lb; ++k) {
        const char c = utf8_body[k];
        if (c != '"' && c != '\'' && c != ':' && c != '=' &&
            c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return false;
    }

    size_t rb = utf8_body.find(']', lb);
    if (rb == std::string::npos) return false;

    std::string inner = utf8_body.substr(lb + 1, rb - lb - 1);
    size_t i = 0;
    while (i < inner.size()) {
        size_t q1 = inner.find_first_of("\"'", i);
        if (q1 == std::string::npos) break;
        char quote = inner[q1];
        size_t q2 = inner.find(quote, q1 + 1);
        if (q2 == std::string::npos) break;
        out.push_back(inner.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return !out.empty();
}

bool SelectProbeTarget(const std::vector<std::string>& blacklist, ProbeTarget& out) {
    out = ProbeTarget{};  // 失败时不残留上一次的选中结果
    if (blacklist.empty()) return false;
    for (const auto& c : kCandidates) {
        if (ListContains(blacklist, c.key)) {
            out.url = c.url;
            out.expect = c.expect;
            return true;
        }
    }
    return false;
}

ProbeTarget DefaultProbeTarget() {
    // 与 store/Config.h 的 Settings 默认值保持一致（2026-11-10 迁移 router 端
    // 真机实证）：苹果探测页在本网被 DNS 劫持、且上游 DNS 就是 AC，
    // 200+Success 不能单独当在线证据；hicloud generate_204 实测 204 / 0.038s /
    // 无重定向。expect 留空：204 走三层判定，200 无标识时判 PortalDetected。
    return ProbeTarget{"http://connectivitycheck.platform.hicloud.com/generate_204", ""};
}

}  // namespace cnk
