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

// 用认证服务器（AC）的视角复核会话（对应 router 端 ac_says_online）：
// 探测页 200+"Success" 不证明会话还在——本网 DNS 被劫持时探测域名照样回 Success，
// 而 AC 那边早已没有这条会话。只有问 AC 自己才分得清。
//   1 = AC 认为本机在线
//   0 = AC 明确说不在线（会话失效）
//  -1 = 查不出来（门户不是 http / 接口不存在 / 超时 / 回包不是 JSONP / 没有 result）
// 返回 -1 时调用方**不得**据此判离线：非 Dr.COM 部署与临时故障都会落到这里。
int DrcomAcSessionState(const Profile& p);

}  // namespace cnk
