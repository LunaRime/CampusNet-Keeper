#pragma once
#include <string>

namespace cnk {

// 返回 %APPDATA%\CampusNet-Keeper，必要时创建目录。
// 失败时返回空字符串。
std::string AppDataDir();

// 在 AppDataDir() 下创建子目录并返回路径。
std::string AppDataSubDir(const std::string& name);

// %LOCALAPPDATA%\CampusNet-Keeper 的**路径**（不创建目录）。
// 任务 26 起，几十 MB 的 UI 载荷放本机（Local），不放漫游（Roaming）配置目录——
// 漫游目录只该放配置这类小文件，放 DLL 会被域漫游同步拖慢甚至写不进去。
std::string LocalAppDataPath();

// 同上，但确保目录存在；失败返回空串。
std::string LocalAppDataDir();

// 在 LocalAppDataDir() 下创建子目录并返回路径。
std::string LocalAppDataSubDir(const std::string& name);

}  // namespace cnk
