#pragma once
#include <map>
#include <string>

namespace cnk {

// 本机密码库：档案名 -> base64(DPAPI 密文)
//
// 为什么单独放一个文件：config.json 在 %APPDATA%（Roaming），会随域漫游、换机搬家工具
// 或用户自己的备份被搬到别的机器；而 DPAPI 密文只对「同机 + 同账号」可解。
// 两者放在一起 = 搬一次就把另一台机器上的密码废掉。密文留在本机专属的
// %LOCALAPPDATA%\CampusNet-Keeper\secret.json，配置才可以安心漫游。
using SecretMap = std::map<std::string, std::string>;

// %LOCALAPPDATA%\CampusNet-Keeper\secret.json（只算路径，不创建目录）
std::string SecretPath();

// 文件不存在视为「空库」并返回 true（首次运行不是错误）。
// 解析失败返回 false 并填 error——调用方必须当作「读不出来」处理，
// 绝不能拿空库去覆盖写盘（那会删掉别人的密码）。
bool SecretLoadFrom(const std::string& path, SecretMap& out, std::string& error);
bool SecretSaveTo(const std::string& path, const SecretMap& secrets, std::string& error);

bool SecretLoad(SecretMap& out, std::string& error);
bool SecretSave(const SecretMap& secrets, std::string& error);

}  // namespace cnk
