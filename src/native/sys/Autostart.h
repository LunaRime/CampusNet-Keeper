#pragma once
#include <string>

namespace cnk {

// 当前可执行文件的完整路径（UTF-8）。失败返回空串。
std::string ExecutablePath();

// 写/删 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 项。
bool SetAutostart(bool enable, std::string& error);
bool IsAutostartEnabled();

}  // namespace cnk
