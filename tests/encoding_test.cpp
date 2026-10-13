#include "doctest.h"
#include "core/Encoding.h"

TEST_CASE("ParseCharset 处理各种写法") {
    CHECK(cnk::ParseCharset("text/html; charset=gbk") == "gbk");
    CHECK(cnk::ParseCharset("text/html;charset=UTF-8") == "utf-8");
    CHECK(cnk::ParseCharset("text/html; charset=\"gb2312\"") == "gb2312");
    CHECK(cnk::ParseCharset("text/html; Charset=GBK ") == "gbk");
    CHECK(cnk::ParseCharset("text/html") == "");
    CHECK(cnk::ParseCharset("") == "");
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

TEST_CASE("ToUtf8 对非法字节序列不崩溃") {
    std::string junk = "\xFF\xFE\x80\x81\x90";
    auto r = cnk::ToUtf8(junk, "gbk");
    CHECK(r.size() > 0);   // 只要不崩、能返回即可
}
