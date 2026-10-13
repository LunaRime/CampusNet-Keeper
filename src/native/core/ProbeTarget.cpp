#include "core/ProbeTarget.h"

namespace cnk {

namespace {

struct Candidate {
    const char* key;    // 在黑名单里需要出现的子串
    const char* url;
    const char* expect;
};

// 优先级顺序即数组顺序
const Candidate kCandidates[] = {
    {"msftncsi",      "http://www.msftncsi.com/ncsi.txt",              "Microsoft NCSI"},
    {"detectportal",  "http://detectportal.firefox.com/success.txt",   "success"},
    {"captive.apple", "http://captive.apple.com/hotspot-detect.html",  "Success"},
    {"success.txt",   "http://detectportal.firefox.com/success.txt",   "success"},
};

bool ListContains(const std::vector<std::string>& list, const std::string& needle) {
    for (const auto& s : list)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

bool ParseVisitBlacklist(const std::string& utf8_body,
                         std::vector<std::string>& out) {
    out.clear();
    size_t pos = utf8_body.find("visit_blacklist");
    if (pos == std::string::npos) return false;
    size_t eq = utf8_body.find('=', pos);
    if (eq == std::string::npos) return false;
    size_t lb = utf8_body.find('[', eq);
    if (lb == std::string::npos) return false;
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
    return ProbeTarget{"http://www.msftncsi.com/ncsi.txt", "Microsoft NCSI"};
}

}  // namespace cnk
