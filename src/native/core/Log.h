#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class LogLevel { Debug, Info, Warn, Error };

// 按天分文件：logs/keeper-YYYY-MM-DD.log，跨天自动切换（写下一行时检查日期）。
// keep_days 是「按天保留」的天数（默认 3）：启动时与每次跨天切换时，删除日期早于
// today-(keep_days-1) 的 keeper-YYYY-MM-DD.log[.N] 文件；解析不出日期的文件一律不动。
// max_bytes 仍是**日内**大小保护：当天文件过大才轮转为 .1/.2/.3，不再靠轮转数决定保留期。
void LogInit(const std::string& log_dir, LogLevel level,
             size_t max_bytes, int keep_days);
void LogShutdown();

void LogWrite(LogLevel level, const std::string& msg);

inline void LogDebug(const std::string& m) { LogWrite(LogLevel::Debug, m); }
inline void LogInfo(const std::string& m)  { LogWrite(LogLevel::Info, m); }
inline void LogWarn(const std::string& m)  { LogWrite(LogLevel::Warn, m); }
inline void LogError(const std::string& m) { LogWrite(LogLevel::Error, m); }

// 返回最近 max_lines 行日志（从旧到新，内存尾部，跨天连续）。
std::vector<std::string> LogTail(size_t max_lines);

// 把 URL 或表单体中的密码类参数值替换为 ***。
std::string RedactSecrets(const std::string& s);

// 以下三个供 AuditLastExit 与单测使用（不依赖全局状态、可独立复算）：
std::string LogFileNameForDate(const std::string& ymd);
int CleanupOldLogs(const std::string& log_dir, const std::string& today_ymd, int keep_days);
bool HasAnyLogFile(const std::string& log_dir);

}  // namespace cnk
