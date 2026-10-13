#pragma once
#include <string>

namespace cnk {

// 百分号编码。保留 A-Z a-z 0-9 - _ . ~，空格编码为 %20（不用 +，
// 因为 + 在查询串中的含义依赖服务端实现，%20 语义无歧义）。
std::string UrlEncode(const std::string& s);

// 从完整 URL 拆出 "scheme://host" 与端口。
// url 未显式指定端口时，port 取 fallback_port。
// 空串或无法解析返回 false。
bool SplitHostPort(const std::string& url, std::string& scheme_host,
                   std::string& port, const std::string& fallback_port);

// 把 action 解析为绝对 URL：
//   - action 已是 http(s):// 开头 → 原样返回
//   - action 以 / 开头 → 拼到 base 的 origin 之后
//   - 其他 → 视为相对路径，拼到 base 的 origin 之后并补 /
//   - action 为空 → 返回 base
//
// ⚠ 已知限制（任务 5 审阅，2026-10-13）：对**路径相对**的 action（不以 / 开头，
// 如 "doLogin.jsp"、"../x"）与 query-only 引用（"?c=1"），本函数按 base 的 origin
// 拼接，而非 RFC 3986 的「相对 base 目录」语义。base 含子路径时结果会丢目录前缀：
//   ResolveUrl("doLogin.jsp", "http://h/portal/index.html") → "http://h/doLogin.jsp"
//   （RFC 语义应为 "http://h/portal/doLogin.jsp"）
// 对本项目实测 Portal（跳转与表单 action 均为根相对 "/eportal/?..." 或绝对）结论正确；
// 使用方若拿到真正的 path-relative 值，必须自行按页面 URL 的目录解析（见计划任务 21 注记）。
std::string ResolveUrl(const std::string& action, const std::string& base);

}  // namespace cnk
