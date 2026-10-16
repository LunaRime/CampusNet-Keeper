#include "doctest.h"
#include "auth/AuthFactory.h"
#include "core/Url.h"
#include "crypto/Hash.h"
#include "crypto/SrunXEncode.h"
#include "json.hpp"
#include "sim/SimServer.h"

namespace {

cnk::Profile MakeSrunProfile(const std::string& base, const std::string& u,
                             const std::string& p, const std::string& preset) {
    cnk::Profile pr;
    pr.auth_url = base + "/";
    pr.username = u;
    pr.password = p;
    pr.vendor = "srun";
    pr.isp.preset = preset;
    return pr;
}

// 测试自己的百分号解码（产品侧该函数只在模拟器内部，未导出）
std::string UrlDecodeForTest(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string hex = s.substr(i + 1, 2);
            out += (char)std::strtol(hex.c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

TEST_CASE("深澜端到端：challenge → HMAC-MD5 口令 → xEncode info → chksum") {
    // 模拟器用「同 token 重新加密期望 JSON 后逐字节比对」校验 info，因此本用例
    // 端到端钉住了：字段取值、xEncode、HMAC-MD5、chksum 拼接顺序，以及查询编码。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "srun";
    o.valid_user = "alice";
    o.valid_pass = "pw";
    o.isp_suffix = "@telecom";  // IspSuffix 在 srun 下：telecom → @telecom
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeSrunProfile(srv.BaseUrl(), "alice", "pw", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    CHECK(std::string(auth->Name()) == "SrunAuth");

    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK(r.success);
    CHECK(r.message.find("深澜") != std::string::npos);
    CHECK(r.request_url.find("srun_portal") != std::string::npos);
    CHECK(r.request_url.find("ac_id=") != std::string::npos);
    CHECK(srv.LoggedIn());
    srv.Stop();
}

TEST_CASE("深澜查询串必须 URL 编码（否则 info 里的 + / 会被解成空格而损坏）") {
    // info = "{SRBX1}" + 自定义 base64；'{' '}' '+' '/' 都必须转义。
    // 用编码后的花括号做确定性断言（密文本身每次随机，不能直接断言其内容）。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "srun";
    o.valid_user = "alice";
    o.valid_pass = "pw";
    o.isp_suffix = "@telecom";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeSrunProfile(srv.BaseUrl(), "alice", "pw", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);

    CHECK(r.request_url.find("info=%7BSRBX1%7D") != std::string::npos);
    CHECK(r.request_url.find("password=%7BMD5%7D") != std::string::npos);
    CHECK(r.request_url.find("{SRBX1}") == std::string::npos);  // 不得出现未转义形态
    srv.Stop();
}

TEST_CASE("深澜错误密码返回失败并带服务器消息") {
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "srun";
    o.valid_user = "alice";
    o.valid_pass = "right";
    o.isp_suffix = "@telecom";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeSrunProfile(srv.BaseUrl(), "alice", "wrong", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号或密码错误") != std::string::npos);
    CHECK_FALSE(srv.LoggedIn());
    srv.Stop();
}

TEST_CASE("深澜空凭据快速失败且不发请求") {
    cnk::Profile p;
    p.vendor = "srun";
    p.auth_url = "http://127.0.0.1:1/";
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("账号") != std::string::npos);
}

TEST_CASE("深澜协议装配的请求级向量（独立复算，防止与服务端共享同一错误公式）") {
    // 这条用例的价值：**不调用产品代码里的公式**，而是在测试里按三份独立来源
    //（深大 srun 逆向、srunc、bitgateway-client）的写法重算一遍：
    //   hmd5   = HMAC-MD5(密码, token)
    //   info   = "{SRBX1}" + xEncode(JSON{username,password,ip,acid,enc_ver}, token)
    //   chksum = SHA1(token+账号+token+hmd5+token+ac_id+token+ip+token+"200"+token+"1"+token+info)
    // 早期实现把 chksum 写成 HMAC-MD5，而模拟器用同一个函数算期望值 → 端到端恒真、
    // 真实部署 100% 失败。若没有这条独立复算，这个 bug 永远不会被测出来。
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "srun";
    o.valid_user = "alice";
    o.valid_pass = "pw";
    o.isp_suffix = "@telecom";
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeSrunProfile(srv.BaseUrl(), "alice", "pw", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    // 真实形态：ac_id 在**跳转 URL 的查询串**里（如 net.szu.edu.cn/?ac_id=12）
    ctx.portal_url = srv.BaseUrl() + "/?ac_id=12";
    auto r = auth->Login(p, ctx);
    CHECK(r.success);

    const std::string token = srv.LastChallenge();
    REQUIRE_FALSE(token.empty());

    // 从请求 URL 里取出各字段（这是"客户端实际发出去的东西"）
    auto param = [&r](const std::string& key) {
        const std::string needle = "&" + key + "=";
        const size_t b = r.request_url.find(needle);
        if (b == std::string::npos) return std::string{};
        const size_t v = b + needle.size();
        const size_t e = r.request_url.find('&', v);
        return UrlDecodeForTest(
            r.request_url.substr(v, e == std::string::npos ? std::string::npos : e - v));
    };
    const std::string account = param("username");
    const std::string ip = param("ip");
    const std::string ac_id = param("ac_id");
    const std::string info = param("info");
    const std::string chksum = param("chksum");
    const std::string pass_field = param("password");

    CHECK(account == "alice@telecom");
    CHECK(ac_id == "12");           // 必须取自跳转 URL，而不是缺省的 "1"
    CHECK(param("n") == "200");
    CHECK(param("type") == "1");
    CHECK(param("double_stack") == "0");
    CHECK(param("action") == "login");

    // 1) 口令字段 = {MD5} + HMAC-MD5(密码, token)
    const std::string hmd5 = cnk::HmacMd5Hex("pw", token);
    CHECK(pass_field == "{MD5}" + hmd5);

    // 2) info 前缀与可重算性（同一 token 加密同样的 JSON 必须逐字节相同）
    CHECK(info.rfind("{SRBX1}", 0) == 0);
    nlohmann::json expected;
    expected["username"] = account;
    expected["password"] = "pw";
    expected["ip"] = ip;
    expected["acid"] = ac_id;
    expected["enc_ver"] = "srun_bx1";
    CHECK(info == "{SRBX1}" + cnk::SrunXEncode(expected.dump(), token));

    // 3) chksum 是 SHA1（不是 HMAC-MD5！）
    const std::string chkstr = token + account + token + hmd5 + token + ac_id + token +
                               ip + token + "200" + token + "1" + token + info;
    CHECK(chksum == cnk::Sha1Hex(chkstr));
    CHECK(chksum != cnk::HmacMd5Hex(chkstr, token));
    CHECK(chksum.size() == 40);  // SHA1 十六进制

    srv.Stop();
}

TEST_CASE("深澜拿不到 challenge 时给出明确失败（不是深澜或接口有变）") {
    // 指向一个只会 404 的地址：应报「未返回 challenge」而不是含糊的「登录失败」
    cnk::SimServer srv;
    cnk::SimOptions o;
    o.vendor = "drcom";  // 该模拟器不提供 /cgi-bin/get_challenge
    std::string err;
    REQUIRE(srv.Start(o, err));

    auto p = MakeSrunProfile(srv.BaseUrl(), "alice", "pw", "telecom");
    auto auth = cnk::CreateAuth(cnk::Vendor::Srun, p);
    cnk::NetContext ctx;
    auto r = auth->Login(p, ctx);
    CHECK_FALSE(r.success);
    CHECK(r.message.find("challenge") != std::string::npos);
    srv.Stop();
}
