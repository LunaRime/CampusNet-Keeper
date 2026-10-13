#include "doctest.h"
#include "core/Encoding.h"
#include "core/Http.h"
#include <windows.h>

namespace {

// 该字节串能否作为合法 UTF-8 读取
bool IsValidUtf8(const std::string& bytes) {
    if (bytes.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                               (int)bytes.size(), nullptr, 0) > 0;
}

}  // namespace

TEST_CASE("ParseCharset 处理各种写法") {
    CHECK(cnk::ParseCharset("text/html; charset=gbk") == "gbk");
    CHECK(cnk::ParseCharset("text/html;charset=UTF-8") == "utf-8");
    CHECK(cnk::ParseCharset("text/html; charset=\"gb2312\"") == "gb2312");
    CHECK(cnk::ParseCharset("text/html; Charset=GBK ") == "gbk");
    CHECK(cnk::ParseCharset("text/html") == "");
    CHECK(cnk::ParseCharset("") == "");
}

TEST_CASE("ParseCharset 处理引号变体与参数位置边界") {
    // 单引号包裹与双引号同等对待
    CHECK(cnk::ParseCharset("text/html; charset='gbk'") == "gbk");
    // charset 不在第一个参数
    CHECK(cnk::ParseCharset("text/html; boundary=x; charset=gbk") == "gbk");
    // charset 后还有其他参数
    CHECK(cnk::ParseCharset("text/html; charset=gbk; foo=bar") == "gbk");
    // 名称中含 charset 的其他参数不得误命中
    CHECK(cnk::ParseCharset("text/html; xcharset=gbk") == "");
}

TEST_CASE("ToUtf8 把 GBK 正确转成 UTF-8") {
    // "学号" 的 GBK 编码
    const std::string gbk = "\xD1\xA7\xBA\xC5";
    CHECK(cnk::ToUtf8(gbk, "gbk") == "学号");
    CHECK(cnk::ToUtf8(gbk, "gb2312") == "学号");
    CHECK(cnk::ToUtf8(gbk, "GBK") == "学号");
}

TEST_CASE("ToUtf8 对 UTF-8 输入原样返回") {
    CHECK(cnk::ToUtf8("学号", "utf-8") == "学号");
    CHECK(cnk::ToUtf8("学号", "") == "学号");
}

TEST_CASE("ToUtf8 对非法字节序列不崩溃且输出合法 UTF-8") {
    std::string junk = "\xFF\xFE\x80\x81\x90";
    auto r = cnk::ToUtf8(junk, "gbk");
    CHECK(r.size() > 0);     // 不崩、有输出
    CHECK(IsValidUtf8(r));   // 且输出可作 UTF-8 读取（系统将非法字节映射为替换字符）
}

TEST_CASE("DecodeBody 按响应头字符集自动解码") {
    // spec §9.1 场景：首页声明 GBK、eportal 声明 UTF-8
    cnk::HttpResponse gbk_resp;
    gbk_resp.content_type = "text/html; charset=gbk";
    gbk_resp.body = "\xD1\xA7\xBA\xC5";  // "学号" 的 GBK 字节
    CHECK(cnk::DecodeBody(gbk_resp) == "学号");

    cnk::HttpResponse utf8_resp;
    utf8_resp.content_type = "application/json; charset=utf-8";
    utf8_resp.body = "{\"msg\":\"成功\"}";
    CHECK(cnk::DecodeBody(utf8_resp) == "{\"msg\":\"成功\"}");
}
