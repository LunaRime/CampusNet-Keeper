#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include "core/Log.h"
#include <cctype>
#include <sstream>

namespace cnk {

namespace {

// 取信息页里 msga='...' / msga="..." 的值（首个合法命中，限长防异常回显）。
// msga 是厂商对该次登录的原始判定码（实测样例：'userid error1'/'userid error2'），
// 诊断价值高，随失败信息一并呈现。
std::string ExtractJsString(const std::string& body, const std::string& name) {
    size_t pos = 0;
    while ((pos = body.find(name, pos)) != std::string::npos) {
        const bool at_boundary =
            pos == 0 ||
            !(std::isalnum((unsigned char)body[pos - 1]) || body[pos - 1] == '_');
        size_t i = pos + name.size();
        if (at_boundary) {
            while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
            if (i < body.size() && body[i] == '=') {
                ++i;
                while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
                if (i < body.size() && (body[i] == '\'' || body[i] == '"')) {
                    const char quote = body[i];
                    const size_t end = body.find(quote, i + 1);
                    if (end != std::string::npos) {
                        std::string v = body.substr(i + 1, end - i - 1);
                        if (v.size() > 64) v.resize(64);
                        return v;
                    }
                }
            }
        }
        // 该处命中非法形态时继续找后续出现（防首个坏值吞掉后面的合法值）
        pos += name.size();
    }
    return {};
}

class DrcomAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext&) override {
        LoginResult r;

        if (p.username.empty()) {
            r.message = "账号为空，请在设置中填写";
            return r;
        }
        if (p.password.empty()) {
            r.message = "密码为空，请在设置中填写（若是首次使用请重新输入密码）";
            return r;
        }

        std::string scheme_host, port;
        // spec §7.1 原文称登录端口 801；**2026-10-16 实测修订**：老式 c=ACSetting
        // 协议与门户页同源——:801 上该请求只得到 SPA 外壳（不处理登录），同源 :80
        // 才真正处理并返回 Dr.COMWebLoginID_2/3 信息页。故使用 auth_url 自身端口，
        // 未显式写端口时不补默认端口（按 scheme 缺省 80/443）。
        if (!SplitHostPort(p.auth_url, scheme_host, port, "")) {
            r.message = "认证网址格式不正确：" + p.auth_url +
                        "（应形如 http://10.0.0.1/）";
            return r;
        }
        r.request_url = scheme_host + (port.empty() ? "" : ":" + port) +
                        "/eportal/?c=ACSetting&a=Login&url=drappall";

        const std::string account = p.username + IspSuffix(p);

        std::ostringstream body;
        body << "DDDDD=" << UrlEncode(account)
             << "&upass=" << UrlEncode(p.password)
             << "&R1=0&R2=0&R3=0&R6=0"
             << "&para=00"
             << "&0MKKey=123456"
             << "&buttonClicked=&redirect_url=&err_flag="
             << "&username=&password=&user=&cmd=&Login="
             << "&v6ip=";
        r.request_body = body.str();

        HttpRequest req;
        req.url = r.request_url;
        req.method = "POST";
        req.body = r.request_body;
        req.content_type = "application/x-www-form-urlencoded";
        req.timeout_ms = 8000;
        req.follow_redirects = false;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "登录请求发送失败：" + err;
            return r;
        }

        const std::string page = DecodeBody(resp);
        LogInfo("Dr.COM 登录响应 HTTP " + std::to_string(resp.status) +
                "，正文 " + std::to_string(page.size()) + " 字节");

        // 4xx/5xx 是服务器错误页，其中的文本不构成有效的登录结果判定
        if (resp.status >= 400) {
            r.vendor_code = -1;
            r.success = false;
            r.message = "服务器返回错误状态码 " + std::to_string(resp.status) +
                        "，将由网络探测确认实际状态";
            return r;
        }

        const std::string& loc = resp.location;

        // 完整标记可信度最高；Location 只认完整标记——短形会误伤
        // http://x/error3.htm 这类无关 URL
        const bool hit3_full =
            page.find("Dr.COMWebLoginID_3.htm") != std::string::npos ||
            loc.find("Dr.COMWebLoginID_3.htm") != std::string::npos;
        const bool hit2_full =
            page.find("Dr.COMWebLoginID_2.htm") != std::string::npos ||
            loc.find("Dr.COMWebLoginID_2.htm") != std::string::npos;
        // 短形标记仅用于正文（spec §7.2 的宽松判据，常见于跳转脚本）
        const bool hit3 = hit3_full || page.find("3.htm") != std::string::npos;
        const bool hit2 = hit2_full || page.find("2.htm") != std::string::npos;

        // 两个方向同时命中（失败响应回显了含 authsuccess/authfail 的框架脚本，
        // 真实首页即同时含两标记）时不猜，交探测裁决——乐观判成功会使
        // 调度器跳过探测复验并绕过凭据错误的停止重试逻辑
        if (hit3 && hit2) {
            r.vendor_code = -1;
            r.success = false;
            r.message = "响应同时含成功与失败标志，将由网络探测确认实际状态";
            return r;
        }
        if (hit3) {
            r.success = true;
            r.vendor_code = 3;
            r.message = "服务器返回登录成功";
            return r;
        }
        if (hit2) {
            r.vendor_code = 2;
            r.success = false;
            r.message = "服务器返回登录失败（账号或密码错误，或该账号不可用）";
            // 附上厂商原始判定码 msga（如 'userid error1/2'）——2026-10-16 实测该码
            // 能区分「用户不存在 / 已在线」等成因，是本项目的关键现场证据
            const std::string detail = ExtractJsString(page, "msga");
            if (!detail.empty())
                r.message += "；服务器信息：" + RedactSecrets(detail);
            return r;
        }

        // 无法从正文判定：交给上层用探测结果定夺
        r.vendor_code = -1;
        r.success = false;
        r.message = "服务器未返回明确的成功或失败标志，将由网络探测确认实际状态";
        return r;
    }

    const char* Name() const override { return "DrcomAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeDrcomAuth() { return std::make_unique<DrcomAuth>(); }

}  // namespace cnk
