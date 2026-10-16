#include "auth/AuthFactory.h"
#include "core/Encoding.h"
#include "core/Http.h"
#include "core/LocalIp.h"
#include "core/Log.h"
#include "core/Url.h"
#include "crypto/Hash.h"
#include "crypto/SrunXEncode.h"
#include "json.hpp"

#include <cctype>
#include <chrono>

namespace cnk {

namespace {

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// JSONP（callback({"k":v})）或纯 JSON 都能解出对象
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

// 在文本里找 `ac_id` 之后紧跟的第一个数字串，兼容三种真实形态：
//   URL 查询串 `?ac_id=12`、表单 `<input name="ac_id" value="12">`、JSON `"ac_id":"12"`
std::string FindAcId(const std::string& text) {
    size_t k = 0;
    while ((k = text.find("ac_id", k)) != std::string::npos) {
        size_t i = k + 5;
        while (i < text.size() && (text[i] == '"' || text[i] == '\'' || text[i] == ' ' ||
                                   text[i] == ':' || text[i] == '='))
            ++i;
        size_t e = i;
        while (e < text.size() && std::isdigit((unsigned char)text[e])) ++e;
        if (e > i) return text.substr(i, e - i);
        k += 5;
    }
    return {};
}

// 严格整数解析：整串必须都是数字。`atoi("abc")` 得 0、`atoi("1abc")` 得 1，
// 会把失败读成成功（ecode/code 都用于判成功）。非纯数字返回 fallback。
int StrictInt(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    for (unsigned char c : s)
        if (!std::isdigit(c) && c != '-') return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

// 深澜 srun。协议按三份独立来源逐字段核对（2026-10-16）：
//   ① 深大 srun 逆向（门户 Portal.js 原文 + 可运行 Python 脚本）
//   ② srunc（Rust，OpenWrt 客户端）  ③ bitgateway-client（Rust，BIT 客户端）
// 三者一致的部分：
//   1) GET {auth_url}/cgi-bin/get_challenge?callback=..&username=..&ip=..&_=<ms>
//      → {"challenge":"..","ecode":0,"error":"ok",...}
//   2) hmd5   = HMAC-MD5(密码, challenge)         ← 报文里写作 {MD5}<hmd5>
//      info   = "{SRBX1}" + 换表 base64(xEncode(JSON{username,password,ip,acid,enc_ver}))
//      chkstr = token+账号+token+hmd5+token+ac_id+token+ip+token+"200"+token+"1"+token+info
//      chksum = **SHA1**(chkstr)                   ← 关键：不是 HMAC-MD5
//   3) GET {auth_url}/cgi-bin/srun_portal?callback=..&action=login&...&info=<加密串>
//      成功判据：res=="ok" 或 error=="ok" 或 ecode==0
//
// ⚠ 未实测：本机是 Dr.COM 部署，无法验证深澜路径，`--test-login` 的输出是唯一验证手段。
// ⚠ 所有查询参数都必须 URL 编码——info 是自定义 base64（含 '+' '/' '='），不编码会被
//   服务器按 form-urlencoded 解成空格，密文即损坏（与 Dr.COM 的 params 同类坑）。
// ⚠ 变体风险：srunc 走的是「明文 md5(密码) + double_stack=1」的变体，本实现按
//   ①②的 hmac-md5 + double_stack=0；若现场反馈「参数看着对但不认账号」，先试这两个开关。
class SrunAuth : public IAuth {
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

        std::string scheme_host, port;
        if (!SplitHostPort(p.auth_url, scheme_host, port, "80")) {
            r.message = "认证网址格式不正确：" + p.auth_url + "（应形如 http://10.0.0.1/）";
            return r;
        }

        const std::string base = scheme_host + ":" + port + "/cgi-bin/";
        const std::string ip = ResolveClientIp(ctx.local_ip, p.auth_url);
        const std::string account = p.username + IspSuffix(p);
        const std::string ts = std::to_string(NowMs());

        // 1) 取 challenge
        const std::string chal_url =
            base + "get_challenge?callback=jsonp&username=" + UrlEncode(account) +
            "&ip=" + UrlEncode(ip) + "&_=" + ts;

        HttpRequest creq;
        creq.url = chal_url;
        creq.method = "GET";
        creq.timeout_ms = 8000;
        creq.follow_redirects = false;
        HttpResponse cresp;
        std::string err;
        if (!HttpPerform(creq, cresp, err)) {
            r.request_url = chal_url;
            r.message = "获取 challenge 失败：" + err;
            return r;
        }

        std::string token;
        {
            nlohmann::json cj;
            if (ParseJsonp(DecodeBody(cresp), cj)) {
                try {
                    token = cj.value("challenge", std::string{});
                } catch (...) {
                }
            }
        }
        if (token.empty()) {
            r.request_url = chal_url;
            r.message = "服务器未返回 challenge（HTTP " + std::to_string(cresp.status) +
                        "），可能不是深澜认证或接口有变";
            return r;
        }

        // 2) ac_id：真实部署多放在跳转 URL 的查询串里，其次门户页，最后退 "1"
        std::string ac_id = FindAcId(ctx.portal_url);
        std::string ac_src = "portal_url";
        if (ac_id.empty()) {
            ac_id = FindAcId(ctx.portal_body);
            ac_src = "portal_body";
        }
        if (ac_id.empty()) {
            ac_id = "1";
            ac_src = "默认值（未在 URL/页面中找到 ac_id）";
        }
        LogInfo("深澜 ac_id=" + ac_id + "（来源：" + ac_src + "）");

        // 3) 组装加密参数
        const std::string n_str = "200";
        const std::string type_str = "1";

        const std::string hmd5 = HmacMd5Hex(p.password, token);

        nlohmann::json info;
        info["username"] = account;
        info["password"] = p.password;
        info["ip"] = ip;
        info["acid"] = ac_id;
        info["enc_ver"] = "srun_bx1";
        const std::string info_enc = "{SRBX1}" + SrunXEncode(info.dump(), token);

        const std::string chk = token + account + token + hmd5 + token + ac_id + token +
                                ip + token + n_str + token + type_str + token + info_enc;
        // chksum 是 SHA1（三份独立来源一致）；早期实现（照计划原文）误用 HMAC-MD5
        const std::string chksum = Sha1Hex(chk);

        // 4) 登录
        r.request_url = base + "srun_portal?callback=jsonp&action=login" +
                        "&username=" + UrlEncode(account) +
                        "&password=" + UrlEncode("{MD5}" + hmd5) +
                        "&os=Windows&name=Windows&nas_ip=" +
                        "&double_stack=0" +
                        "&chksum=" + UrlEncode(chksum) +
                        "&info=" + UrlEncode(info_enc) +
                        "&ac_id=" + UrlEncode(ac_id) +
                        "&ip=" + UrlEncode(ip) +
                        "&n=" + n_str + "&type=" + type_str +
                        "&captchaVal=&_=" + ts;
        r.request_body = "(GET，无请求体；info 为 xEncode 加密串，已打码)";

        HttpRequest req;
        req.url = r.request_url;
        req.method = "GET";
        req.timeout_ms = 8000;
        req.follow_redirects = false;

        HttpResponse resp;
        if (!HttpPerform(req, resp, err)) {
            r.message = "登录请求发送失败：" + err;
            return r;
        }

        const std::string text = DecodeBody(resp);
        LogInfo("深澜登录响应 HTTP " + std::to_string(resp.status) + "，正文 " +
                std::to_string(text.size()) + " 字节");

        if (resp.status >= 400) {
            r.message = "服务器返回错误状态码 " + std::to_string(resp.status) +
                        "，将由网络探测确认实际状态";
            return r;
        }

        nlohmann::json j;
        if (ParseJsonp(text, j)) {
            try {
                const std::string error = j.value("error", std::string{});
                const std::string res = j.value("res", std::string{});
                const std::string msg = j.value("error_msg", std::string{});
                // 真实字段是 ecode（深大响应样本），code 仅作旧版兜底
                int ecode = -1;
                if (j.contains("ecode"))
                    ecode = j["ecode"].is_number()
                                ? j["ecode"].get<int>()
                                : StrictInt(j["ecode"].get<std::string>(), -1);
                if (ecode == -1 && j.contains("code"))
                    ecode = j["code"].is_number()
                                ? j["code"].get<int>()
                                : StrictInt(j["code"].get<std::string>(), -1);

                if (res == "ok" || error == "ok" || ecode == 0) {
                    r.success = true;
                    std::string suc = j.value("suc_msg", std::string{});
                    r.message = suc.empty() ? "服务器返回登录成功（深澜认证）"
                                            : "服务器返回登录成功（深澜认证）：" +
                                                  RedactSecrets(suc);
                    return r;
                }

                r.success = false;
                r.message = "服务器返回登录失败：";
                if (!msg.empty()) r.message += RedactSecrets(msg);
                else if (!error.empty()) r.message += error;
                else r.message += "（无消息）";
                if (ecode != -1) r.message += "（ecode=" + std::to_string(ecode) + "）";
                return r;
            } catch (...) {
                // 落到下面统一处理
            }
        }

        r.message = "无法解析服务器响应，将由网络探测确认实际状态";
        return r;
    }

    const char* Name() const override { return "SrunAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeSrunAuth() { return std::make_unique<SrunAuth>(); }

}  // namespace cnk
