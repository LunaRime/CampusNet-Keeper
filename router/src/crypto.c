/* AES-128-ECB + PKCS7 + Base64、Base64、URL 编码、JSON 转义。
 *
 * 为什么要自己写 AES：路由器上没有 OpenSSL 开发库（只有运行时也会多占 1MB Flash，
 * 且 openssl-util 的 CLI 方式每次登录要 fork 一个进程）。这里只需要「加密 300 字节」
 * 一个功能，纯 C 表实现约 2KB 代码，常驻内存可忽略。
 *
 * 正确性用 FIPS-197 标准向量 + 与本项目 Windows 版（BCrypt 实现）逐字节交叉校验，
 * 见 tests/test_aes.c 与 tests/aes_crosscheck.cpp。
 */
#include "cnk.h"
#include <string.h>

/* ---------------- AES-128 ---------------- */

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

/* GF(2^8) 乘法：用移位加法而非查表，少两张表、也不会写错 */
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    int i;
    for (i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

static void key_expand(const uint8_t key[16], uint8_t rk[176]) {
    uint8_t rcon = 0x01;
    int i, j;
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4];
        t[0] = rk[i - 4];
        t[1] = rk[i - 3];
        t[2] = rk[i - 2];
        t[3] = rk[i - 1];
        if (i % 16 == 0) {
            uint8_t first = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[first];
            rcon = xtime(rcon);
        }
        for (j = 0; j < 4; ++j) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk) {
    int i;
    for (i = 0; i < 16; ++i) s[i] ^= rk[i];
}

static void sub_bytes(uint8_t s[16]) {
    int i;
    for (i = 0; i < 16; ++i) s[i] = SBOX[s[i]];
}

static void shift_rows(uint8_t s[16]) {
    uint8_t t;
    /* 行 1 左移 1 */
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    /* 行 2 左移 2 */
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    /* 行 3 左移 3（等价右移 1） */
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void mix_columns(uint8_t s[16]) {
    int c;
    for (c = 0; c < 4; ++c) {
        uint8_t *p = s + 4 * c;
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        p[1] = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        p[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        p[3] = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    int round;
    memcpy(s, in, 16);
    add_round_key(s, rk);
    for (round = 1; round <= 9; ++round) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, rk + round * 16);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, rk + 160);
    memcpy(out, s, 16);
}

/* ---------------- Base64 ---------------- */

static const char B64TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap) {
    size_t i, o = 0;
    if (cap < 4) return 0;
    for (i = 0; i + 2 < len; i += 3) {
        if (o + 4 >= cap) return 0;   /* 留 1 字节给 NUL */
        out[o++] = B64TAB[(in[i] >> 2) & 0x3f];
        out[o++] = B64TAB[((in[i] & 0x03) << 4) | ((in[i + 1] >> 4) & 0x0f)];
        out[o++] = B64TAB[((in[i + 1] & 0x0f) << 2) | ((in[i + 2] >> 6) & 0x03)];
        out[o++] = B64TAB[in[i + 2] & 0x3f];
    }
    if (i < len) {
        const size_t rem = len - i;
        if (o + 4 >= cap) return 0;
        out[o++] = B64TAB[(in[i] >> 2) & 0x3f];
        if (rem == 1) {
            out[o++] = B64TAB[(in[i] & 0x03) << 4];
            out[o++] = '=';
            out[o++] = '=';
        } else {
            out[o++] = B64TAB[((in[i] & 0x03) << 4) | ((in[i + 1] >> 4) & 0x0f)];
            out[o++] = B64TAB[(in[i + 1] & 0x0f) << 2];
            out[o++] = '=';
        }
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, size_t inlen, uint8_t *out, size_t cap) {
    size_t i, o = 0;
    int quad[4], n = 0;
    for (i = 0; i < inlen; ++i) {
        char c = in[i];
        int v;
        if (c == '=' ) { quad[n++] = -2; }
        else {
            v = b64_val(c);
            if (v < 0) {
                if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
                return -1;
            }
            quad[n++] = v;
        }
        if (n == 4) {
            int pad = 0;
            if (quad[2] == -2) pad = 2;
            else if (quad[3] == -2) pad = 1;
            if (pad == 2) {
                if (o + 1 > cap) return -1;
                out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
            } else if (pad == 1) {
                if (o + 2 > cap) return -1;
                out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
                out[o++] = (uint8_t)(((quad[1] & 0x0f) << 4) | (quad[2] >> 2));
            } else {
                if (o + 3 > cap) return -1;
                out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
                out[o++] = (uint8_t)(((quad[1] & 0x0f) << 4) | (quad[2] >> 2));
                out[o++] = (uint8_t)(((quad[2] & 0x03) << 6) | quad[3]);
            }
            n = 0;
            if (pad) break;
        }
    }
    return (int)o;
}

/* ---------------- AES-128-ECB + PKCS7 + Base64 ---------------- */

void aes128_ecb_encrypt_raw(const uint8_t key[16], const uint8_t *in, size_t len,
                            uint8_t *out) {
    uint8_t rk[176];
    size_t i;
    key_expand(key, rk);
    for (i = 0; i + 16 <= len; i += 16)
        aes128_encrypt_block(rk, in + i, out + i);
}

void aes128_ecb_pkcs7_b64(const uint8_t key[16], const char *plain,
                          char *out, size_t cap) {
    uint8_t buf[1024];
    uint8_t enc[1024];
    size_t len = strlen(plain);
    size_t pad, i;

    if (len > sizeof(buf) - 16) {          /* 载荷正常 300 字节左右，超限直接失败 */
        if (cap) out[0] = '\0';
        return;
    }
    memcpy(buf, plain, len);
    pad = 16 - (len % 16);                 /* 满块时补一整块 0x10，符合 PKCS7 */
    for (i = 0; i < pad; ++i) buf[len + i] = (uint8_t)pad;
    len += pad;

    aes128_ecb_encrypt_raw(key, buf, len, enc);
    b64_encode(enc, len, out, cap);
}

/* ---------------- URL 编码 ---------------- */

static int is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

size_t url_encode(const char *in, size_t len, char *out, size_t cap) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t i, o = 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (is_unreserved(c)) {
            if (o + 1 >= cap) return 0;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) return 0;
            out[o++] = '%';
            out[o++] = HEX[(c >> 4) & 0x0f];
            out[o++] = HEX[c & 0x0f];
        }
    }
    if (cap) out[o] = '\0';
    return o;
}

/* ---------------- JSON 字符串转义 ---------------- */

size_t json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p; ++p) {
        unsigned char c = *p;
        const char *rep = NULL;
        char tmp[8];
        size_t need;
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b"; break;
            case '\f': rep = "\\f"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char HEX[] = "0123456789abcdef";
                    tmp[0] = '\\'; tmp[1] = 'u'; tmp[2] = '0'; tmp[3] = '0';
                    tmp[4] = HEX[(c >> 4) & 0x0f];
                    tmp[5] = HEX[c & 0x0f];
                    tmp[6] = '\0';
                    rep = tmp;
                }
                break;
        }
        need = rep ? strlen(rep) : 1;
        if (o + need + 1 > cap) { if (cap) out[0] = '\0'; return 0; }
        if (rep) { memcpy(out + o, rep, need); o += need; }
        else out[o++] = (char)c;
    }
    if (cap) out[o] = '\0';
    return o;
}
