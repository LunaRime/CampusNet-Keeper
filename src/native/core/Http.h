#pragma once
#include <string>

namespace cnk {

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string content_type;
    std::string user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    // 毫秒。注意 WinHTTP 的超时粒度约 4 秒：配置值被向上取整到 4000 的倍数
    // （实测 <=4000 一律约 4.0 秒返回，5000 约 8.0 秒，10000 约 12.0 秒）。
    int timeout_ms = 3000;
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

    bool Ok() const { return status > 0 && error.empty(); }
};

bool HttpPerform(const HttpRequest& req, HttpResponse& out, std::string& error);

}  // namespace cnk
