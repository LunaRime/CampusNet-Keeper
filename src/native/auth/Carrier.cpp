#include "auth/Carrier.h"
#include "json.hpp"
#include <cstdlib>

using json = nlohmann::json;

namespace cnk {

namespace {

// 找到 carrier='...' 或 carrier="..." 的赋值，返回引号内的原文
bool ExtractCarrierLiteral(const std::string& body, std::string& out) {
    size_t pos = body.find("carrier");
    while (pos != std::string::npos) {
        size_t eq = body.find('=', pos + 7);
        if (eq == std::string::npos) return false;

        // 'carrier' 与 '=' 之间必须只有空白，防 carrierXXX= 型误命中
        bool between_ok = true;
        for (size_t k = pos + 7; k < eq; ++k) {
            if (body[k] != ' ' && body[k] != '\t') { between_ok = false; break; }
        }
        if (!between_ok) {
            pos = body.find("carrier", pos + 7);
            continue;
        }

        // 跳过 = 与引号之间的空白
        size_t i = eq + 1;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
        if (i >= body.size()) return false;
        char quote = body[i];
        if (quote != '\'' && quote != '"') { pos = body.find("carrier", pos + 7); continue; }
        size_t start = i + 1;
        size_t end = body.find(quote, start);
        if (end == std::string::npos) return false;
        out = body.substr(start, end - start);
        return true;
    }
    return false;
}

}  // namespace

bool ParseCarrier(const std::string& utf8_body, CarrierList& out) {
    out = CarrierList{};

    std::string literal;
    if (!ExtractCarrierLiteral(utf8_body, literal)) return false;

    json j;
    try {
        j = json::parse(literal);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object() || !j.contains("yys") || !j["yys"].is_object()) return false;

    const auto& yys = j["yys"];
    out.title = yys.value("title", std::string{});
    out.default_id = yys.value("defaultID", std::string{});
    if (out.default_id.empty() && yys.contains("defaultid"))
        out.default_id = yys.value("defaultid", std::string{});

    if (yys.contains("data") && yys["data"].is_array()) {
        for (const auto& item : yys["data"]) {
            if (!item.is_object()) continue;
            CarrierOption o;
            o.id = item.value("id", std::string{});
            o.name = item.value("name", std::string{});
            o.suffix = item.value("suffix", std::string{});
            out.options.push_back(o);
        }
    }
    return !out.options.empty();
}

int DefaultCarrierIndex(const CarrierList& list) {
    if (list.options.empty()) return -1;
    for (size_t i = 0; i < list.options.size(); ++i)
        if (list.options[i].id == list.default_id) return (int)i;
    return 0;
}

}  // namespace cnk
