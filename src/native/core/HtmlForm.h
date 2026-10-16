#pragma once
#include <string>
#include <vector>

namespace cnk {

struct FormField {
    std::string name;
    std::string value;
    std::string type;  // <input type=...>（小写）；<select> 为 "select"
};

struct HtmlForm {
    std::string action;
    std::string method = "POST";
    std::vector<FormField> fields;
};

// 解析页面中第一个 <form>，收集其 <input> 与 <select> 字段。
// 只做「够用」的解析：不做完整 HTML 解析、不处理嵌套表单、不管实体转义。
// 无 <form> 返回 false（此时 out 为默认值）。
bool ParseFirstForm(const std::string& utf8_html, HtmlForm& out);

// 按候选名列表匹配字段名（大小写不敏感），返回**表单里的真实字段名**，未命中返回空串。
// 两轮：先精确匹配（按候选顺序），再放宽为包含匹配——第二轮会跳过 hidden 字段，
// 避免把 wlanuserip 这类隐藏字段误当账号字段（那会把凭据填进错误的字段）。
std::string FindFieldByNames(const HtmlForm& form,
                             const std::vector<std::string>& candidates);

}  // namespace cnk
