#pragma once
#include <string>

namespace cnk {

// 当前可执行文件的完整路径（UTF-8）。失败返回空串。
std::string ExecutablePath();

// 写/删 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 项。
// 启用时写入 `"<当前 exe>" --autostart`（--autostart 表示静默启动、不弹设置界面）。
bool SetAutostart(bool enable, std::string& error);

// 让注册表与期望状态一致（配置里的 settings.autostart 是唯一真源）：
//   desired=true  → 写入 `"<当前 exe>" --autostart`（已指向本程序则不动）
//   desired=false → 删除该值（无论之前指向谁，避免残留死条目）
// 返回 false 时 error 说明原因。**只在该变时**写注册表。
bool SyncAutostart(bool desired, std::string& error);

// 注册表里的自启项是否指向**当前这个 exe**（带引号路径前缀匹配；被移动/改名后返回 false）。
bool IsAutostartEnabled();

}  // namespace cnk
