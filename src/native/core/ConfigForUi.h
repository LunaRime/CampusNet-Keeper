#pragma once
#include <string>

#include "store/Config.h"

namespace cnk {

// 给设置界面用的配置序列化：**剥掉 password_enc**，只留 has_password。
//
// 安全红线：password_enc 是 DPAPI 密文，解它只需要「当前用户」这一条件——而 UI 进程
// 正是当前用户，拿到密文就等于拿到密码。界面只需要知道「这份档案有没有密码」。
std::string ConfigForUiJson(const AppConfig& cfg);

// 把设置界面送来的配置合并成可落盘的配置：密码按「新值优先、否则沿用旧值」处理。
//   · 档案带非空 password_plain → 用新密码（ConfigSave 会 DPAPI 加密）
//   · 否则按**档案名**匹配旧配置，沿用旧密码；匹配不到就留空
// 非法输入返回 false 并填 error（不抛异常）。
bool MergeUiConfig(const std::string& ui_json, const AppConfig& existing, AppConfig& out,
                   std::string& error);

}  // namespace cnk
