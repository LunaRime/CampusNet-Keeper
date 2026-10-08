#include "doctest.h"
#include "json.hpp"
#include "sim/SimServer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // MultiByteToWideChar / CP_UTF8；必须排在 winsock2.h 之后
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {

// 极简 HTTP 客户端：发一个请求，读回全部响应字节。
// 仅用于测试断言，不解析响应（正文匹配用 find 就够）。
std::string RawHttp(int port, const std::string& method,
                    const std::string& path, const std::string& body) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        return {};
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    // 客户端超时：不依赖"服务端一定会 close"这个不变量，
    // 否则将来某条服务端路径忘记 close 会让整个套件挂死而不是一条用例转红。
    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    send(s, req.c_str(), (int)req.size(), 0);

    std::string resp;
    char buf[4096];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);

    closesocket(s);
    WSACleanup();
    return resp;
}

// 用系统 API 按指定代码页把字节解成 UTF-8。返回空串表示解码失败。
std::string DecodeToUtf8(const std::string& bytes, UINT code_page) {
    if (bytes.empty()) return {};
    int wn = MultiByteToWideChar(code_page, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wn <= 0) return {};
    std::wstring wide((size_t)wn, L'\0');
    MultiByteToWideChar(code_page, 0, bytes.data(), (int)bytes.size(), wide.data(), wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return {};
    std::string utf8((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wn, utf8.data(), un, nullptr, nullptr);
    return utf8;
}

// 用系统 API 把 UTF-8 文本编成指定代码页的字节。返回空串表示失败。
std::string EncodeFromUtf8(const std::string& utf8, UINT code_page) {
    if (utf8.empty()) return {};
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wn <= 0) return {};
    std::wstring wide((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), wn);
    int n = WideCharToMultiByte(code_page, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(code_page, 0, wide.data(), wn, out.data(), n, nullptr, nullptr);
    return out;
}

// 把 GBK(936) 字节解成 UTF-8，用于端到端验证服务端真的发了 GBK。
std::string GbkToUtf8(const std::string& gbk) { return DecodeToUtf8(gbk, 936); }

// 该字节串本身能否作为合法 UTF-8 读取（用于确认证据文件确实是 GBK 原始字节）。
bool IsValidUtf8(const std::string& bytes) {
    if (bytes.empty()) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                               (int)bytes.size(), nullptr, 0) > 0;
}

// 读文件原始字节（不解码、不做换行转换）。
std::string ReadFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 取 JS 变量 name=... 到下一个 ';' 之间的值，去掉首尾空白与包裹的引号。
// 真实首页里这六个字段与 carrier 的值都不含 ';'。
std::string JsValue(const std::string& text, const std::string& name) {
    std::string needle = name + "=";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        bool at_boundary = (pos == 0) ||
            !(std::isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '_');
        if (at_boundary) {
            size_t start = pos + needle.size();
            size_t end = text.find(';', start);
            if (end == std::string::npos) return {};
            std::string v = text.substr(start, end - start);
            size_t b = v.find_first_not_of(" \t\r\n");
            size_t e = v.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            v = v.substr(b, e - b + 1);
            if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
                v = v.substr(1, v.size() - 2);
            return v;
        }
        pos += needle.size();
    }
    return {};
}

// 从响应头里取字符集声明（"charset=xxx"）。
std::string CharsetOf(const std::string& resp) {
    size_t p = resp.find("charset=");
    if (p == std::string::npos) return {};
    size_t e = resp.find_first_of("\r\n;", p);
    return resp.substr(p, e - p);
}

// 从 loadConfig 证据里取出 visit_blacklist 的数组原文（含方括号，不含键名）。
std::string ExtractBlacklist(const std::string& json) {
    size_t p = json.find("\"visit_blacklist\":[");
    if (p == std::string::npos) return {};
    size_t open = json.find('[', p);
    size_t close = json.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return {};
    return json.substr(open, close - open + 1);
}

std::string StripHeaders(const std::string& resp) {
    size_t hdr_end = resp.find("\r\n\r\n");
    return hdr_end == std::string::npos ? std::string() : resp.substr(hdr_end + 4);
}

std::string EvidencePath(const char* name) {
    return std::string(CNK_EVIDENCE_DIR) + "/" + name;
}

// 剥掉 dr1003(...); 包裹并解析其中的 JSON（与下游解析器走同一条路径）。
// 解析失败返回 false，不会抛异常。
bool ParseDr1003(const std::string& body, nlohmann::json& out) {
    const std::string prefix = "dr1003(";
    if (body.compare(0, prefix.size(), prefix) != 0) return false;
    size_t end = body.rfind(");");
    if (end == std::string::npos || end < prefix.size()) return false;
    out = nlohmann::json::parse(body.substr(prefix.size(), end - prefix.size()),
                                nullptr, false);
    return !out.is_discarded();
}

}  // namespace

TEST_CASE("模拟服务器能响应门户首页请求") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    std::string resp = RawHttp(srv.Port(), "GET", "/", "");
    CHECK(resp.find("HTTP/1.1 200") != std::string::npos);
    CHECK(resp.find("DrcomServer1.0") != std::string::npos);
    CHECK(resp.find("Dr.COMWebLoginID_") != std::string::npos);

    srv.Stop();
}

TEST_CASE("模拟服务器对正确凭据返回成功标志") {
    cnk::SimOptions opts;
    opts.valid_user = "alice";
    opts.valid_pass = "pw123";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
        "DDDDD=alice&upass=pw123&R1=0&R2=0&R3=0&R6=0&para=00&0MKKey=123456");
    CHECK(resp.find("Dr.COMWebLoginID_3.htm") != std::string::npos);
    CHECK(srv.LoginCount() == 1);

    srv.Stop();
}

TEST_CASE("模拟服务器对错误凭据返回失败标志") {
    cnk::SimOptions opts;
    opts.valid_user = "alice";
    opts.valid_pass = "pw123";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                               "DDDDD=alice&upass=wrong");
    CHECK(resp.find("Dr.COMWebLoginID_2.htm") != std::string::npos);

    srv.Stop();
}

TEST_CASE("模拟服务器按后缀校验运营商") {
    cnk::SimOptions opts;
    opts.valid_user = "bob";
    opts.valid_pass = "pw";
    opts.isp_suffix = "@cmcc";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    std::string resp = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                               "DDDDD=bob@cmcc&upass=pw");
    CHECK(resp.find("Dr.COMWebLoginID_3.htm") != std::string::npos);

    srv.Stop();
}

TEST_CASE("门户首页按 GBK 发送，中文运营商名可读") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    std::string resp = RawHttp(srv.Port(), "GET", "/", "");
    size_t hdr_end = resp.find("\r\n\r\n");
    REQUIRE(hdr_end != std::string::npos);
    // 声明侧：头部与 meta 都按真实门户声明 GBK
    CHECK(resp.find("charset=gbk") != std::string::npos);
    CHECK(resp.find("charset=\"gb2312\"") != std::string::npos);

    std::string body = resp.substr(hdr_end + 4);
    // 正文长度必须按转码后的字节算，否则客户端会截断或读超
    size_t cl_pos = resp.find("Content-Length: ");
    REQUIRE(cl_pos != std::string::npos);
    size_t content_length = (size_t)std::strtoul(resp.c_str() + cl_pos + 16, nullptr, 10);
    CHECK(content_length == body.size());

    // 字节侧：用系统 API 按 GBK 解码，中文运营商名必须可读
    std::string utf8 = GbkToUtf8(body);
    CHECK(utf8.find("服务类型") != std::string::npos);
    CHECK(utf8.find("校园用户") != std::string::npos);
    CHECK(utf8.find("校园电信") != std::string::npos);
    CHECK(utf8.find("校园联通") != std::string::npos);
    // 负向：正文不得是 UTF-8 字节序列，证明真的转码了而非两边都能读
    CHECK(body.find("校园用户") == std::string::npos);
    CHECK(body.find("校园电信") == std::string::npos);

    srv.Stop();
}

TEST_CASE("首页字段与实测证据逐项一致（忠实性）") {
    // 证据文件是线上原始 GBK 字节，按 GBK 解——顺带验证解码路径。
    std::string raw = ReadFileBytes(EvidencePath("portal_home_gbk_raw.htm"));
    REQUIRE(!raw.empty());
    // 夹具自检：它必须是"不能当合法 UTF-8 读"的原始字节。若将来又被存成 UTF-8，
    // 这条会先失败，避免把夹具问题误报成模拟器问题。
    REQUIRE(!IsValidUtf8(raw));
    std::string evidence = GbkToUtf8(raw);
    REQUIRE(!evidence.empty());
    // 证据侧自证：真实首页没有 visit_blacklist（免劫持清单只在 loadConfig 响应里）
    CHECK(evidence.find("visit_blacklist") == std::string::npos);

    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));
    std::string page = GbkToUtf8(StripHeaders(RawHttp(srv.Port(), "GET", "/", "")));
    REQUIRE(!page.empty());

    // 逐项一致：字段名与取值都必须来自实测原件，改名或改值都会被钉住
    for (const char* field : {"authloginpath", "authloginparam", "authuserfield",
                              "authpassfield", "authsuccess", "authfail", "carrier"}) {
        INFO("字段: " << std::string(field));
        CHECK(JsValue(page, field) != "");
        CHECK(JsValue(page, field) == JsValue(evidence, field));
    }
    // 端口是"不变量"而非字面值：真实门户上报 801 是因为它就在 801 上服务，
    // 模拟器监听随机端口，就必须上报自己的实际端口，否则客户端照配置连过来会连不上。
    CHECK(JsValue(evidence, "authloginport") == "801");
    CHECK(JsValue(page, "authloginport") == std::to_string(srv.Port()));
    // epHTTPPort 只在首页引用的 a41.js 里（首页 HTML 0 命中），客户端探测拿不到它，
    // 模拟器首页就不得凭空多给这条指纹 —— 防止将来有人又把它加回来。
    CHECK(evidence.find("epHTTPPort") == std::string::npos);
    CHECK(page.find("epHTTPPort") == std::string::npos);
    // 「哆点」是 Dr.COM 的识别指纹之一：真实首页以 GBK 中文写着「哆点参数 start」，
    // 客户端按声明解码后应能读到它（证据侧一并自证，防指纹被删而无人察觉）。
    CHECK(evidence.find("/** 哆点参数 start */") != std::string::npos);
    CHECK(page.find("/** 哆点参数 start */") != std::string::npos);
    // 真实首页没有免劫持清单，客户端必须容忍缺失并回退到内置默认值
    CHECK(page.find("visit_blacklist") == std::string::npos);

    srv.Stop();
}

TEST_CASE("loadConfig 端点与实测证据逐项一致（忠实性）") {
    std::string evidence = ReadFileBytes(EvidencePath("loadconfig_185_801.json"));
    REQUIRE(!evidence.empty());
    std::string blacklist = ExtractBlacklist(evidence);
    REQUIRE(!blacklist.empty());

    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));
    std::string resp = RawHttp(srv.Port(), "GET", "/eportal/portal/page/loadConfig", "");
    CHECK(resp.find("HTTP/1.1 200") != std::string::npos);
    std::string body = StripHeaders(resp);
    REQUIRE(!body.empty());

    // 声明与字节必须一致。首页方向靠「按 GBK 解出中文 + 正文不得是 UTF-8 字节」两侧钉住，
    // 这一侧是对称的镜像保护：声明 utf-8 却发 GBK 字节，同样会让下游乱码。
    CHECK(CharsetOf(resp) == "charset=utf-8");
    const std::string kMsg = "加载页面设置信息成功！";
    CHECK(DecodeToUtf8(body, CP_UTF8).find(kMsg) != std::string::npos);
    std::string msg_gbk = EncodeFromUtf8(kMsg, 936);
    REQUIRE(!msg_gbk.empty());
    CHECK(msg_gbk != kMsg);                          // 自检：GBK 字节确实不同于 UTF-8
    CHECK(body.find(msg_gbk) == std::string::npos);  // 正文不得是 GBK 字节

    CHECK(body.compare(0, 8, "dr1003({") == 0);
    REQUIRE(body.size() >= 3);
    CHECK(body.compare(body.size() - 3, 3, "});") == 0);
    // 15 项免劫持清单逐字一致（顺序也钉住）
    CHECK(body.find(blacklist) != std::string::npos);

    // 结构层面：用与下游同一个解析器（nlohmann/json）解析两侧，钉住类型与取值。
    // 字符串型字段必须是字符串：原件里 en_md5 / ep_http_port 都是字符串，
    // 写成数字会诱导下游写出"只在模拟器上正确"的解析代码。
    nlohmann::json ev, sim;
    REQUIRE(ParseDr1003(evidence, ev));
    REQUIRE(ParseDr1003(body, sim));
    CHECK(sim["code"] == 1);
    CHECK(sim["msg"] == ev["msg"]);
    CHECK(sim["data"]["visit_blacklist"].size() == 15);
    CHECK(sim["data"]["visit_blacklist"] == ev["data"]["visit_blacklist"]);
    CHECK(sim["data"]["en_md5"].is_string());
    CHECK(sim["data"]["en_md5"] == ev["data"]["en_md5"]);
    CHECK(sim["data"]["ep_http_port"].is_string());
    CHECK(sim["data"]["ep_http_port"] == std::to_string(srv.Port()));
    // 免劫持清单只在 loadConfig 里，首页不得有
    CHECK(RawHttp(srv.Port(), "GET", "/", "").find("visit_blacklist") == std::string::npos);

    srv.Stop();
}

// 三条路径各自声明自己的编码：首页 gbk，/eportal/ 与 loadConfig 都是 utf-8。
// 声明值必须与正文实际字节一致（正文编码由上面两条忠实性测试做字节级校验）。
TEST_CASE("三条路径各自声明正确的字符集") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));

    std::string home = RawHttp(srv.Port(), "GET", "/", "");
    std::string login = RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    std::string config = RawHttp(srv.Port(), "GET", "/eportal/portal/page/loadConfig", "");

    CHECK(CharsetOf(home) == "charset=gbk");
    CHECK(CharsetOf(login) == "charset=utf-8");
    CHECK(CharsetOf(config) == "charset=utf-8");
    CHECK(CharsetOf(home) != CharsetOf(login));
    CHECK(CharsetOf(login) == CharsetOf(config));

    srv.Stop();
}

TEST_CASE("运营商后缀不匹配时登录失败") {
    cnk::SimOptions opts;
    opts.valid_user = "bob";
    opts.valid_pass = "pw";
    opts.isp_suffix = "@cmcc";
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(opts, err));

    // 后缀不符：不得放行（挡住"把后缀剥掉再比"的宽松实现）
    std::string wrong_suffix = RawHttp(srv.Port(), "POST", srv.LoginPath(),
                                       "DDDDD=bob@dx&upass=pw");
    CHECK(wrong_suffix.find("Dr.COMWebLoginID_2.htm") != std::string::npos);
    CHECK(wrong_suffix.find("Dr.COMWebLoginID_3.htm") == std::string::npos);

    // 缺后缀的裸账号同样不得放行
    std::string bare = RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=bob&upass=pw");
    CHECK(bare.find("Dr.COMWebLoginID_2.htm") != std::string::npos);
    CHECK(bare.find("Dr.COMWebLoginID_3.htm") == std::string::npos);

    srv.Stop();
}

TEST_CASE("登录计数随请求累加") {
    cnk::SimServer srv;
    std::string err;
    REQUIRE(srv.Start(cnk::SimOptions{}, err));
    CHECK(srv.LoginCount() == 0);
    RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    CHECK(srv.LoginCount() == 1);
    RawHttp(srv.Port(), "POST", srv.LoginPath(), "DDDDD=x&upass=y");
    CHECK(srv.LoginCount() == 2);
    srv.Stop();
}
