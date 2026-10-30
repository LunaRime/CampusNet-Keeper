#pragma once
#include <memory>
#include "auth/IAuth.h"
#include "auth/Detector.h"

namespace cnk {

// 依 profile.vendor 决定策略：
//   - "custom"  → CustomTemplateAuth（无条件）
//   - "generic" → GenericFormAuth（强制通用表单，覆盖自动识别）
//   - "auto"    → 按 detected 选择
//   - 其他已知值 → 强制对应策略（huawei 走 GenericFormAuth）
// 永不返回 nullptr。
std::unique_ptr<IAuth> CreateAuth(Vendor detected, const Profile& profile);

// Dr.COM 新式接口登录应答的分类（纯函数，便于单测）。
// 单独拆出来的原因：这三条语义都踩过坑，必须能被断言钉住——
//   code==1                      → 成功
//   「终端…已在线」              → 成功，但打 terminal_online：还要复测，可能是僵尸会话
//   「…已在线」但不含「终端」    → in_use：会话被占，重试无效，必须先注销
//   其余                         → 失败；只有「凭据词 + 否定词」同时出现才算凭据错误
LoginResult ClassifyDrcomReply(int code, const std::string& msg,
                               const std::string& ret_code);

}  // namespace cnk
