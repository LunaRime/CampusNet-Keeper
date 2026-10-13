#include "core/Encoding.h"
#include <windows.h>
#include <algorithm>
#include <cctype>

namespace cnk {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string Trim(std::string s) {
    auto notspace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

UINT CodePageFor(const std::string& charset) {
    std::string cs = ToLower(charset);
    if (cs.empty() || cs == "utf-8" || cs == "utf8") return CP_UTF8;
    if (cs == "gbk" || cs == "gb2312" || cs == "gb18030" ||
        cs == "cp936" || cs == "x-gbk") return 936;
    if (cs == "big5") return 950;
    if (cs == "iso-8859-1" || cs == "latin1") return 28591;
    return CP_UTF8;  // 未知编码按 UTF-8 兜底
}

}  // namespace

std::string ParseCharset(const std::string& content_type) {
    std::string lower = ToLower(content_type);
    size_t pos = lower.find("charset=");
    if (pos == std::string::npos) return {};
    std::string val = content_type.substr(pos + 8);
    val = Trim(val);
    if (!val.empty() && val.front() == '"') {
        size_t end = val.find('"', 1);
        val = val.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    } else {
        size_t end = val.find_first_of("; \t\r\n");
        if (end != std::string::npos) val = val.substr(0, end);
    }
    return ToLower(Trim(val));
}

std::string ToUtf8(const std::string& bytes, const std::string& charset) {
    if (bytes.empty()) return {};
    UINT cp = CodePageFor(charset);
    if (cp == CP_UTF8) return bytes;

    int n = MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(),
                                nullptr, 0);
    if (n <= 0) return bytes;  // 解码失败时原样返回，不崩溃
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(),
                        wide.data(), n);

    int m = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                                nullptr, 0, nullptr, nullptr);
    if (m <= 0) return bytes;
    std::string out(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                        out.data(), m, nullptr, nullptr);
    return out;
}

std::string DecodeBody(const HttpResponse& resp) {
    return ToUtf8(resp.body, ParseCharset(resp.content_type));
}

}  // namespace cnk
