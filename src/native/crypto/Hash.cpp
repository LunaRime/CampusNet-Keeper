#include "crypto/Hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace cnk {

namespace {

std::string ToHex(const std::vector<BYTE>& bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (BYTE b : bytes) {
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }
    return out;
}

// 单次哈希（with_hmac 时使用 HMAC，key 作为 HMAC 密钥）。
bool HashWith(BCRYPT_ALG_HANDLE alg, const std::string& data, bool with_hmac,
              const std::string& key, std::vector<BYTE>& out) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS st = BCryptCreateHash(alg, &hash, nullptr, 0,
                                   with_hmac ? (PUCHAR)key.data() : nullptr,
                                   with_hmac ? (ULONG)key.size() : 0, 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    st = BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptDestroyHash(hash);
        return false;
    }

    DWORD hash_len = 0, cb = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len,
                                          sizeof(hash_len), &cb, 0))) {
        BCryptDestroyHash(hash);
        return false;
    }
    out.resize(hash_len);
    st = BCryptFinishHash(hash, out.data(), hash_len, 0);
    BCryptDestroyHash(hash);
    return BCRYPT_SUCCESS(st);
}

std::string HexOf(LPCWSTR alg_id, const std::string& data, bool with_hmac,
                  const std::string& key) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &alg, alg_id, nullptr, with_hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0)))
        return {};

    std::vector<BYTE> digest;
    const bool ok = HashWith(alg, data, with_hmac, key, digest);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? ToHex(digest) : std::string{};
}

}  // namespace

std::string Md5Hex(const std::string& in) {
    return HexOf(BCRYPT_MD5_ALGORITHM, in, false, {});
}

std::string Sha1Hex(const std::string& in) {
    return HexOf(BCRYPT_SHA1_ALGORITHM, in, false, {});
}

std::string HmacMd5Hex(const std::string& data, const std::string& key) {
    return HexOf(BCRYPT_MD5_ALGORITHM, data, true, key);
}

}  // namespace cnk
