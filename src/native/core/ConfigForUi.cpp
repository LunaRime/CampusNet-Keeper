#include "core/ConfigForUi.h"

#include "json.hpp"

namespace cnk {

std::string ConfigForUiJson(const AppConfig& cfg) {
    std::string js;
    if (!ConfigToJson(cfg, js)) return "{}";  // 加密失败也不能把密文漏给 UI

    nlohmann::json j = nlohmann::json::parse(js, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "{}";

    if (j.contains("profiles") && j["profiles"].is_array()) {
        for (auto& p : j["profiles"]) {
            if (!p.is_object()) continue;
            const bool has = !p.value("password_enc", std::string{}).empty();
            p.erase("password_enc");   // ← 安全红线：密文绝不外传
            p["has_password"] = has;
        }
    }
    return j.dump();
}

bool MergeUiConfig(const std::string& ui_json, const AppConfig& existing, AppConfig& out,
                   std::string& error) {
    error.clear();
    out = AppConfig{};

    const nlohmann::json j = nlohmann::json::parse(ui_json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "配置不是合法 JSON 对象";
        return false;
    }
    if (!j.contains("profiles") || !j["profiles"].is_array()) {
        error = "配置缺少 profiles 数组";
        return false;
    }

    std::string perr;
    if (!ConfigFromJson(ui_json, out, perr)) {
        error = "配置格式错误：" + perr;
        return false;
    }

    for (size_t i = 0; i < out.profiles.size() && i < j["profiles"].size(); ++i) {
        const auto& pj = j["profiles"][i];
        if (!pj.is_object()) continue;

        const std::string fresh = pj.value("password_plain", std::string{});
        if (!fresh.empty()) {
            out.profiles[i].password = fresh;   // 用户输入了新密码
            out.profiles[i].has_password = true;
            continue;
        }

        // 没带新密码：按档案名沿用旧密码，避免"保存一下就把密码清掉"
        for (const auto& old : existing.profiles) {
            if (old.name == out.profiles[i].name) {
                out.profiles[i].password = old.password;
                out.profiles[i].has_password = old.has_password;
                break;
            }
        }
    }
    return true;
}

}  // namespace cnk
