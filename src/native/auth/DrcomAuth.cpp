#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include "core/Log.h"
#include <sstream>

namespace cnk {

namespace {

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
        if (!SplitHostPort(p.auth_url, scheme_host, port, "801")) {
            r.message = "认证网址格式不正确：" + p.auth_url +
                        "（应形如 http://10.0.0.1/）";
            return r;
        }
        r.request_url = scheme_host + ":" + port +
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

        if (page.find("Dr.COMWebLoginID_3.htm") != std::string::npos ||
            page.find("3.htm") != std::string::npos) {
            r.success = true;
            r.vendor_code = 3;
            r.message = "服务器返回登录成功";
            return r;
        }
        if (page.find("Dr.COMWebLoginID_2.htm") != std::string::npos ||
            page.find("2.htm") != std::string::npos) {
            r.vendor_code = 2;
            r.success = false;
            r.message = "服务器返回登录失败（账号或密码错误，或该账号不可用）";
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
