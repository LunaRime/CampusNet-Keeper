#pragma once
#include <string>

namespace cnk {

// 进程权限上下文：「提升/普通」+「完整性=低/中/高」。
// 低完整性进程对常规用户目录（%APPDATA%、%TEMP% 等）的写类访问会被内核的
// No-Write-Up 规则强制拒绝（与 DACL 无关）——排查「写入被拒（Win32=5）」
// 的关键维度。
std::string ProcessTokenContext();

// 读对象自身完整性标签的 RID（标签即 S-1-16-<rid>）：
//   >= 0  标签存在（0x1000=低 0x2000=中 0x3000=高）
//   -1    无标签 ACE（常规状态，等效中完整性）
//   -2    读取失败（对象不存在或无权读取）
//
// 为什么要读"可执行文件自身"的标签：新进程的完整性取
// min(父进程令牌, 可执行文件标签)。exe 被标成低完整性时，从任何终端启动
// 都是低完整性进程——与终端无关，换终端无效。
long long FileIntegrityRid(const std::string& utf8_path);

// RID → 中文名（提示文案用）
std::string IntegrityRidName(long long rid);

}  // namespace cnk
