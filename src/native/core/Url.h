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
std::string ResolveUrl(const std::string& action, const std::string& base);

}  // namespace cnk
