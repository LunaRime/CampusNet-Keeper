#pragma once

#include "core/Scheduler.h"
#include "ipc/Protocol.h"

namespace cnk {

// 处理设置界面发来的命令（守护进程侧）。命令集见计划任务 25：
//   get_status / reconnect_now / get_log_tail / get_config / set_config /
//   get_carrier_options / test_profile
//
// 刻意**不实现 `get_password`**：任务 24 的硬约束是「UI 进程从头到尾不接触已存密码的明文」，
// 而该命令会把明文交给界面；两处要求冲突时按更严的那条执行（界面上的「显示」只揭示
// 用户本回合刚输入的字符，不去取磁盘上的密码）。
IpcResponse HandleDaemonCommand(const IpcRequest& req, Scheduler& sch);

}  // namespace cnk
