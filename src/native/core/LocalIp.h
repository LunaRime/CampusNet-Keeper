#pragma once
#include <string>

namespace cnk {

// 取「访问 host:port 时本机会使用的 IPv4 源地址」。
//
// 实现方式：建一个 UDP socket 并 connect() 到目标（**不发任何包**），再 getsockname()
// 取本地端点。优点：
//   - 不需要管理员权限（不用原始套接字、不改路由）；
//   - 不需要枚举网卡，天然选对多网卡/多 IP/多网关的情形——取的正是路由会选的那个源地址；
//   - 目标不可达也无妨（UDP connect 只做路由与本地绑定，不握手）。
// 失败（host 为空、解析失败、无路由）返回空串。
std::string LocalIpFor(const std::string& host, int port);

// 同上，但直接吃一个认证网址（如 http://10.0.0.1/）：
// 主机与端口由 SplitHostPort 解析，未显式写端口时按 scheme 取 80/443。
// 解析失败返回空串。
std::string LocalIpForUrl(const std::string& url);

// 认证策略用：提交给服务器的客户端 IP（如 wlan_user_ip / ip 字段）。
// 顺序：上层给的 ctx_local_ip → 按 auth_url 现算 → "0.0.0.0" 兜底。
// 之所以需要现算：M1 的调度器尚未给 NetContext.local_ip 赋值（历史 TODO），
// 若直接取空值，锐捷/深澜会提交 0.0.0.0 而被服务器拒绝。
std::string ResolveClientIp(const std::string& ctx_local_ip, const std::string& auth_url);

// 「到某个目标时本机长什么样」——把"连不上"细分成可判读的几种原因。
// 2026-10-26 的真实现场：程序与浏览器都打不开门户，日志只有一句超时（12002），
// 事后无法区分"压根没拿到地址"和"有地址但网关不通"。本结构就是为这个区分而存在。
struct LocalRoute {
    std::string ip;       // 本机会用的源地址；空 = 没有到该目标的路由
    std::string gateway;  // 下一跳；空 = 直连（或没有默认路由）
};

// 按认证网址（或任意 http(s) 地址）取上面两个字段；解析失败时字段为空。
LocalRoute RouteForUrl(const std::string& url);

// 一行诊断文案，含 169.254 链路本地 / 缺地址的判读，直接进日志。
std::string DescribeRoute(const LocalRoute& r);

}  // namespace cnk
