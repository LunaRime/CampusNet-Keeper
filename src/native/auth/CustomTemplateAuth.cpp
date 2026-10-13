#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Template.h"
#include "core/Log.h"
#include <cctype>
#include <cstring>
#include <regex>
#include <chrono>

namespace cnk {

namespace {

bool RegexSearch(const std::string& pattern, const std::string& text) {
    if (pattern.empty()) return false;
    try {
        std::regex re(pattern, std::regex::ECMAScript);
        return std::regex_search(text, re);
    } catch (const std::regex_error& e) {
        LogWarn(std::string("自定义模板的成功正则无效：") + e.what());
        return false;
    }
}

// HTTP 头名大小写不敏感：把用户配置的键与全小写目标名比较
bool HeaderNameEquals(const std::string& key, const char* lower) {
    const size_t n = std::strlen(lower);
    if (key.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)key[i]) != lower[i]) return false;
    return true;
}

class CustomTemplateAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;
        const auto& ct = p.custom_template;

        if (ct.url.empty()) {
            r.message = "自定义模板的 URL 为空，请在高级设置中填写";
            return r;
        }

        std::map<std::string, std::string> vars;
        vars["username"] = p.username;
        vars["password"] = p.password;
        vars["isp"] = IspSuffix(p);
        vars["ip"] = ctx.local_ip;
        vars["timestamp"] = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        r.request_url = ExpandTemplate(ct.url, vars);
        r.request_body = ExpandTemplate(ct.body, vars);

        HttpRequest req;
        req.url = r.request_url;
        req.method = ct.method.empty() ? "POST" : ct.method;
        req.body = r.request_body;
        req.content_type = "application/x-www-form-urlencoded";
        // 现有 HttpRequest 只暴露 Content-Type 与 User-Agent 两个头；
        // 其余自定义头暂不生效（需要时扩展 Http 层）。头名按 HTTP 语义大小写不敏感。
        for (const auto& [k, v] : ct.headers) {
            if (HeaderNameEquals(k, "content-type")) req.content_type = v;
            if (HeaderNameEquals(k, "user-agent")) req.user_agent = v;
        }
        req.timeout_ms = 8000;
        req.follow_redirects = false;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "请求发送失败：" + err;
            return r;
        }

        const std::string page = DecodeBody(resp);
        if (ct.success_regex.empty()) {
            r.success = resp.status >= 200 && resp.status < 400;
            r.message = r.success
                ? "请求已发送（未配置成功判定正则，以 HTTP 状态判定）"
                : "HTTP 状态 " + std::to_string(resp.status);
            return r;
        }

        // 与 DrcomAuth 的判定维度保持一致：302 场景下成功标志可能只出现在
        // Location 头（body 为空），故正则同时匹配正文与 Location
        r.success = RegexSearch(ct.success_regex, page) ||
                    (!resp.location.empty() &&
                     RegexSearch(ct.success_regex, resp.location));
        r.message = r.success ? "响应匹配成功正则"
                              : "响应未匹配成功正则，请检查正则或凭据";
        return r;
    }

    const char* Name() const override { return "CustomTemplateAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeCustomTemplateAuth() {
    return std::make_unique<CustomTemplateAuth>();
}

}  // namespace cnk
