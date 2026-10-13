#include "core/Template.h"

namespace cnk {

std::string ExpandTemplate(const std::string& tpl,
                           const std::map<std::string, std::string>& vars) {
    std::string out;
    out.reserve(tpl.size());
    size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] != '{') { out += tpl[i++]; continue; }
        size_t close = tpl.find('}', i + 1);
        if (close == std::string::npos) { out += tpl.substr(i); break; }
        std::string key = tpl.substr(i + 1, close - i - 1);
        auto it = vars.find(key);
        if (it != vars.end()) out += it->second;
        else out += tpl.substr(i, close - i + 1);  // 保留原样
        i = close + 1;
    }
    return out;
}

}  // namespace cnk
