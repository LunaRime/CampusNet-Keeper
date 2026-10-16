#pragma once
#include <string>

namespace cnk {

// BCrypt 实现的标准哈希（十六进制小写）。
// 深澜 srun 的登录需要 HMAC-MD5；Sha1Hex 供后续厂商扩展与校验使用。
std::string Md5Hex(const std::string& in);
std::string Sha1Hex(const std::string& in);
std::string HmacMd5Hex(const std::string& data, const std::string& key);

}  // namespace cnk
