#include "crypto/SrunXEncode.h"

#include <cstdint>
#include <vector>

namespace cnk {

namespace {

// 深澜自定义码表（与标准 base64 不同）。
// ⚠ 2026-10-16：**计划文档里给的那张表是坏表**（`...Uq/3DLbfKw...`，大写 D）——
// 'L' 出现两次、缺 'd' 与 'l'，64 个字符不是有效置换，照抄会静默产出错误密文。
// 这里用的是经两个独立来源确认的正确表：
//   ① 置换有效性：小写 26 + 大写 26 + 数字 10 + 符号 2 = 64；
//   ② srunc（Rust 深澜客户端，MIT）的 ALPHABET 常量与之逐字节一致。
const char kSrunTable[] =
    "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";

// 按 4 字节小端打包为 uint32；include_length 为真时追加密文前的原长度
std::vector<uint32_t> PackBytes(const std::string& s, bool include_length) {
    std::vector<uint32_t> v;
    const size_t n = s.size();
    for (size_t i = 0; i < n; i += 4) {
        uint32_t word = 0;
        for (size_t j = 0; j < 4 && i + j < n; ++j)
            word |= (uint32_t)(unsigned char)s[i + j] << (8 * j);
        v.push_back(word);
    }
    if (include_length) v.push_back((uint32_t)n);
    return v;
}

std::string UnpackBytes(const std::vector<uint32_t>& v) {
    std::string out;
    out.reserve(v.size() * 4);
    for (uint32_t word : v)
        for (int j = 0; j < 4; ++j) out += (char)((word >> (8 * j)) & 0xFF);
    return out;
}

// TEA 变体混淆（深澜协议公开实现的标准形态）
std::vector<uint32_t> SrunCore(std::vector<uint32_t> v, std::vector<uint32_t> k) {
    if (v.size() < 2) return v;  // 单块无混淆可言（先判长度再算 n，避免无符号下溢）
    const size_t n = v.size() - 1;
    if (k.size() < 4) k.resize(4, 0);  // 短 key 补零（与参考实现中越界取 0 等价）

    uint32_t z = v[n], y = v[0];
    const uint32_t delta = 0x9E3779B9;
    uint32_t d = 0;
    size_t rounds = (size_t)(6 + 52 / (n + 1));

    while (rounds-- > 0) {
        d += delta;
        const uint32_t e = (d >> 2) & 3;
        for (size_t p = 0; p < n; ++p) {
            y = v[p + 1];
            uint32_t m = (z >> 5) ^ (y << 2);
            m += ((y >> 3) ^ (z << 4)) ^ (d ^ y);
            m += k[(p & 3) ^ e] ^ z;
            z = v[p] = v[p] + m;
        }
        y = v[0];
        uint32_t m = (z >> 5) ^ (y << 2);
        m += ((y >> 3) ^ (z << 4)) ^ (d ^ y);
        m += k[(n & 3) ^ e] ^ z;
        z = v[n] = v[n] + m;
    }
    return v;
}

}  // namespace

std::string SrunBase64(const std::string& in) {
    if (in.empty()) return {};
    // 用 uint32 累加：位移回绕有定义；取低 6 位的结果与参考实现（JS 有符号
    // 移位再 &0x3F）一致——移位量最小 2，32-bit 的符号扩展不影响低 6 位。
    std::string out;
    uint32_t val = 0;
    int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) | c;
        bits += 8;
        while (bits >= 0) {
            out += kSrunTable[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += kSrunTable[((val << 8) >> (bits + 8)) & 0x3F];

    // 补 padding 到 4 的倍数：三个能实际登录的参考实现（srunc、bitgateway-client、
    // 深大 Python 脚本）都输出带 '=' 的标准 padding，故以它们为准（'=' 不在码表内，
    // 原样保留）。
    const size_t padded = ((in.size() + 2) / 3) * 4;
    while (out.size() < padded) out += '=';
    return out;
}

std::string SrunXEncode(const std::string& data, const std::string& key) {
    if (data.empty()) return {};

    std::vector<uint32_t> v = PackBytes(data, true);
    std::vector<uint32_t> k = PackBytes(key, false);

    v = SrunCore(std::move(v), std::move(k));
    return SrunBase64(UnpackBytes(v));
}

}  // namespace cnk
