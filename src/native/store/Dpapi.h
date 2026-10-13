#pragma once
#include <string>

namespace cnk {

// 用当前 Windows 用户的 DPAPI 密钥加密，固定 entropy 绑定本程序。
// 空输入返回 true 且 blob 为空。失败返回 false。
bool DpapiProtect(const std::string& plain, std::string& blob);
bool DpapiUnprotect(const std::string& blob, std::string& plain);

}  // namespace cnk
