#include "core/Crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <vector>

namespace cnk {

namespace {

// 单个 BCrypt 算法句柄的 RAII 包装。
struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgHandle() {
        if (h) BCryptCloseAlgorithmProvider(h, 0);
    }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;
    AlgHandle() = default;
};

struct KeyHandle {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~KeyHandle() {
        if (h) BCryptDestroyKey(h);
    }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;
    KeyHandle() = default;
};

constexpr size_t kBlock = 16;

// PKCS7 填充：整块对齐时也补一个完整块（与 CryptoJS 行为一致）。
void Pkcs7Pad(std::string& data) {
    const size_t pad = kBlock - (data.size() % kBlock);
    data.append(pad, (char)pad);
}

// 去填充。填充非法返回 false（密钥不对时通常在这里暴露）。
bool Pkcs7Unpad(std::string& data) {
    if (data.empty() || data.size() % kBlock != 0) return false;
    const unsigned char pad = (unsigned char)data.back();
    if (pad == 0 || pad > kBlock || pad > data.size()) return false;
    for (size_t i = 0; i < pad; ++i) {
        if ((unsigned char)data[data.size() - 1 - i] != pad) return false;
    }
    data.resize(data.size() - pad);
    return true;
}

// AES-128-ECB 单块加/解密（无填充），输入长度必须是 16 的倍数。
bool AesEcbRaw(bool encrypt, const std::string& key, const std::string& in,
               std::string& out) {
    out.clear();
    if (key.size() != 16 || in.size() % kBlock != 0) return false;

    AlgHandle alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    if (BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0)
        return false;

    DWORD obj_len = 0, cb = 0;
    if (BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len,
                          sizeof(obj_len), &cb, 0) != 0)
        return false;

    std::vector<unsigned char> key_obj(obj_len);
    KeyHandle key_h;
    if (BCryptGenerateSymmetricKey(alg.h, &key_h.h, key_obj.data(), obj_len,
                                   (PUCHAR)key.data(), (DWORD)key.size(), 0) != 0)
        return false;

    // 无 BCRYPT_BLOCK_PADDING：输入已是整块（PKCS7 由调用方处理），输出等长。
    DWORD written = 0;
    std::string buf(in.size(), '\0');
    const NTSTATUS st =
        encrypt ? BCryptEncrypt(key_h.h, (PUCHAR)in.data(), (DWORD)in.size(),
                                nullptr, nullptr, 0, (PUCHAR)buf.data(),
                                (DWORD)buf.size(), &written, 0)
                : BCryptDecrypt(key_h.h, (PUCHAR)in.data(), (DWORD)in.size(),
                                nullptr, nullptr, 0, (PUCHAR)buf.data(),
                                (DWORD)buf.size(), &written, 0);
    if (st != 0 || written != in.size()) return false;
    out.assign(buf.data(), written);
    return true;
}

}  // namespace

std::string Base64Encode(const std::string& bytes) {
    if (bytes.empty()) return {};
    DWORD chars = 0;
    if (!CryptBinaryToStringA((const BYTE*)bytes.data(), (DWORD)bytes.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                              &chars))
        return {};
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA((const BYTE*)bytes.data(), (DWORD)bytes.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(),
                              &chars))
        return {};
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

bool Base64Decode(const std::string& b64, std::string& out) {
    out.clear();
    if (b64.empty()) return true;
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64,
                              nullptr, &bytes, nullptr, nullptr))
        return false;
    out.resize(bytes);
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64,
                              (BYTE*)out.data(), &bytes, nullptr, nullptr)) {
        out.clear();
        return false;
    }
    out.resize(bytes);
    return true;
}

bool AesEcbPkcs7EncryptB64(const std::string& key, const std::string& plain,
                           std::string& out_b64) {
    out_b64.clear();
    std::string data = plain;
    Pkcs7Pad(data);
    std::string cipher;
    if (!AesEcbRaw(true, key, data, cipher)) return false;
    out_b64 = Base64Encode(cipher);
    return !out_b64.empty();
}

bool AesEcbPkcs7DecryptB64(const std::string& key, const std::string& b64,
                           std::string& out_plain) {
    out_plain.clear();
    std::string cipher;
    if (!Base64Decode(b64, cipher)) return false;
    std::string data;
    if (!AesEcbRaw(false, key, cipher, data)) return false;
    if (!Pkcs7Unpad(data)) return false;
    out_plain = std::move(data);
    return true;
}

}  // namespace cnk
