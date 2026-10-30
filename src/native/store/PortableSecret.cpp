#include "store/PortableSecret.h"

#include "core/Crypto.h"
#include "json.hpp"

#include <ctime>

using json = nlohmann::json;

namespace cnk {

namespace {

constexpr int kIterations = 200000;   // 200k 次 PBKDF2-SHA256，2026 年桌面机约 100ms 量级
constexpr int kIterMin = 10000;       // 导入时下限：低于这个值说明文件可疑
constexpr int kIterMax = 10000000;    // 导入时上限：防止恶意文件用超大迭代数卡死解密
constexpr size_t kSaltLen = 16;
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;
constexpr size_t kKeyLen = 32;
constexpr int kFormatVersion = 1;

// 绑定格式的附加认证数据：改格式版本 / 改算法名都会导致解密失败，
// 不会出现「用旧解析器读新文件」这种静默错配
const char kAad[] = "CampusNetKeeper.portable.v1";

std::string StrOrEmpty(const json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    return {};
}

int IntOr(const json& j, const char* key, int fallback) {
    if (!j.contains(key)) return fallback;
    const auto& v = j[key];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_unsigned()) return (int)v.get<unsigned long long>();
    return fallback;
}

}  // namespace

bool ExportPortableSecret(const PortableSecret& secret, const std::string& passphrase,
                          std::string& out_json, std::string& error) {
    out_json.clear();
    error.clear();
    if (passphrase.empty()) {
        error = "口令不能为空";
        return false;
    }
    if (secret.password.empty()) {
        error = "没有可导出的密码";
        return false;
    }

    const std::string salt = RandomBytes(kSaltLen);
    const std::string nonce = RandomBytes(kNonceLen);
    if (salt.size() != kSaltLen || nonce.size() != kNonceLen) {
        error = "随机数生成失败";
        return false;
    }

    std::string key;
    if (!Pbkdf2Sha256(passphrase, salt, kIterations, kKeyLen, key)) {
        error = "密钥派生失败";
        return false;
    }

    std::string cipher, tag;
    if (!AesGcmEncrypt(key, nonce, kAad, secret.password, cipher, tag) ||
        tag.size() != kTagLen) {
        error = "加密失败";
        return false;
    }

    json j;
    j["format"] = "cnk-secret";
    j["version"] = kFormatVersion;
    j["profile"] = secret.profile;
    j["username"] = secret.username;
    j["created"] = (long long)std::time(nullptr);
    j["kdf"] = {{"algo", "pbkdf2-sha256"}, {"iter", kIterations},
                {"salt", Base64Encode(salt)}};
    j["cipher"] = {{"algo", "aes-256-gcm"},
                   {"nonce", Base64Encode(nonce)},
                   {"tag", Base64Encode(tag)}};
    j["data"] = Base64Encode(cipher);

    try {
        out_json = j.dump(2);
    } catch (const std::exception& e) {
        error = std::string("生成导出文件失败: ") + e.what();
        return false;
    }
    return true;
}

bool ImportPortableSecret(const std::string& json_text, const std::string& passphrase,
                          PortableSecret& out, std::string& error) {
    out = PortableSecret{};
    error.clear();
    if (json_text.empty()) {
        error = "导出文件为空";
        return false;
    }
    if (passphrase.empty()) {
        error = "口令不能为空";
        return false;
    }

    json j;
    try {
        j = json::parse(json_text);
    } catch (const std::exception& e) {
        error = std::string("不是合法的导出文件（JSON 解析失败）: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "不是合法的导出文件（根节点不是对象）";
        return false;
    }
    if (StrOrEmpty(j, "format") != "cnk-secret") {
        error = "不是本程序导出的密码文件（format 不匹配）";
        return false;
    }
    if (IntOr(j, "version", 0) != kFormatVersion) {
        error = "导出文件版本不支持（请升级程序后再导入）";
        return false;
    }

    if (!j.contains("kdf") || !j["kdf"].is_object() ||
        !j.contains("cipher") || !j["cipher"].is_object()) {
        error = "导出文件缺少 kdf/cipher 段";
        return false;
    }
    const auto& kdf = j["kdf"];
    const auto& cipher = j["cipher"];
    if (StrOrEmpty(kdf, "algo") != "pbkdf2-sha256") {
        error = "不支持的密钥派生算法";
        return false;
    }
    if (StrOrEmpty(cipher, "algo") != "aes-256-gcm") {
        error = "不支持的加密算法";
        return false;
    }
    const int iters = IntOr(kdf, "iter", 0);
    if (iters < kIterMin || iters > kIterMax) {
        error = "导出文件的迭代次数不合理，已拒绝";
        return false;
    }

    std::string salt, nonce, tag, data;
    if (!Base64Decode(StrOrEmpty(kdf, "salt"), salt) || salt.size() != kSaltLen) {
        error = "导出文件的 salt 无效";
        return false;
    }
    if (!Base64Decode(StrOrEmpty(cipher, "nonce"), nonce) || nonce.size() != kNonceLen) {
        error = "导出文件的 nonce 无效";
        return false;
    }
    if (!Base64Decode(StrOrEmpty(cipher, "tag"), tag) || tag.size() != kTagLen) {
        error = "导出文件的校验值无效";
        return false;
    }
    if (!Base64Decode(StrOrEmpty(j, "data"), data) || data.empty()) {
        error = "导出文件没有密文数据";
        return false;
    }

    std::string key, plain;
    if (!Pbkdf2Sha256(passphrase, salt, iters, kKeyLen, key)) {
        error = "密钥派生失败";
        return false;
    }
    if (!AesGcmDecrypt(key, nonce, kAad, data, tag, plain)) {
        // GCM 认证失败只有两种可能：口令错，或文件被改动过
        error = "口令错误，或导出文件已被修改";
        return false;
    }

    out.profile = StrOrEmpty(j, "profile");
    out.username = StrOrEmpty(j, "username");
    out.password = std::move(plain);
    return true;
}

std::string PortableSecretSuggestedName() {
    char buf[32] = {0};
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    std::strftime(buf, sizeof(buf), "campusnet-secret-%Y%m%d.json", &tm);
    return buf;
}

}  // namespace cnk
