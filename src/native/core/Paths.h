#pragma once
#include <string>

namespace cnk {

// 返回 %APPDATA%\CampusNet-Keeper，必要时创建目录。
// 失败时返回空字符串。
std::string AppDataDir();

// 在 AppDataDir() 下创建子目录并返回路径。
std::string AppDataSubDir(const std::string& name);

}  // namespace cnk
