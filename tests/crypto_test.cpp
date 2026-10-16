#include "doctest.h"
#include "core/Crypto.h"

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
