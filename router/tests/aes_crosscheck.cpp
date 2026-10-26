/* 交叉校验：本实现的 AES-128-ECB+PKCS7+Base64 与主项目（BCrypt）逐字节比对。
 *
 * 这是「路由器版真的和已验收的 Windows 版等价」的关键证据——
 * 只跑 FIPS-197 单块向量是不够的（填充、Base64、长度边界都可能出错）。
 */
#include "core/Crypto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

extern "C" {
void aes128_ecb_pkcs7_b64(const unsigned char key[16], const char *plain,
                          char *out, size_t cap);
}

int main(void) {
    const char *keys[] = {
        "5c1d5ad4dea0e8dd",
        "0000000000000000",
        "0123456789abcdef",
        "zzzzzzzzzzzzzzzz",
    };
    /* 覆盖：空串、短串、跨块边界、整块、含中文、含 JSON 需要转义的字符 */
    const char *fixed[] = {
        "",
        "a",
        "abc",
        "0123456789abcdef",
        "0123456789abcdef0",
        "{\"login_method\":1,\"user_account\":\",0,20230001@cmcc\"}",
        "\xe4\xb8\xad\xe6\x96\x87\xe5\xaf\x86\xe7\xa0\x81",
        "quote\" backslash\\ tab\t",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
    };

    int fails = 0, total = 0;
    size_t ki, i;

    for (ki = 0; ki < sizeof keys / sizeof keys[0]; ++ki) {
        std::string key(keys[ki]);
        if (key.size() != 16) { std::printf("测试自身错误：key 长度\n"); return 2; }

        for (i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) {
            std::string expect, got;
            char buf[2048];
            if (!cnk::AesEcbPkcs7EncryptB64(key, fixed[i], expect)) {
                std::printf("[FAIL] 主项目加密失败 key#%u case#%u\n", (unsigned)ki, (unsigned)i);
                ++fails;
                continue;
            }
            aes128_ecb_pkcs7_b64((const unsigned char *)key.data(), fixed[i], buf, sizeof buf);
            got = buf;
            ++total;
            if (got != expect) {
                std::printf("[FAIL] key#%u case#%u\n  expect=%s\n  got   =%s\n",
                            (unsigned)ki, (unsigned)i, expect.c_str(), got.c_str());
                ++fails;
            }
        }

        /* 随机长度 0..600，随机可见 ASCII */
        {
            int n;
            std::srand(20261026);
            for (n = 0; n < 120; ++n) {
                int len = std::rand() % 601;
                std::string plain;
                int j;
                for (j = 0; j < len; ++j) plain.push_back((char)(32 + std::rand() % 95));
                std::string expect, got;
                char buf[4096];
                if (!cnk::AesEcbPkcs7EncryptB64(key, plain, expect)) continue;
                aes128_ecb_pkcs7_b64((const unsigned char *)key.data(), plain.c_str(), buf, sizeof buf);
                got = buf;
                ++total;
                if (got != expect) {
                    std::printf("[FAIL] 随机用例 len=%d\n", len);
                    ++fails;
                }
            }
        }
    }

    std::printf("AES 交叉校验：%d 组，%d 失败\n", total, fails);
    return fails ? 1 : 0;
}
