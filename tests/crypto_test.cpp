#include "doctest.h"
#include "core/Crypto.h"
#include "crypto/Hash.h"
#include "crypto/SrunXEncode.h"

TEST_CASE("AES-128-ECB/PKCS7 加密与线上实现一致（固定向量）") {
    // 向量由线上同款实现（CryptoJS AES-ECB + 门户密钥）生成，钉住兼容性
    const std::string key = "5c1d5ad4dea0e8dd";
    std::string out;
    REQUIRE(cnk::AesEcbPkcs7EncryptB64(key, "{\"a\":1}", out));
    CHECK(out == "LrzOaRNV4PCVDAXDXFgBnw==");
    REQUIRE(cnk::AesEcbPkcs7EncryptB64(key, "0123456789abcdef", out));
    CHECK(out == "qWUrFL1s8EI+FswICPx0d98WPO0KsSiAzNOF2xH6WX8=");
    REQUIRE(cnk::AesEcbPkcs7EncryptB64(key, "中文@#", out));
    CHECK(out == "2rEslngW2QQgyLtzZBbEog==");
    REQUIRE(cnk::AesEcbPkcs7EncryptB64(key, "", out));
    CHECK(out == "3xY87QqxKIDM04XbEfpZfw==");
}

TEST_CASE("AES 解密回环与错误密钥拒绝") {
    const std::string key = "5c1d5ad4dea0e8dd";
    const std::string plain = "{\"user_password\":\"p@ss\"}";
    std::string b64, back;
    REQUIRE(cnk::AesEcbPkcs7EncryptB64(key, plain, b64));
    REQUIRE(cnk::AesEcbPkcs7DecryptB64(key, b64, back));
    CHECK(back == plain);

    // 错误密钥：去填充校验应拒绝（该向量为固定输入，结果确定）
    std::string junk;
    CHECK_FALSE(cnk::AesEcbPkcs7DecryptB64("0000000000000000", b64, junk));
    // 非法密钥长度直接失败
    CHECK_FALSE(cnk::AesEcbPkcs7DecryptB64("shortkey", b64, junk));
}

TEST_CASE("Base64 编解码回环") {
    const std::string b64 = cnk::Base64Encode("hello world");
    CHECK(b64 == "aGVsbG8gd29ybGQ=");
    std::string back;
    REQUIRE(cnk::Base64Decode(b64, back));
    CHECK(back == "hello world");

    std::string empty;
    REQUIRE(cnk::Base64Decode("", empty));
    CHECK(empty.empty());
}

// ===== 任务 20：BCrypt 哈希工具与深澜 xEncode =====

TEST_CASE("MD5 标准测试向量") {
    CHECK(cnk::Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(cnk::Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(cnk::Md5Hex("The quick brown fox jumps over the lazy dog") ==
          "9e107d9d372bb6826bd81d3542a419d6");
}

TEST_CASE("SHA1 标准测试向量") {
    CHECK(cnk::Sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(cnk::Sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("HMAC-MD5 标准测试向量（RFC 2202 case 1）") {
    const std::string key(16, '\x0b');
    CHECK(cnk::HmacMd5Hex("Hi There", key) == "9294727a3638bb1c13f48ef8158bfc9d");
}

TEST_CASE("深澜自定义 base64：非标准码表、标准 padding") {
    // 标准 base64("abc") = "YWJj"；把每个字符按「标准字母表下标 → 深澜码表同下标字符」
    // 映射：Y(24)→Z、W(22)→a、J(9)→R、j(35)→k ⇒ "ZaRk"。
    // 该结论由两个独立实现交叉确认（门户 JS 写法 与 srunc 的 quirk_base64，150 组用例零差异）。
    // ⚠ 计划文档里写的 "LVoJ" 是错的（其码表也是坏表，见 SrunXEncode.cpp 注记）。
    CHECK(cnk::SrunBase64("abc") == "ZaRk");
    // padding：三个能实际登录的参考实现（srunc / bitgateway-client / 深大 Python 脚本）
    // 都输出带 '=' 的标准 padding，故以它们为准
    CHECK(cnk::SrunBase64("a") == "Z+==");
    CHECK(cnk::SrunBase64("ab") == "Za2=");
    CHECK(cnk::SrunBase64("") == "");
}

TEST_CASE("SrunXEncode 与独立参考实现导出的向量一致") {
    // 向量由 tools/srun_xencode_oracle.js 生成（两个独立参考实现交叉验证通过）。
    // 深澜没有公开官方向量，这组向量是当前可得的最强正确性依据。
    // ⚠ 非 ASCII 输入的口径：本实现按 **UTF-8 字节** 打包（与 srunc/bitgateway/Python
    //   客户端一致）；门户自己的 JS 用 charCodeAt（等价 UTF-16 码元），两者对纯 ASCII
    //   完全等价，只对非 ASCII 分叉——已记入 README「已知限制」。
    CHECK(cnk::SrunXEncode("abc", "key") == "sANj/t6/ajv=");
    CHECK(cnk::SrunXEncode("1", "k") == "qJ58nwPHLo2=");
    CHECK(cnk::SrunXEncode("hello", "1234567890") == "SgOJLr+dS7zWHacY");
    CHECK(cnk::SrunXEncode("中文密码@#", "token") == "CaYqvwnn9WNnD2qK/t8IU+mpnKL=");
    // 登录 info 的真实形态（字段按 nlohmann 的字典序）
    const std::string info =
        "{\"acid\":\"1\",\"enc_ver\":\"srun_bx1\",\"ip\":\"10.0.0.5\","
        "\"password\":\"pw\",\"username\":\"alice@telecom\"}";
    CHECK(cnk::SrunXEncode(info, "simchallenge1") ==
          "np8Bn9bUf7CiInoy74nbZP2gtxdjz/WSXxciHJT6zyIy1TA/J2DI+nxjHlMfFeTmXogJkpEFlcZOFGfVCtR8"
          "mIYjnWXmo13EvA/gBMIswZw6TaT6S6D9zDgWMd+q+sva");
}

TEST_CASE("SrunXEncode 边界：空输入、稳定性、不同 key 不同结果") {
    CHECK(cnk::SrunXEncode("", "key") == "");
    const std::string a = cnk::SrunXEncode("hello", "1234567890");
    CHECK(a == cnk::SrunXEncode("hello", "1234567890"));
    CHECK(a != cnk::SrunXEncode("hello", "abcdefghij"));
    CHECK(a != cnk::SrunXEncode("hellp", "1234567890"));
}
