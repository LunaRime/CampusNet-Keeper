#pragma once
#include <string>

namespace cnk {

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string content_type;
    std::string user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    // 单步超时（毫秒）。WinHTTP 的实际等待是粗粒度的：最小约 4 秒
    // （实测 500ms 配置也可能 3.77 秒才返回），而且**不是可靠上限** ——
    // 服务端稍晚回包仍可能被成功取回（实测 1000ms 配置、服务端 2500ms 才回包
    // 时，2515ms 成功取回）。整次请求的约束见 total_timeout_ms。
    int timeout_ms = 3000;

    // 整次请求总时限（毫秒），从 HttpPerform 入口起算。0 = 自动，取
    // max(2 × timeout_ms, 8000)。存在的理由：timeout_ms 管的是单次读操作，
    // 服务端"慢滴"（每隔一会儿挤一点字节）可一直拖到 4 MB 正文上限，
    // 调度线程就被无限期占住；有了总时限，超时即返回失败。
    // 注意这是检查点式的：只在每次 WinHTTP 调用前后检查，单次调用本身
    // 仍可能再阻塞数秒，故实际耗时可能略超本值。
    int total_timeout_ms = 0;

    // 探测场景需要观察 302 以判定劫持，故默认不跟随
    bool follow_redirects = false;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string server;
    std::string location;
    std::string content_type;
    std::string effective_url;
    std::string error;

    // 契约：HttpPerform 返回 false 时 status 恒为 0、error 非空，故只看 status
    // 也不会误判。正文只要不完整（读取失败或超出总时限）就绝不返回 true，
    // 此时 body 为空（不返回半截正文）。
    bool Ok() const { return status > 0 && error.empty(); }
};

bool HttpPerform(const HttpRequest& req, HttpResponse& out, std::string& error);

}  // namespace cnk
