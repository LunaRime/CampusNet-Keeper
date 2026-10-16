#include "auth/AuthFactory.h"
#include "core/Http.h"
#include "core/Encoding.h"
#include "core/Url.h"
#include "core/Crypto.h"
#include "core/Log.h"
#include "json.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace cnk {

namespace {

// ============================================================================
// 门户新式 API 常量（2026-10-16 以真实浏览器 HAR 逆向实测，见
// docs/evidence/README.md「新式 API 登录流程」）：
//   真实登录 = GET :801/eportal/portal/login?callback=..&params=<AES(JSON)>，
//   AES = AES-128-ECB + PKCS7 + Base64（密钥取自门户页 a41.js util.aes_en 默认参数）；
//   登录前必须先经 loadConfig 获得页面令牌 rcn（缺 rcn 服务器报「当前页面已超时」）。
// ============================================================================
const char kAesKey[] = "5c1d5ad4dea0e8dd";
const char kAccountPrefix[] = ",0,";  // 实测 user_account 前缀，照实复刻
// is_page_new 无协议含义：门户脚本取 Math.floor(Math.random()*10000+500)（实测两次
// 真实登录分别为 558 与 9997，服务器均接受），故取同区间内的固定值即可。
constexpr int kIsPageNew = 558;

int RandomInt(int lo, int hi) {
    static const bool seeded = [] {
        std::srand((unsigned)std::time(nullptr));
        return true;
    }();
    (void)seeded;
    return lo + std::rand() % (hi - lo + 1);
}

// JSON 字符串值转义（仅处理 JSON 语法必需字符）
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// 从 JSONP（前缀 + ({...});）截出 JSON 主体并解析。
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

// 登录载荷（字段与顺序照实测 HAR 复刻）。pw 单独传入，便于生成打码版展示文本。
std::string BuildLoginPayload(const std::string& account, const std::string& pw,
                              const std::string& ip, const std::string& rcn) {
    std::string s;
    s.reserve(384);
    s += "{\"login_method\":1";
    s += ",\"user_account\":\"" + JsonEscape(kAccountPrefix + account) + "\"";
    s += ",\"user_password\":\"" + JsonEscape(pw) + "\"";
    s += ",\"wlan_user_ip\":\"" + JsonEscape(ip) + "\"";
    s += ",\"wlan_user_ipv6\":\"\",\"wlan_user_mac\":\"000000000000\"";
    s += ",\"wlan_ac_ip\":\"\",\"wlan_ac_name\":\"\"";
    s += ",\"jsVersion\":\"4.2.1\",\"login_t\":\"0\",\"js_status\":\"0\"";
    s += ",\"is_page\":\"1\",\"is_page_new\":" + std::to_string(kIsPageNew);
    s += ",\"terminal_type\":1,\"lang\":\"zh-cn\"";
    s += ",\"rcn\":\"" + JsonEscape(rcn) + "\"}";
    return s;
}

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

// 老式协议（同源 c=ACSetting）。2026-10-16 端口修订后仅作为**回退通道**：
// 新式接口在旧版本部署上不存在时使用。本机实测该通道恒回 userid error2
// （该部署已不处理老协议的真实认证），故不能作为首选；其响应标记判定保留，
// 供旧部署使用。另注：首轮把该通道建到 :801 是错误假设，已改为门户同源端口。
LoginResult LegacyLogin(const Profile& p, const std::string& account,
                        const std::string& scheme_host, const std::string& port) {
    LoginResult r;
    r.request_url = scheme_host + (port.empty() ? "" : ":" + port) +
                    "/eportal/?c=ACSetting&a=Login&url=drappall";

    std::ostringstream body;
    body << "DDDDD=" << UrlEncode(account) << "&upass=" << UrlEncode(p.password)
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
    LogInfo("Dr.COM 老协议登录响应 HTTP " + std::to_string(resp.status) + "，正文 " +
            std::to_string(page.size()) + " 字节");

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
        // 能区分「用户不存在 / 密码不匹配」等成因，是本项目的关键现场证据
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

// 新式 API 的结果包装。usable=false 表示结构性不可用（接口不存在/无 rcn/网络不通），
// 调用方应回退老协议；usable=true 时 result 即最终判定。
// api_present：loadConfig 正常应答过（接口确实存在）——用于判定回退通道的结论是否可信。
struct NewApiOutcome {
    bool usable = false;
    bool api_present = false;
    std::string note;
    LoginResult result;
};

NewApiOutcome TryNewApi(const Profile& p, const std::string& account,
                        const std::string& scheme_host, const std::string& own_port) {
    NewApiOutcome out;
    const bool https = scheme_host.compare(0, 6, "https:") == 0;
    // 新式 API 端口：auth_url 显式端口优先；否则按 scheme 用实测的 801/802
    const std::string api_port = own_port.empty() ? (https ? "802" : "801") : own_port;
    const std::string api_base = scheme_host + ":" + api_port;
    const std::string origin = scheme_host + (own_port.empty() ? "" : ":" + own_port);

    HttpRequest req;
    req.method = "GET";
    // 前置两步按短超时（局域网内通常 <1s；失败时尽快回退老协议，见 fail_mode=2 测试）
    req.timeout_ms = 3000;
    req.follow_redirects = false;
    HttpResponse resp;
    std::string err;

    // 1) chkstatus：取客户端校园网 IP（失败不致命，可留空）
    std::string client_ip;
    req.url = origin + "/drcom/chkstatus?callback=dr8001&jsVersion=4.X&v=" +
              std::to_string(RandomInt(1000, 9999)) + "&lang=zh";
    if (HttpPerform(req, resp, err) && resp.status == 200) {
        nlohmann::json j;
        if (ParseJsonp(resp.body, j) && j.contains("v46ip") && j["v46ip"].is_string())
            client_ip = j["v46ip"].get<std::string>();
    }

    // 2) loadConfig：取页面令牌 rcn（新式登录的必要前置，缺它服务器报「页面已超时」）
    req.url = api_base +
              "/eportal/portal/page/loadConfig?callback=dr8002&program_index=&wlan_vlan_id=0"
              "&wlan_user_ip=" +
              UrlEncode(Base64Encode(client_ip)) +
              "&wlan_user_ipv6=&wlan_user_ssid=&wlan_user_areaid=&wlan_ac_ip=&wlan_ap_mac=000000000000"
              "&gw_id=000000000000&jsVersion=4.X&v=" +
              std::to_string(RandomInt(1000, 9999)) + "&lang=zh";
    if (!HttpPerform(req, resp, err) || resp.status != 200) {
        out.note = "loadConfig 请求失败（HTTP " + std::to_string(resp.status) +
                   (err.empty() ? std::string() : " " + err) + "）";
        return out;
    }
    std::string rcn;
    {
        nlohmann::json lc;
        if (!ParseJsonp(resp.body, lc)) {
            out.note = "loadConfig 响应不是 JSONP（该部署可能不支持新式接口）";
            return out;
        }
        out.api_present = true;  // 接口确实存在（能给出结构化应答）
        if (lc.contains("data") && lc["data"].contains("rcn") &&
            lc["data"]["rcn"].is_string())
            rcn = lc["data"]["rcn"].get<std::string>();
        if (rcn.empty() && lc.contains("rcn") && lc["rcn"].is_string())
            rcn = lc["rcn"].get<std::string>();
        if (rcn.empty()) {
            out.note = "loadConfig 未下发 rcn（该部署可能不支持新式接口）";
            return out;
        }
    }

    // 3) AES 加密登录参数并提交
    const std::string payload = BuildLoginPayload(account, p.password, client_ip, rcn);
    std::string params_b64;
    if (!AesEcbPkcs7EncryptB64(kAesKey, payload, params_b64)) {
        out.note = "登录参数加密失败";
        return out;
    }
    req.timeout_ms = 8000;  // 登录是主操作，给足超时
    req.url = api_base + "/eportal/portal/login?callback=dr8003&params=" +
              UrlEncode(params_b64) + "&jsVersion=4.X&v=" +
              std::to_string(RandomInt(1000, 9999)) + "&lang=zh";
    if (!HttpPerform(req, resp, err) || resp.status != 200) {
        out.note = "login 请求失败（HTTP " + std::to_string(resp.status) +
                   (err.empty() ? std::string() : " " + err) + "）";
        return out;
    }
    nlohmann::json lj;
    if (!ParseJsonp(resp.body, lj)) {
        out.note = "login 响应不是 JSONP（该部署可能不支持新式接口）";
        return out;
    }

    out.usable = true;
    // 展示用信息：URL 里 params 虽是密文，但密钥公开（页面脚本内），一律不回显；
    // 请求体同理——密码、账号（含学号）与页面令牌 rcn 全部打码
    //（2026-10-16 审阅 P1：老式通道的 DDDDD 原本是打码的，新式载荷不能反而把
    //  学号与 rcn 明着打出来）
    out.result.request_url =
        api_base +
        "/eportal/portal/login?callback=dr8003&params=***&jsVersion=4.X&v=***&lang=zh";
    out.result.request_body = BuildLoginPayload("***", "***", client_ip, "***");

    int code = 0;
    std::string msg, ret_code;
    try {
        if (lj.contains("result")) {
            if (lj["result"].is_number()) code = lj["result"].get<int>();
            else if (lj["result"].is_string())
                code = std::atoi(lj["result"].get<std::string>().c_str());
        }
        if (lj.contains("msg") && lj["msg"].is_string())
            msg = lj["msg"].get<std::string>();
        if (lj.contains("ret_code")) {
            if (lj["ret_code"].is_string()) ret_code = lj["ret_code"].get<std::string>();
            else if (lj["ret_code"].is_number())
                ret_code = std::to_string(lj["ret_code"].get<long long>());
        }
    } catch (...) {
        // 保持缺省值；下面按「无有效 result」处理
    }

    if (code == 1) {
        out.result.success = true;
        out.result.vendor_code = 1;
        out.result.message = "服务器返回登录成功（Portal协议认证成功）";
        return out;
    }

    // 服务器明确报「该终端 IP 已在线」（2026-10-16 本机线上实测原样响应：
    // result=0、ret_code=2、msg="终端IP已在线！"；该判定先于凭据校验）。语义是
    // 链路已通、无需重新登录，故按成功处理，且**不能**落到下面的凭据类启发式——
    // 否则调度器会把「已在线」当成密码错误而停止重试（vendor_code=2）。
    // 必须同时含「终端」：只有「终端 IP 已在线」才说明本机链路通了；
    // 「该账号已在线」（账号在别处在线）不是本机可用的证据，不得判成功。
    if (msg.find("已在线") != std::string::npos &&
        msg.find("终端") != std::string::npos) {
        out.result.success = true;
        out.result.vendor_code = 1;  // 成功类；与 result=1 同族，保证 success⇒code≠2
        out.result.message =
            "服务器报告该终端已在线（无需重新登录）：" + RedactSecrets(msg);
        return out;
    }

    out.result.success = false;
    // 凭据类失败的启发式识别（新式接口的完整文案表尚未采齐）：必须**同时**出现
    // 「凭据词」与「否定词」才算凭据错误。只看「账号/密码/用户」会把
    // 「获取用户信息失败，请稍后重试」这类普通故障误判成密码错误，进而让调度器
    // 永久停止自动重试（2026-10-16 审阅 P1）。
    const bool mentions_cred = msg.find("密码") != std::string::npos ||
                               msg.find("账号") != std::string::npos ||
                               msg.find("用户名") != std::string::npos ||
                               msg.find("用户") != std::string::npos;
    const bool says_invalid = msg.find("错误") != std::string::npos ||
                              msg.find("不正确") != std::string::npos ||
                              msg.find("不存在") != std::string::npos ||
                              msg.find("无效") != std::string::npos;
    const bool cred_like = mentions_cred && says_invalid;
    out.result.vendor_code = cred_like ? 2 : -1;
    out.result.message =
        "服务器返回登录失败：" + (msg.empty() ? std::string("（无消息）") : RedactSecrets(msg));
    if (!ret_code.empty()) out.result.message += "（ret_code=" + ret_code + "）";
    return out;
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

        std::string scheme_host, own_port;
        if (!SplitHostPort(p.auth_url, scheme_host, own_port, "")) {
            r.message = "认证网址格式不正确：" + p.auth_url +
                        "（应形如 http://10.0.0.1/）";
            return r;
        }

        const std::string account = p.username + IspSuffix(p);

        // 首选新式 API（2026-10-16 实测的真实登录通道）
        NewApiOutcome na = TryNewApi(p, account, scheme_host, own_port);
        if (na.usable) return na.result;

        // 结构性不可用 → 回退老式协议（旧部署通道）
        LogInfo("Dr.COM 新式接口不可用（" + na.note + "），回退老式协议");
        LoginResult legacy = LegacyLogin(p, account, scheme_host, own_port);

        // 只在新式接口**确实不存在**时才采信老式通道的判定。本部署的老式通道恒回
        // msga='userid error2'（与凭据对错无关，见 docs/evidence/README.md）；若接口
        // 明明在场（loadConfig 正常）却因登录步骤故障而回退，把 error2 当「密码错误」
        // 上报会让调度器 AuthFailed + 停止重试——正是本项目踩过的那次误诊
        //（2026-10-16 审阅 P1）。此时降级为「无法判定」，交探测/退避重试裁决。
        if (legacy.vendor_code == 2 && na.api_present) {
            legacy.vendor_code = -1;
            legacy.message = "新式接口不可用（" + na.note + "）；老式回退通道返回：" +
                             legacy.message +
                             "。该通道在本部署不做真实认证，其判定不作为凭据错误依据。";
        }
        return legacy;
    }

    const char* Name() const override { return "DrcomAuth"; }
};

}  // namespace

std::unique_ptr<IAuth> MakeDrcomAuth() { return std::make_unique<DrcomAuth>(); }

}  // namespace cnk
