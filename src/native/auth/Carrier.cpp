#include "auth/Carrier.h"
#include "json.hpp"

using json = nlohmann::json;

namespace cnk {

namespace {

// 字段取值：字符串原样返回；数值型（如 "defaultID":1）转字符串；其他类型返回空。
// 不能直接用 nlohmann 的 value()：类型不匹配会抛异常，而服务器下发数值型
// id 是合法变体，曾导致解析直接终止进程（审阅实测）。
std::string StrField(const json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

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

    // 解析与字段提取整体保护：任何 JSON 形态问题（非法语法、字段类型异常）
    // 都转为普通 false，绝不抛出（守护进程依赖本函数永不崩溃）
    try {
        json j = json::parse(literal);
        if (!j.is_object() || !j.contains("yys") || !j["yys"].is_object()) return false;

        const auto& yys = j["yys"];
        out.title = StrField(yys, "title");
        out.default_id = StrField(yys, "defaultID");
        if (out.default_id.empty()) out.default_id = StrField(yys, "defaultid");

        if (yys.contains("data") && yys["data"].is_array()) {
            for (const auto& item : yys["data"]) {
                if (!item.is_object()) continue;
                CarrierOption o;
                o.id = StrField(item, "id");
                o.name = StrField(item, "name");
                o.suffix = StrField(item, "suffix");
                out.options.push_back(o);
            }
        }
    } catch (const std::exception&) {
        out = CarrierList{};
        return false;
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
