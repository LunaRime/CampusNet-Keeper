#pragma once
#include <string>

namespace cnk {

// AES-128-ECB + PKCS7（密钥为 16 字节的 UTF-8 字节序列），输出 Base64。
// 用于门户新式 API 的加密参数（2026-10-16 实测：门户页 a41.js 的 util.aes_en，
// 密钥与模式均取自线上脚本；见 docs/evidence/README.md「新式 API 登录流程」）。
bool AesEcbPkcs7EncryptB64(const std::string& key, const std::string& plain,
                           std::string& out_b64);

// 上述的逆运算（供模拟器与测试校验用）。去填充失败（密钥不对/密文被改）返回 false。
bool AesEcbPkcs7DecryptB64(const std::string& key, const std::string& b64,
                           std::string& out_plain);

// 通用 Base64（标准字母表，不换行）。
std::string Base64Encode(const std::string& bytes);
bool Base64Decode(const std::string& b64, std::string& out);

// ---- 便携密码导出（任务 27：跨机迁移靠用户口令，不靠放宽 DPAPI）----

// 密码学随机字节（BCryptGenRandom，系统首选 RNG）。失败返回空串。
std::string RandomBytes(size_t n);

// PBKDF2-HMAC-SHA256。passphrase 为空 / salt 为空 / iters<=0 / dkLen==0 一律失败。
bool Pbkdf2Sha256(const std::string& passphrase, const std::string& salt, int iters,
                  size_t dkLen, std::string& out);

// AES-256-GCM（key 必须 32 字节、nonce 12 字节）。tag 固定 16 字节。
// 解密时 tag 不匹配（口令错 / 文件被改）返回 false，plain 留空。
bool AesGcmEncrypt(const std::string& key, const std::string& nonce,
                   const std::string& aad, const std::string& plain,
                   std::string& cipher, std::string& tag);
bool AesGcmDecrypt(const std::string& key, const std::string& nonce,
                   const std::string& aad, const std::string& cipher,
                   const std::string& tag, std::string& plain);

}  // namespace cnk
