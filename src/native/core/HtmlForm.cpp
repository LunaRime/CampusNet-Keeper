#include "core/HtmlForm.h"

#include <algorithm>
#include <cctype>

namespace cnk {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// 在 [from, end) 内不区分大小写地找 needle，返回绝对下标或 npos
size_t FindCI(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty() || from >= hay.size()) return std::string::npos;
    auto it = std::search(hay.begin() + (std::ptrdiff_t)from, hay.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    return it == hay.end() ? std::string::npos
                           : (size_t)std::distance(hay.begin(), it);
}

bool IsSpace(char c) { return std::isspace((unsigned char)c) != 0; }

// 把 HTML 注释（<!-- ... -->）替换成等长空格：注释里的旧表单/旧字段若被当成真表单，
// 凭据会被 POST 到废弃地址，或直接「无法识别字段名」把用户推向自定义模板。
// 用等长空格替换可保持后续所有偏移量不变。
std::string StripComments(const std::string& html) {
    std::string out = html;
    size_t i = 0;
    while ((i = out.find("<!--", i)) != std::string::npos) {
        const size_t end = out.find("-->", i + 4);
        if (end == std::string::npos) {
            for (size_t k = i; k < out.size(); ++k) out[k] = ' ';
            break;
        }
        for (size_t k = i; k < end + 3; ++k) out[k] = ' ';
        i = end + 3;
    }
    return out;
}

// 解 HTML 实体（只处理表单属性里常见的几个）。不解实体会让
// `action="/eportal/?c=Portal&amp;a=login"` 原样发出，参数名变成 `amp;a`。
std::string DecodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 8) {  // 不是实体（无分号或过长）
            out += s[i++];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        std::string rep;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos" || ent == "#39") rep = "'";
        else if (ent == "nbsp") rep = " ";
        else if (!ent.empty() && ent[0] == '#') {
            int code = 0;
            try {
                if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    code = std::stoi(ent.substr(2), nullptr, 16);
                else
                    code = std::stoi(ent.substr(1), nullptr, 10);
            } catch (...) {
                code = 0;
            }
            if (code > 0 && code < 0x80) {
                rep = std::string(1, (char)code);
            } else {
                out += s[i++];  // 非 ASCII 实体不认识，原样保留
                continue;
            }
        } else {
            out += s[i++];  // 未知实体原样保留
            continue;
        }
        out += rep;
        i = semi + 1;
    }
    return out;
}

// 从某个标签的文本里取属性值；取不到返回空串。
// 属性名必须落在边界上（前一个字符是 '<' 或空白），否则 "wlanuserip" 里的
// "userip"、或属性值内部的同名子串会被误取。取到的值解 HTML 实体。
std::string Attr(const std::string& tag, const std::string& name) {
    size_t pos = FindCI(tag, name, 0);
    while (pos != std::string::npos) {
        const bool boundary = pos == 0 || tag[pos - 1] == '<' || IsSpace(tag[pos - 1]);
        if (boundary) {
            size_t i = pos + name.size();
            // 属性名后必须紧跟可选的空白与 '='
            while (i < tag.size() && IsSpace(tag[i])) ++i;
            if (i < tag.size() && tag[i] == '=') {
                ++i;
                while (i < tag.size() && IsSpace(tag[i])) ++i;
                if (i >= tag.size()) return {};
                const char quote = tag[i];
                if (quote == '"' || quote == '\'') {
                    const size_t end = tag.find(quote, i + 1);
                    if (end == std::string::npos) return {};
                    return DecodeEntities(tag.substr(i + 1, end - i - 1));
                }
                size_t end = i;
                while (end < tag.size() && !IsSpace(tag[end]) && tag[end] != '>' &&
                       tag[end] != '/')
                    ++end;
                return DecodeEntities(tag.substr(i, end - i));
            }
        }
        pos = FindCI(tag, name, pos + 1);
    }
    return {};
}

// 只在 <select> 内取选中项：优先带 selected 的 <option>，否则第一个。
// value 取 option 的 value 属性；没有则取标签之间的文本（已去空白、解实体）。
std::string SelectValue(const std::string& body) {
    size_t i = 0;
    std::string first;
    while (true) {
        const size_t opt = FindCI(body, "<option", i);
        if (opt == std::string::npos) break;
        const size_t oe = body.find('>', opt);
        if (oe == std::string::npos) break;
        const std::string tag = body.substr(opt, oe - opt + 1);
        std::string value = Attr(tag, "value");
        if (value.empty()) {
            const size_t close = FindCI(body, "</option", oe);
            const std::string text = body.substr(
                oe + 1, (close == std::string::npos ? body.size() : close) - oe - 1);
            value = DecodeEntities(text);
            const size_t b = value.find_first_not_of(" \t\r\n");
            const size_t e = value.find_last_not_of(" \t\r\n");
            value = (b == std::string::npos) ? std::string{} : value.substr(b, e - b + 1);
        }
        if (FindCI(tag, "selected", 0) != std::string::npos) return value;
        if (first.empty()) first = value;
        i = oe + 1;
    }
    return first;
}

}  // namespace

bool ParseFirstForm(const std::string& html, HtmlForm& out) {
    out = HtmlForm{};

    // 注释里的旧表单不算表单
    const std::string src = StripComments(html);

    const size_t form_start = FindCI(src, "<form", 0);
    if (form_start == std::string::npos) return false;

    const size_t tag_end = src.find('>', form_start);
    if (tag_end == std::string::npos) return false;

    const std::string form_tag = src.substr(form_start, tag_end - form_start + 1);
    out.action = Attr(form_tag, "action");
    const std::string method = Attr(form_tag, "method");
    out.method = method.empty() ? "POST" : method;

    const size_t form_close = FindCI(src, "</form", tag_end);
    const size_t body_end = form_close == std::string::npos ? src.size() : form_close;

    size_t i = tag_end + 1;
    while (i < body_end) {
        const size_t inp = FindCI(src, "<input", i);
        const size_t sel = FindCI(src, "<select", i);
        size_t next = std::string::npos;
        bool is_select = false;
        if (inp != std::string::npos && inp < body_end) next = inp;
        if (sel != std::string::npos && sel < body_end &&
            (next == std::string::npos || sel < next)) {
            next = sel;
            is_select = true;
        }
        if (next == std::string::npos) break;

        if (is_select) {
            const size_t se = src.find('>', next);
            if (se == std::string::npos || se > body_end) break;
            const std::string tag = src.substr(next, se - next + 1);
            const size_t close = FindCI(src, "</select", se);
            const size_t inner_end =
                (close == std::string::npos || close > body_end) ? body_end : close;

            FormField f;
            f.name = Attr(tag, "name");
            f.type = "select";
            f.value = SelectValue(src.substr(se + 1, inner_end - se - 1));
            if (!f.name.empty()) out.fields.push_back(f);
            i = inner_end;
            continue;
        }

        const size_t ie = src.find('>', next);
        if (ie == std::string::npos || ie > body_end) break;

        const std::string tag = src.substr(next, ie - next + 1);
        FormField f;
        f.name = Attr(tag, "name");
        f.value = Attr(tag, "value");
        f.type = ToLower(Attr(tag, "type"));
        // 提交类控件只在被点击时才随表单提交，不是固定字段
        const bool submit_like = f.type == "submit" || f.type == "button" ||
                                 f.type == "reset" || f.type == "image";
        if (!f.name.empty() && !submit_like) out.fields.push_back(f);

        i = ie + 1;
    }

    return true;
}

std::string FindFieldByNames(const HtmlForm& form,
                             const std::vector<std::string>& candidates) {
    // hidden 字段一律不参与「账号/密码字段」的匹配（值仍会照原样提交）：
    // 否则 hidden 的 `user`/`wlanuserip` 会抢走账号字段，凭据被填进隐藏字段、
    // 可见字段提交为空，服务端看到空账号而静默失败。
    const auto usable = [](const FormField& f) { return f.type != "hidden"; };

    // 第一轮：按候选顺序精确匹配（不区分大小写）
    for (const auto& want : candidates) {
        for (const auto& f : form.fields) {
            if (usable(f) && ToLower(f.name) == ToLower(want)) return f.name;
        }
    }
    // 第二轮：包含匹配，但**只用足够长的候选词**避免短词截胡
    //（`user` 会命中 `userip`、`pass` 会命中 `pass_count`）
    for (const auto& want : candidates) {
        if (want.size() < 5) continue;
        const std::string w = ToLower(want);
        for (const auto& f : form.fields) {
            if (usable(f) && ToLower(f.name).find(w) != std::string::npos) return f.name;
        }
    }
    return {};
}

}  // namespace cnk
