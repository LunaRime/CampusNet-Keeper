#pragma once
#include <string>

namespace cnk {

// 可携带的密码导出文件（任务 27）。
//
// 为什么不是「放宽 DPAPI 让配置能跨机」：DPAPI 的机器+用户绑定正是它的价值所在。
// 跨机迁移应当是一次**显式、可审计、需要口令**的动作：
//   口令 --PBKDF2-HMAC-SHA256(salt, 200k)--> 32 字节密钥 --AES-256-GCM--> 文件；
// 文件被拷走也解不开，除非对方知道口令；口令不在文件里，也不进日志。
struct PortableSecret {
    std::string profile;   // 档案名（导入时用于提示，不参与匹配决策）
    std::string username;  // 账号（导入时提示确认，避免导错档案）
    std::string password;  // 明文，仅在内存中短暂存在
};

// 导出为 JSON 文本（调用方负责写文件；不在这里碰磁盘，便于测试与 IPC 复用）。
// 口令为空直接失败——文件的安全性全压在这句口令上。
bool ExportPortableSecret(const PortableSecret& secret, const std::string& passphrase,
                          std::string& out_json, std::string& error);

// 解析并解密。口令错 / 文件被改 / 格式不支持 都返回 false 并填中文 error。
bool ImportPortableSecret(const std::string& json_text, const std::string& passphrase,
                          PortableSecret& out, std::string& error);

// 建议的文件名（不含目录），带日期，便于用户区分多个导出文件
std::string PortableSecretSuggestedName();

}  // namespace cnk
