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

// 首页 JS 变量的取值逐字取自 docs/evidence/portal_home_gbk_raw.htm（UTF-8 转写后）。
// 端口是"不变量"而非字面值：真实门户上报 801 是因为它就在 801 上服务，模拟器监听随机
// 端口，就必须上报自己的实际端口，否则照配置连过来的客户端会连不上（测试里钉住这条）。
// 首页只放真实首页 HTML 里存在的变量：
//   - 免劫持清单 visit_blacklist 只在 loadConfig 响应里（首页 0 命中）
//   - epHTTPPort 只在首页引用的 a41.js 里（首页 HTML 0 命中），客户端探测拿不到，故不放
//
// 契约：首页只复刻真实页面中**客户端会读**的变量（逐个列在 tests/sim_test.cpp 的忠实性
// 字段清单里），其余变量（portalver / portalid / querypath 等）本就不复刻，故忠实性测试
// 只钉这些字段。将来谁往模拟器页面加变量，谁负责同步往 sim_test.cpp 的字段清单里加一条，
// 否则加了也不会被忠实性测试保护。
const char* kCarrierJson =
    "{\"yys\":{\"title\": \"服务类型\",\"mode\":\"radiobutton\",\"type\":\"0\","
    "\"data\":[{\"id\":\"1\",\"name\":\"校园用户\",\"suffix\":\"\"},"
    "{\"id\":\"2\",\"name\":\"校园电信\",\"suffix\":\"@dx\"},"
    "{\"id\":\"3\",\"name\":\"校园联通\",\"suffix\":\"@lt\"},"
    "{\"id\":\"4\",\"name\":\"校园其他\",\"suffix\":\"\"}],\"defaultID\":\"1\"}}";

std::string BuildHomePage(int port) {
    std::ostringstream oss;
    oss << "<html><head><meta charset=\"gb2312\"><!--Dr.COMWebLoginID_1.htm--></head>"
           "<body><script>"
           "/** 哆点参数 start */"
        << "authloginport=" << port << ";"
           "authloginpath='/eportal/?c=ACSetting&a=Login';"
           "authloginparam='url=drappall';"
           "authuserfield='DDDDD';"
           "authpassfield='upass';"
           "authsuccess='Dr.COMWebLoginID_3.htm';"
           "authfail='Dr.COMWebLoginID_2.htm';"
        << "carrier='" << kCarrierJson << "';"
           "</script></body></html>";
    return oss.str();
}

// 免劫持清单逐字取自 docs/evidence/loadconfig_185_801.json 的 visit_blacklist（15 项）。
const char* kVisitBlacklist =
    "[\"1.1.1.1\",\"www.msftncsi.com\",\"land.xiaomi.net\",\"detectportal.firefox.com\","
    "\"www.airport.us\",\"www.thinkdifferent.us\",\"www.ibook.info\",\"www.itools.info\","
    "\"www.appleiphonecell.com\",\"captive.apple.com\",\"www.apple.com\","
    "\"gspe21.ls.apple.com\",\"generate_204\",\"ncsi.txt\",\"success.txt\"]";

// GET /eportal/portal/page/loadConfig 的响应：dr1003({...}) 包裹的 JSON（真实格式）。
// 字段名与取值取自证据文件；字符串型字段必须保持字符串（en_md5 / ep_http_port 原件都是
// 字符串），否则会诱导下游写出"只在模拟器上正确"的解析代码。
std::string BuildLoadConfig(int port) {
    std::ostringstream oss;
    oss << "dr1003({\"code\":1,\"msg\":\"加载页面设置信息成功！\",\"data\":{"
        << "\"program_index\":\"GqrAs31700550965\","
           "\"page_index\":\"5Tl76r1700550989\","
           "\"page_name\":\"Page_231121\","
           "\"login_method\":\"1\","
           "\"account_suffix\":\"\","
           "\"account_prefix\":\"1\","
           "\"en_md5\":\"0\","
           "\"password_cut\":\"0\","
           "\"enable_r3\":\"0\","
           "\"enable_https\":\"0\","
           "\"change_pass_mode\":\"0\","
           "\"online_monitor\":\"1\","
           "\"isp_unbind_suffix\":\"1\","
           "\"find_mac\":\"0\","
           "\"un_bind_mac\":\"0\","
           "\"domain_name\":\"\","
           "\"cvlan_id\":\"4095\","
           "\"rcn\":\"SD0S0zk6\","
        << "\"ep_http_port\":\"" << port << "\","
           "\"ep_https_port\":\"802\","
        << "\"visit_blacklist\":" << kVisitBlacklist
        << "}});";
    return oss.str();
}

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
    std::atomic<bool> logged_in{false};   // 登录成功即建立会话状态
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

    if (opts.fail_mode == 3) {
        // 302 重定向：任何路径都跳走，模拟 Portal 劫持
        // （客户端靠"302 且 Location 指向别的主机"判定）。
        std::ostringstream oss;
        oss << "HTTP/1.1 302 Found\r\n"
            << "Server: DrcomServer1.0\r\n"
            << "Location: " << opts.redirect_location << "\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        SendAll(client, oss.str());
        closesocket(client);
        return;
    }

    if (path == "/") {
        // 真实门户首页是 GBK 字节；转码后再算 Content-Length（GBK 与 UTF-8 字节数不同）
        resp_body = Utf8ToGbk(BuildHomePage(port));
    } else if (path == "/eportal/portal/page/loadConfig") {
        // 免劫持清单只在这里下发，不在首页
        resp_body = BuildLoadConfig(port);
        content_type = "text/html; charset=utf-8";
    } else if (path == "/eportal/" || path == "/eportal") {
        // 真实 eportal 路径按 UTF-8 声明（与首页的 gbk 不同），硬编码任一种都会让另一条乱码
        content_type = "text/html; charset=utf-8";
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
        if (opts.fail_mode == 4) {
            // 失败时回显完整登录页：框架脚本同时含成功与失败两个标志
            //（真实登录页即有 authsuccess/authfail 两个变量定义）。
            // 客户端必须因此返回「无法判定」（-1）交探测裁决，而不是乐观判成功。
            resp_body = "<html><script>authsuccess='Dr.COMWebLoginID_3.htm';"
                        "authfail='Dr.COMWebLoginID_2.htm';</script>"
                        "<body>login failed</body></html>";
        } else if (opts.fail_mode == 1 || !user_ok || !pass_ok) {
            resp_body = "<html><body><!--Dr.COMWebLoginID_2.htm-->login failed</body></html>";
        } else {
            logged_in.store(true);   // 会话建立，/sim_probe 随之切换为正常应答
            resp_body = "<html><body><!--Dr.COMWebLoginID_3.htm-->"
                        "window.location='3.htm';</body></html>";
        }
    } else if (path == "/sim_probe") {
        // 端到端模拟专用：登录前相当于被劫持（返回门户页），登录成功后
        // 网络恢复（返回 nCSI 应答）——用于验证「探测 → 劫持 → 登录 → 恢复」整链
        if (logged_in.load()) {
            resp_body = "Microsoft NCSI";
        } else {
            resp_body = Utf8ToGbk(BuildHomePage(port));
        }
    } else if (path == "/generate_204") {
        // 探测地址类型（generate_204）：标准应答为 204 空体
        status = 204;
        resp_body = "";
    } else if (path == "/big_ncsi.txt") {
        // 测试用：含预期标识但长度超 8KB 的响应（长度启发式判劫持）
        resp_body = std::string(9000, 'x') + "Microsoft NCSI";
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

bool SimServer::LoggedIn() const { return impl_->logged_in.load(); }

}  // namespace cnk
