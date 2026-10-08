#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class LogLevel { Debug, Info, Warn, Error };

// keep_files 是「轮转备份数」，不含当前文件（标准 logrotate 语义）：
// keep_files=3 → keeper.log + keeper.log.1/.2/.3 共 4 个文件。
void LogInit(const std::string& log_dir, LogLevel level,
             size_t max_bytes, int keep_files);
void LogShutdown();

void LogWrite(LogLevel level, const std::string& msg);

inline void LogDebug(const std::string& m) { LogWrite(LogLevel::Debug, m); }
inline void LogInfo(const std::string& m)  { LogWrite(LogLevel::Info, m); }
inline void LogWarn(const std::string& m)  { LogWrite(LogLevel::Warn, m); }
inline void LogError(const std::string& m) { LogWrite(LogLevel::Error, m); }

// 返回最近 max_lines 行日志（从旧到新）。
std::vector<std::string> LogTail(size_t max_lines);

// 把 URL 或表单体中的密码类参数值替换为 ***。
// 覆盖字段：upass, password, pwd, pass, user_password, passwd
std::string RedactSecrets(const std::string& s);

}  // namespace cnk
