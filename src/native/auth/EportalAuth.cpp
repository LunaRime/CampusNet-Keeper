#include "auth/AuthFactory.h"
#include "core/Encoding.h"
#include "core/Http.h"
#include "core/LocalIp.h"
#include "core/Log.h"
#include "core/Url.h"
#include "json.hpp"

#include <cstdlib>
#include <ctime>

namespace cnk {

namespace {

// 从 JSONP（前缀 + ({...});）截出 JSON 主体并解析；纯 JSON 响应同样适用。
bool ParseJsonp(const std::string& body, nlohmann::json& out) {
    const size_t b = body.find('{');
    const size_t e = body.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e < b) return false;
    try {
        out = nlohmann::json::parse(body.substr(b, e - b + 1));
    } catch (...) {
        return false;
    }
    return true;
}

int RandomInt(int lo, int hi) {
    static const bool seeded = [] {
        std::srand((unsigned)std::time(nullptr));
        return true;
    }();
    (void)seeded;
    return lo + std::rand() % (hi - lo + 1);
}

// 锐捷 eportal（spec §4.4）。各校部署差异较大，本实现只覆盖公开的标准形态：
//   GET {auth_url 的 scheme+host:port}/eportal/?c=Portal&a=login&...&user_account=账号,0,服务
//   响应 JSONP：dr1003({"result":1,"msg":"..."})，result==1 为成功
//
// ⚠ vendor_code 固定 -1（不透传厂商原始码）：本项目只有 Dr.COM 的码 2 具有
// 「凭据错误 → 停止重试」的约定语义（Scheduler 依赖它），跨厂商透传会误触发停止重试。
// 原始码仍会出现在失败信息里，便于诊断。
class EportalAuth : public IAuth {
public:
    LoginResult Login(const Profile& p, const NetContext& ctx) override {
        LoginResult r;

        if (p.username.empty()) {
            r.message = "账号为空，请在设置中填写";
            return r;
        }
        if (p.password.empty()) {
            r.message = "密码为空，请在设置中填写（若是首次使用请重新输入密码）";
            return r;
        }

        // 默认端口按 scheme 取：spec 里 https 门户是 802（DrcomAuth 纠正过同一假设）。
        // 裸 `https://host/` 若写成 801 永远打不到。
        const bool https = p.auth_url.compare(0, 6, "https:") == 0;
        std::string scheme_host, port;
        if (!SplitHostPort(p.auth_url, scheme_host, port, https ? "802" : "801")) {
            r.message = "认证网址格式不正确：" + p.auth_url + "（应形如 http://10.0.0.1/）";
            return r;
        }

        // 锐捷的 user_account 形如「账号,0,服务」，服务名按厂商取 IspSuffix
        // （eportal 下 telecom→telecom、cmcc→cmcc、campus→空）
        const std::string account_field = p.username + ",0," + IspSuffix(p);
        const std::string ip = ResolveClientIp(ctx.local_ip, p.auth_url);

        r.request_url = scheme_host + ":" + port + "/eportal/" +
                        "?c=Portal&a=login&callback=dr1003&login_method=1" +
                        "&user_account=" + UrlEncode(account_field) +
                        "&user_password=" + UrlEncode(p.password) +
                        "&wlan_user_ip=" + UrlEncode(ip) +
                        "&wlan_user_ipv6=&wlan_user_mac=000000000000" +
                        "&wlan_ac_ip=&wlan_ac_name=" +
                        "&jsVersion=4.1.3&v=" + std::to_string(RandomInt(1000, 9999)) +
                        "&lang=zh";
        r.request_body = "(GET，无请求体)";

        HttpRequest req;
        req.url = r.request_url;
        req.method = "GET";
        req.timeout_ms = 8000;
        req.follow_redirects = false;

        HttpResponse resp;
        std::string err;
        if (!HttpPerform(req, resp, err)) {
            r.message = "登录请求发送失败：" + err;
            return r;
        }

        const std::string text = DecodeBody(resp);
        LogInfo("锐捷登录响应 HTTP " + std::to_string(resp.status) + "，正文 " +
                std::to_string(text.size()) + " 字节");

        // 非 JSONP 应答必须给出可诊断的信息：本部署同族门户的实测形态就是
        // 「302 空正文」与「200 + SPA 外壳」（见 docs/evidence/README.md）。只回一句
        // 「无法解析响应」无从下手；更重的是调度器三振后会判「不是校园网」并停止重试，
        // 对凭据完全正确的学校是错误结论且不自愈。
        if (resp.status >= 300 && resp.status < 400) {
            r.message = "登录端点返回重定向 " + std::to_string(resp.status) +
                        (resp.location.empty()
                             ? std::string()
                             : "（Location: " + RedactSecrets(resp.location) + "）") +
                        "；该部署可能不使用本登录端点（锐捷各校差异较大）";
            return r;
        }
        if (resp.status >= 400) {
            r.message = "服务器返回错误状态码 " + std::to_string(resp.status) +
                        "，将由网络探测确认实际状态";
            return r;
        }

        nlohmann::json j;
        if (!ParseJsonp(text, j)) {
            r.message = "无法解析服务器响应（HTTP " + std::to_string(resp.status) +
                        (resp.content_type.empty()
                             ? std::string()
                             : "，Content-Type: " + RedactSecrets(resp.content_type)) +
                        "），将由网络探测确认实际状态";
            return r;
        }

        int code = -1;
        std::string msg;
        try {
            if (j.contains("result")) {
                if (j["result"].is_number()) code = j["result"].get<int>();
                else if (j["result"].is_string())
                    code = std::atoi(j["result"].get<std::string>().c_str());
            }
            if (j.contains("msg") && j["msg"].is_string())
                msg = j["msg"].get<std::string>();
        } catch (...) {
            // 保持缺省值，按「无有效 result」处理
        }

        if (code == 1) {
            r.success = true;
            r.message = "服务器返回登录成功（锐捷 Portal 认证成功）";
            return r;
        }

        r.success = false;
        r.message = "服务器返回登录失败：";
        r.message += msg.empty() ? "（无消息）" : RedactSecrets(msg);
        // 原始码始终附上（计划注记要求「原始码仍出现在失败信息里」）：各校码表不同，
        // 现场判读需要它，但它不作为「凭据错误」的唯一依据（见上方 vendor_code 说明）
        r.message += "（result=" + std::to_string(code) + "）";
        return r;
    }

    const char* Name() const override { return "EportalAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeEportalAuth() { return std::make_unique<EportalAuth>(); }

}  // namespace cnk
