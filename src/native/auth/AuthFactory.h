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

}  // namespace cnk
