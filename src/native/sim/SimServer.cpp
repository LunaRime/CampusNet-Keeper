#include "sim/SimServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // MultiByteToWideChar；必须排在 winsock2.h 之后，否则会引入 winsock1

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace cnk {

namespace {

const char* kDrcomHome =
    "<html><head><meta charset=\"gb2312\"><!--Dr.COMWebLoginID_1.htm--></head>"
    "<body><script>"
    "epHTTPPort=801;"
    "authloginport=801;"
    "authloginpath='/eportal/?c=ACSetting&a=Login';"
    "authloginparam='url=drappall';"
    "authuserfield='DDDDD';"
    "authpassfield='upass';"
    "authsuccess='Dr.COMWebLoginID_3.htm';"
    "authfail='Dr.COMWebLoginID_2.htm';"
    "carrier='{\"yys\":{\"title\":\"服务类型\",\"mode\":\"radiobutton\",\"type\":\"0\","
    "\"data\":[{\"id\":\"1\",\"name\":\"校园用户\",\"suffix\":\"\"},"
    "{\"id\":\"2\",\"name\":\"校园电信\",\"suffix\":\"@dx\"},"
    "{\"id\":\"3\",\"name\":\"校园联通\",\"suffix\":\"@lt\"},"
    "{\"id\":\"4\",\"name\":\"校园其他\",\"suffix\":\"\"}],\"defaultID\":\"1\"}}';"
    "visit_blacklist=[\"www.msftncsi.com\",\"detectportal.firefox.com\","
    "\"captive.apple.com\",\"generate_204\",\"ncsi.txt\"];"
    "</script></body></html>";

// accept 轮询间隔：决定 Stop() 的最坏唤醒延迟。
constexpr int kAcceptPollMs = 100;
// 单个客户端两次 recv 之间的最长等待：防止连上不说话的客户端卡死服务线程。
constexpr int kClientRecvTimeoutMs = 5000;
// fail_mode==2 的挂起时长，按 kHangStepMs 分片轮询，使 Stop() 不必等满全程。
constexpr int kHangTotalMs = 10000;
constexpr int kHangStepMs = 100;

std::string ParseFormValue(const std::string& body, const std::string& key) {
    std::string needle = key + "=";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        bool at_boundary = (pos == 0) || body[pos - 1] == '&';
        if (at_boundary) {
            size_t start = pos + needle.size();
            size_t end = body.find('&', start);
            if (end == std::string::npos) end = body.size();
            return body.substr(start, end - start);
        }
        pos += needle.size();
    }
    return {};
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { out += (char)(h * 16 + l); i += 2; continue; }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

// 源码字面量经 /utf-8 编译是 UTF-8，而真实门户首页按 GBK 发送，
// 故发送前转码（UTF-8 → UTF-16 → GBK/936）。转码失败则原样返回，不让基础设施吞掉正文。
std::string Utf8ToGbk(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring wide((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), wn);
    int gn = WideCharToMultiByte(936, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
    if (gn <= 0) return utf8;
    std::string gbk((size_t)gn, '\0');
    WideCharToMultiByte(936, 0, wide.data(), wn, gbk.data(), gn, nullptr, nullptr);
    return gbk;
}

// loopback 上小响应通常一次就发完，循环发送以免响应被截断。
void SendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.c_str() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

}  // namespace

struct SimServer::Impl {
    SOCKET listen_sock = INVALID_SOCKET;
    std::thread worker;
    std::atomic<bool> running{false};
    int port = 0;
    SimOptions opts;
    std::atomic<int> login_count{0};
    std::mutex body_mutex;
    std::string last_login_body;

    void HandleClient(SOCKET client);
    void ServeLoop();
};

void SimServer::Impl::HandleClient(SOCKET client) {
    std::string request;
    char buf[4096];
    int n;
    while ((n = recv(client, buf, sizeof(buf), 0)) > 0) {
        request.append(buf, n);
        size_t hdr_end = request.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            // 读取 Content-Length 指定的正文
            std::string headers = request.substr(0, hdr_end);
            size_t cl_pos = headers.find("Content-Length:");
            if (cl_pos == std::string::npos)
                cl_pos = headers.find("content-length:");
            size_t want = 0;
            if (cl_pos != std::string::npos) {
                size_t vstart = headers.find(':', cl_pos) + 1;
                want = (size_t)std::strtoul(headers.c_str() + vstart, nullptr, 10);
            }
            if (request.size() >= hdr_end + 4 + want) break;
        }
        if (request.size() > 65536) break;
    }
    // recv 出错（含 SO_RCVTIMEO 超时）或对端关闭、报文已收全时都会走到这里。
    if (request.empty()) { closesocket(client); return; }

    size_t sp1 = request.find(' ');
    size_t sp2 = request.find(' ', sp1 + 1);
    std::string method = request.substr(0, sp1);
    std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string body;
    size_t hdr_end = request.find("\r\n\r\n");
    if (hdr_end != std::string::npos) body = request.substr(hdr_end + 4);

    std::string path = target;
    size_t q = path.find('?');
    std::string query = q == std::string::npos ? "" : path.substr(q + 1);
    if (q != std::string::npos) path = path.substr(0, q);

    std::string resp_body;
    std::string content_type = "text/html; charset=gbk";
    int status = 200;

    if (opts.fail_mode == 2) {
        // 挂起：不响应，让客户端超时。分片睡眠，Stop() 时能提前退出。
        for (int waited = 0; waited < kHangTotalMs && running.load();
             waited += kHangStepMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kHangStepMs));
        }
        closesocket(client);
        return;
    }

    if (path == "/") {
        // 真实门户首页是 GBK 字节；转码后再算 Content-Length（GBK 与 UTF-8 字节数不同）
        resp_body = Utf8ToGbk(kDrcomHome);
    } else if (path == "/eportal/" || path == "/eportal") {
        login_count++;
        // 凭据可能来自 query（GET）或 body（POST）
        std::string all = query + "&" + body;
        std::string user = UrlDecode(ParseFormValue(all, "DDDDD"));
        std::string pass = UrlDecode(ParseFormValue(all, "upass"));
        {
            std::lock_guard<std::mutex> lk(body_mutex);
            last_login_body = all;
        }
        bool user_ok = !opts.valid_user.empty() && user == opts.valid_user + opts.isp_suffix;
        bool pass_ok = !opts.valid_pass.empty() && pass == opts.valid_pass;
        if (opts.fail_mode == 1 || !user_ok || !pass_ok) {
            resp_body = "<html><body><!--Dr.COMWebLoginID_2.htm-->login failed</body></html>";
        } else {
            resp_body = "<html><body><!--Dr.COMWebLoginID_3.htm-->"
                        "window.location='3.htm';</body></html>";
        }
    } else if (path == "/ncsi.txt") {
        resp_body = "Microsoft NCSI";
    } else if (path == "/success.txt") {
        resp_body = "success";
    } else {
        status = 404;
        resp_body = "not found";
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " OK\r\n"
        << "Server: DrcomServer1.0\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << resp_body.size() << "\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << resp_body;
    SendAll(client, oss.str());
    closesocket(client);
}

void SimServer::Impl::ServeLoop() {
    // 用带超时的 select 而不是阻塞 accept：Stop() 只需置 running=false，
    // 循环最多再等 kAcceptPollMs 就会退出，不依赖跨线程 closesocket 唤醒 accept。
    while (running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_sock, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = kAcceptPollMs * 1000;
        int rc = select(0, &rfds, nullptr, nullptr, &tv);
        if (!running.load()) break;
        if (rc <= 0) continue;  // 超时或出错：回到循环顶重新判断
        SOCKET client = accept(listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        int timeout = kClientRecvTimeoutMs;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        HandleClient(client);
    }
    if (listen_sock != INVALID_SOCKET) {
        closesocket(listen_sock);
        listen_sock = INVALID_SOCKET;
    }
}

SimServer::SimServer() : impl_(new Impl) {}
SimServer::~SimServer() { Stop(); }

bool SimServer::Start(const SimOptions& opts, std::string& error) {
    if (impl_->running.load()) {
        error = "already started";
        return false;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    impl_->opts = opts;
    impl_->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listen_sock == INVALID_SOCKET) {
        error = "socket() failed";
        return false;
    }

    int one = 1;
    setsockopt(impl_->listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 让系统分配
    if (bind(impl_->listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        error = "bind() failed: " + std::to_string(WSAGetLastError());
        closesocket(impl_->listen_sock);
        impl_->listen_sock = INVALID_SOCKET;
        return false;
    }
    int len = sizeof(addr);
    if (getsockname(impl_->listen_sock, (sockaddr*)&addr, &len) == SOCKET_ERROR) {
        error = "getsockname() failed";
        closesocket(impl_->listen_sock);
        impl_->listen_sock = INVALID_SOCKET;
        return false;
    }
    impl_->port = ntohs(addr.sin_port);

    if (listen(impl_->listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        error = "listen() failed";
        closesocket(impl_->listen_sock);
        impl_->listen_sock = INVALID_SOCKET;
        return false;
    }

    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->ServeLoop(); });
    return true;
}

void SimServer::Stop() {
    if (!impl_) return;
    impl_->running.store(false);
    if (impl_->worker.joinable()) impl_->worker.join();
}

int SimServer::Port() const { return impl_->port; }

std::string SimServer::BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(impl_->port);
}

std::string SimServer::LoginPath() const {
    return "/eportal/?c=ACSetting&a=Login&url=drappall";
}

int SimServer::LoginCount() const { return impl_->login_count.load(); }

std::string SimServer::LastLoginBody() const {
    std::lock_guard<std::mutex> lk(impl_->body_mutex);
    return impl_->last_login_body;
}

}  // namespace cnk
