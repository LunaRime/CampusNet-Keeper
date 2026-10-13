#include "doctest.h"
#include "auth/Detector.h"
#include <windows.h>

namespace {

// 把 UTF-8 文本转成 GBK 字节，用于构造「GBK 页面 + 声明 charset」的真实场景
std::string ToGbk(const std::string& utf8) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wn <= 0) return {};
    std::wstring wide((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), wn);
    int n = WideCharToMultiByte(936, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(936, 0, wide.data(), wn, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

TEST_CASE("Server 头为 DrcomServer 判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.server = "DrcomServer1.0";
    r.body = "<html>whatever</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("同时含 DrcomServer 与 /eportal/ 时判定为 Dr.COM 而非锐捷") {
    // 这正是实测环境的情形，是最容易误判的组合
    cnk::HttpResponse r;
    r.server = "DrcomServer1.0";
    r.body = "<script>authloginpath='/eportal/?c=ACSetting&a=Login';"
             "authloginport=801;</script>";
    r.effective_url = "http://10.0.0.1:801/eportal/";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("页面含中文「哆点」判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.body = "<script>/** 哆点参数 start */</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("GBK 页面按声明解码后仍能识别哆点指纹") {
    // 真实首页是 GBK 字节 + charset=gbk 头；识别链依赖 DecodeBody 先转 UTF-8
    cnk::HttpResponse r;
    r.content_type = "text/html; charset=gbk";
    r.body = ToGbk("<script>/** 哆点参数 start */</script>");
    REQUIRE(r.body.find("哆点") == std::string::npos);  // 自检：正文确实不是 UTF-8
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("epHTTPPort 不构成 Dr.COM 指纹") {
    // 该变量真实存在，但只在首页引用的 a41.js 里，探测拿到的 HTML 看不到它。
    // 若把它当指纹，真实环境永远不命中 —— 这条测试防止它被改回来。
    cnk::HttpResponse r;
    r.server = "nginx";
    r.body = "<script>epHTTPPort=801;</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Unknown);
}

TEST_CASE("页面含 Dr.COMWebLoginID_ 判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.body = "<!--Dr.COMWebLoginID_1.htm-->";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("页面含 0MKKey 判定为 Dr.COM") {
    cnk::HttpResponse r;
    r.body = "<input name=\"0MKKey\" value=\"123456\">";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Drcom);
}

TEST_CASE("无 Dr.COM 特征但含 /eportal/ 判定为锐捷") {
    cnk::HttpResponse r;
    r.server = "nginx";
    r.body = "<script>jsVersion='4.1.3';</script>";
    r.effective_url = "http://10.0.0.1/eportal/";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Eportal);
}

TEST_CASE("含 srun_portal 判定为深澜") {
    cnk::HttpResponse r;
    r.body = "<script>var api='/cgi-bin/srun_portal';</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Srun);
}

TEST_CASE("含 ac_id 与 callback 判定为深澜") {
    cnk::HttpResponse r;
    r.body = "<script>var ac_id='1'; var callback='jsonp';</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Srun);
}

TEST_CASE("含 iPortal 判定为华为") {
    cnk::HttpResponse r;
    r.body = "<html>iPortal 认证</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Huawei);
}

TEST_CASE("URL 含 /portal/login 判定为华为") {
    // spec §7 序 5 的对象是「URL 含 /portal/login」，URL-only 场景不得漏判
    cnk::HttpResponse r;
    r.effective_url = "http://10.0.0.1/portal/login?user=1";
    r.body = "<html></html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Huawei);
}

TEST_CASE("multiportal 等含 iportal 子串的词不误判为华为") {
    // 弱指纹必须大小写敏感匹配，避免 multiportal 之类子串误命中
    cnk::HttpResponse r;
    r.body = "<html>multiportal gateway</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Unknown);
}

TEST_CASE("含 wlanuserip/nasip 与 iPortal 时按序判定为锐捷") {
    // spec §7 顺序所致：序 4（锐捷）先于序 5（华为）。
    // 钉住该行为，任何顺序调整都会让本用例转红。
    cnk::HttpResponse r;
    r.body = "<script>wlanuserip='10.0.0.5'; nasip='10.0.0.1'; iPortal;</script>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Eportal);
}

TEST_CASE("GBK 字节未声明 charset 时中文指纹失效（当前降级行为）") {
    // 钉住当前行为：无 charset 声明 → 按 UTF-8 原样处理 → 中文指纹匹配失败。
    // 真实首页声明 charset=gbk 且有 DrcomServer 头兜底，故实际风险低。
    cnk::HttpResponse r;
    r.body = ToGbk("<script>/** 哆点参数 start */</script>");
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Unknown);
}

TEST_CASE("无任何特征判定为 Unknown") {
    cnk::HttpResponse r;
    r.body = "<html>hello</html>";
    CHECK(cnk::DetectVendor(r) == cnk::Vendor::Unknown);
}

TEST_CASE("VendorName 与 VendorFromString 互逆") {
    for (auto v : {cnk::Vendor::Drcom, cnk::Vendor::Eportal,
                   cnk::Vendor::Srun, cnk::Vendor::Huawei}) {
        CHECK(cnk::VendorFromString(cnk::VendorName(v)) == v);
    }
    CHECK(cnk::VendorFromString("auto") == cnk::Vendor::Unknown);
    CHECK(cnk::VendorFromString("garbage") == cnk::Vendor::Unknown);
}
