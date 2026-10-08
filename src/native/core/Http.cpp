#include "core/Http.h"
#include "core/Version.h"

#include <windows.h>
#include <winhttp.h>

#include <string>

namespace cnk {

namespace {

// 正文上限：门户页面在百 KB 级，4 MB 足够，同时挡住异常大的响应。
constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;
constexpr DWORD kReadChunk = 8 * 1024;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// WinHTTP 句柄的 RAII 包装：不可拷贝，析构即关闭。
struct WinHttpHandle {
    HINTERNET h = nullptr;

    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    operator HINTERNET() const { return h; }
};

struct CrackedUrl {
    std::wstring host;
    std::wstring path;  // 含查询串，直接用作请求目标
    INTERNET_PORT port = 0;
    bool https = false;
};

bool CrackUrl(const std::string& url, CrackedUrl& out) {
    std::wstring wurl = Utf8ToWide(url);
    if (wurl.empty()) return false;

    wchar_t host_buf[256] = {};
    wchar_t path_buf[2048] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    if (host_buf[0] == L'\0') return false;

    // 未给 lpszExtraInfo 缓冲时查询串并入 lpszUrlPath，正是请求行需要的形式。
    out.host = host_buf;
    out.path = path_buf;
    if (out.path.empty()) out.path = L"/";
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

// 两段式查询：先取所需长度，再取内容。WinHttpQueryHeaders 与 WinHttpQueryOption
// 都是这个套路，差别只在查询函数本身。
template <typename QueryFn>
std::string QueryWide(QueryFn query) {
    DWORD len = 0;
    query(nullptr, &len);
    if (len == 0) return {};  // 不存在（如响应没有该头）
    std::wstring buf((size_t)(len / sizeof(wchar_t)), L'\0');
    if (!query(buf.data(), &len)) return {};
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return WideToUtf8(buf);
}

// 取单个响应头。头不存在时返回空串（不是错误）。
std::string QueryHeader(HINTERNET request, DWORD which) {
    return QueryWide([&](void* buf, DWORD* len) {
        return WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, buf, len,
                                   WINHTTP_NO_HEADER_INDEX) != FALSE;
    });
}

}  // namespace

bool HttpPerform(const HttpRequest& req, HttpResponse& out, std::string& error) {
    out = HttpResponse{};
    error.clear();

    auto fail = [&out, &error](const std::string& msg) {
        error = msg;
        out.error = msg;  // 与 Ok() 保持一致：失败即 Ok()==false
        return false;
    };

    CrackedUrl u;
    if (!CrackUrl(req.url, u)) return fail("URL 解析失败: " + req.url);

    const int timeout = req.timeout_ms > 0 ? req.timeout_ms : 3000;

    // 直连不走系统代理：校园网内网地址经代理无意义，且代理会掩盖劫持特征。
    const std::wstring agent = Utf8ToWide(std::string("CampusNetKeeper/") + VersionString());
    WinHttpHandle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return fail("WinHttpOpen 失败，错误码 " + std::to_string(GetLastError()));

    // 解析/连接/发送/接收四类超时压到同一个值。
    // 实测 WinHTTP 的实际粒度约 4 秒：<=4000ms 的配置一律约 4 秒才返回
    // （同机 curl 走原生 socket 则精确遵守毫秒），配置值只当上界用。
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    WinHttpHandle connect{WinHttpConnect(session, u.host.c_str(), u.port, 0)};
    if (!connect.h)
        return fail("无法连接到 " + WideToUtf8(u.host) + ":" + std::to_string(u.port) +
                    "，错误码 " + std::to_string(GetLastError()));

    const std::wstring verb = Utf8ToWide(req.method);
    WinHttpHandle request{WinHttpOpenRequest(
        connect, verb.c_str(), u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h)
        return fail("WinHttpOpenRequest 失败，错误码 " + std::to_string(GetLastError()));

    // WinHTTP 默认跟随重定向（同协议最多 10 跳），"不跟随"必须显式关掉。
    // 看见 302 且 Location 指向别的主机是判定 Portal 劫持的依据；跟随了就只剩 200。
    if (!req.follow_redirects) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
            return fail("无法关闭自动重定向，错误码 " + std::to_string(GetLastError()));
    }

    std::wstring headers;
    if (!req.content_type.empty())
        headers += L"Content-Type: " + Utf8ToWide(req.content_type) + L"\r\n";
    if (!req.user_agent.empty())
        headers += L"User-Agent: " + Utf8ToWide(req.user_agent) + L"\r\n";
    headers += L"Cache-Control: no-cache\r\n";

    if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L,
                            req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(),
                            (DWORD)req.body.size(), (DWORD)req.body.size(), 0))
        return fail("发送请求失败，错误码 " + std::to_string(GetLastError()));

    if (!WinHttpReceiveResponse(request, nullptr))
        return fail("接收响应超时或失败，错误码 " + std::to_string(GetLastError()));

    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                             WINHTTP_NO_HEADER_INDEX))
        return fail("查询状态码失败，错误码 " + std::to_string(GetLastError()));
    out.status = (int)status;

    out.server = QueryHeader(request, WINHTTP_QUERY_SERVER);
    out.location = QueryHeader(request, WINHTTP_QUERY_LOCATION);
    out.content_type = QueryHeader(request, WINHTTP_QUERY_CONTENT_TYPE);

    // 读正文。三种退出条件保证循环必然终止，Content-Length 与实际字节不符也不会挂住：
    //   avail==0 读尽 / read==0 无进度 / 达到 4 MB 上限。
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail))
            return fail("读取响应失败，错误码 " + std::to_string(GetLastError()));
        if (avail == 0) break;
        if (body.size() >= kMaxBodyBytes) break;

        DWORD want = avail < kReadChunk ? avail : kReadChunk;
        if (want > kMaxBodyBytes - body.size()) want = (DWORD)(kMaxBodyBytes - body.size());

        char buf[kReadChunk];
        DWORD read = 0;
        if (!WinHttpReadData(request, buf, want, &read))
            return fail("读取响应失败，错误码 " + std::to_string(GetLastError()));
        if (read == 0) break;
        body.append(buf, read);
    }

    out.body = std::move(body);

    // 跟随重定向后 req.url 已不是实际取回正文的地址；WinHTTP 记录了最终 URL，
    // 问得到就用（实测跨请求仍返回终点地址）。不跟随时 req.url 本身即准确值。
    out.effective_url = req.url;
    if (req.follow_redirects) {
        std::string final_url = QueryWide([&](void* buf, DWORD* len) {
            return WinHttpQueryOption(request, WINHTTP_OPTION_URL, buf, len) != FALSE;
        });
        if (!final_url.empty()) out.effective_url = final_url;
    }
    return true;
}

}  // namespace cnk
