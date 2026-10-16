#include "auth/AuthFactory.h"
#include "core/Encoding.h"
#include "core/HtmlForm.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Url.h"

#include <cctype>
#include <vector>

namespace cnk {

namespace {

// 字段启发式（spec §4.2，大小写不敏感，按序取首个命中）
const std::vector<std::string> kUserFields = {
    "username", "user", "account", "name", "DDDDD",
    "user_account", "user_name", "login_name"};
const std::vector<std::string> kPassFields = {
    "password", "pwd", "pass", "upass", "user_password", "passwd"};
const std::vector<std::string> kIspFields = {
    "domain", "service", "isp", "operator", "type"};

std::string ToLowerCopy(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool CaseInsensitiveEqual(const std::string& a, const std::string& b) {
    return ToLowerCopy(a) == ToLowerCopy(b);
}

// 未识别厂商时的兜底：解析 Portal 页面里的第一个表单，按字段名启发式填入凭据后提交。
// 识别不了就明确让用户改用自定义模板——绝不静默假装成功（spec §5 要求 5）。
class GenericFormAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;

        if (ctx.portal_body.empty()) {
            r.message = "尚未取得 Portal 页面内容，无法解析登录表单。"
                        "若反复出现，请改用自定义模板。";
            return r;
        }
        if (p.username.empty()) {
            r.message = "账号为空，请在设置中填写";
            return r;
        }
        if (p.password.empty()) {
            r.message = "密码为空，请在设置中填写（若是首次使用请重新输入密码）";
            return r;
        }

        HtmlForm form;
        if (!ParseFirstForm(ctx.portal_body, form)) {
            r.message = "Portal 页面中没有找到登录表单，无法自动填写。"
                        "请改用自定义模板：在浏览器开发者工具中复制登录请求的 "
                        "URL 与请求体，粘贴到设置的自定义模板中。";
            return r;
        }

        const std::string user_field = FindFieldByNames(form, kUserFields);
        const std::string pass_field = FindFieldByNames(form, kPassFields);
        const std::string isp_field = FindFieldByNames(form, kIspFields);

        if (user_field.empty() || pass_field.empty()) {
            r.message = "无法识别表单中的账号或密码字段名，请改用自定义模板。";
            return r;
        }

        const std::string isp = IspSuffix(p);

        std::string body;
        for (const auto& f : form.fields) {
            std::string value = f.value;
            if (CaseInsensitiveEqual(f.name, user_field))
                value = p.username;
            else if (CaseInsensitiveEqual(f.name, pass_field))
                value = p.password;
            else if (!isp_field.empty() && CaseInsensitiveEqual(f.name, isp_field))
                value = isp;
            if (!body.empty()) body += "&";
            body += UrlEncode(f.name) + "=" + UrlEncode(value);
        }

        // 解析基准：优先 portal_url（表单所在页面的 URL，spec §4.2），其次 auth_url。
        // ⚠ 已知边界：ResolveUrl 对不以 '/' 开头的 path-relative action 按 origin 拼接，
        // 不是 RFC 3986 目录语义（见 core/Url.h 警示）；页面在子路径且 action 为
        // path-relative 时会丢目录前缀，届时需扩展 ResolveUrl。
        const std::string base = ctx.portal_url.empty() ? p.auth_url : ctx.portal_url;
        r.request_url = ResolveUrl(form.action, base);
        r.request_body = body;

        HttpRequest req;
        req.url = r.request_url;
        const bool is_get = CaseInsensitiveEqual(form.method, "GET");
        req.method = is_get ? "GET" : "POST";
        if (is_get) {
            req.url += (req.url.find('?') == std::string::npos ? "?" : "&") + body;
        } else {
            req.body = body;
            req.content_type = "application/x-www-form-urlencoded";
        }
        req.timeout_ms = 8000;
        req.follow_redirects = false;
        // GET 的字段在查询串里：诊断输出必须显示**实际请求的 URL**，否则用户看到的
        // 是不带参数的地址，无法核对填了什么
        r.request_url = req.url;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "请求发送失败：" + err;
            return r;
        }

        const std::string text = DecodeBody(resp);
        LogInfo("通用表单登录响应 HTTP " + std::to_string(resp.status) + "，正文 " +
                std::to_string(text.size()) + " 字节");

        // 3xx：登录成功后回跳是常见形态，但没有响应体可判，且刻意不跟随重定向
        //（不跟随是为了保留原始响应）。如实报「无法判定」，交下一次探测裁决——
        // 绝不猜成功：Scheduler 拿到 success 会直接置 Online 并 ResetCache，猜错代价高。
        if (resp.status >= 300 && resp.status < 400) {
            r.message = "服务器返回重定向 " + std::to_string(resp.status) +
                        (resp.location.empty()
                             ? std::string()
                             : "（Location: " + RedactSecrets(resp.location) + "）") +
                        "，无法从响应判定，将由网络探测确认";
            return r;
        }
        if (resp.status >= 400) {
            r.message = "服务器返回错误状态码 " + std::to_string(resp.status) +
                        "，将由网络探测确认实际状态";
            return r;
        }

        // 成功判定（spec §4.2 的 成功 / success / result":1）收紧为**具体形态**：
        // 裸 "success" 会命中登录页里的 `var loginSuccess=false` 之类，把失败判成成功
        // → 调度器置 Online + ResetCache，用户看到「已重新登录」而网络其实没恢复
        //（2026-10-16 审阅 P1）。
        const bool looks_ok =
            text.find("成功") != std::string::npos ||
            text.find("\"result\":1") != std::string::npos ||
            text.find("\"result\": 1") != std::string::npos ||
            text.find("\"success\":true") != std::string::npos ||
            text.find("\"success\": true") != std::string::npos ||
            text.find("\"code\":0") != std::string::npos;
        r.success = looks_ok;
        r.message = looks_ok ? "响应中包含成功标识"
                             : "响应中没有成功标识，将由网络探测确认实际状态";
        return r;
    }

    const char* Name() const override { return "GenericFormAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeGenericFormAuth() {
    return std::make_unique<GenericFormAuth>();
}

}  // namespace cnk
