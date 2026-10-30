#pragma once
#include <string>
#include <vector>
#include <map>

namespace cnk {

struct IspConfig {
    std::string preset = "campus";  // cmcc | telecom | unicom | campus | custom
    std::string custom;
};

struct CustomTemplate {
    std::string method = "POST";
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string success_regex;
};

struct Profile {
    std::string name;
    std::string auth_url;
    std::string username;
    std::string password;       // 内存中明文
    bool has_password = false;  // 本机可解、已加载明文（== 有可用密码）
    // 密文存在但本机解不开（换机器 / 换 Windows 账号）。此时 password 为空，
    // 但 password_enc_raw 会**原样保留**，任何保存都要把它写回去——
    // 否则「在另一台机器上点一次保存」就把原机器还能用的密码抹掉了。
    bool password_foreign = false;
    std::string password_enc_raw;  // 外来密文（base64），只在本机解不开时非空
    std::string vendor = "auto";  // auto|drcom|srun|eportal|generic|custom
    IspConfig isp;
    CustomTemplate custom_template;
};

struct Settings {
    int probe_interval_seconds = 5;
    int probe_timeout_seconds = 6;
    // 探测地址（2026-10-16 真机实测修订）：原默认 www.msftncsi.com/ncsi.txt 在本校园网
    // 直连实测 **0/4 可达**（每次都卡满超时），会把「在线」误判成「离线」——守护程序
    // 永远认为不在线，--test-login 的复测也报「仍不在线」。改用免劫持清单内、实测
    // 4/4 且平均约 50ms 的 Apple 探测页（iOS/macOS 的原生 captive 探测地址，
    // 各校园网普遍放行；正文 134 字节，含 "Success"）。
    std::string probe_url = "http://captive.apple.com/hotspot-detect.html";
    std::string probe_expect = "Success";
    std::vector<int> retry_backoff_seconds = {5, 10, 20, 30, 60};
    bool autostart = true;
    bool notify_on_reconnect = true;
    std::string log_level = "info";
    int log_max_kb = 512;
    int log_keep_files = 3;
};

struct AppConfig {
    int version = 1;
    std::string active_profile = "默认";
    Settings settings;
    std::vector<Profile> profiles;
};

std::string ConfigPath();

// 序列化到 JSON 字符串；密码以 base64(DPAPI) 存入 password_enc 字段（自包含形态）。
bool ConfigToJson(const AppConfig& cfg, std::string& out_json);
// 从 JSON 反序列化；解密 password_enc 到 password。
// 任何格式/类型错误都返回 false 并填 error，不抛异常、不崩溃。
bool ConfigFromJson(const std::string& json, AppConfig& out, std::string& error);

// 可注入路径的版本（测试与 CLI 用）。path 必须为 UTF-8 编码（与 ConfigPath()
// 输出一致；非 UTF-8 路径会在 u8path 转换处抛 std::system_error）。
// 失败时不抛异常。写入采用「先写 .tmp 再改名」的原子替换；替换失败时保留
// 旧文件并清理 .tmp。读取遇损坏文件时备份为 <path>.bak 后返回失败并回落默认配置。
//
// 这两个是**自包含**形态：密文内联在 config.json 里（`--config` 与测试用）。
bool ConfigLoadFrom(const std::string& path, AppConfig& out, std::string& error);
bool ConfigSaveTo(const std::string& path, const AppConfig& cfg, std::string& error);

// **拆分**形态（默认安装用它）：密文写 secret_path（%LOCALAPPDATA%，本机专属），
// config.json 只保留外来密文（password_enc_raw）。这样配置可以随 Roaming 漫游，
// 而密码不会被"搬"到别的机器上失效。
bool ConfigLoadSplitFrom(const std::string& config_path, const std::string& secret_path,
                         AppConfig& out, std::string& error);
bool ConfigSaveSplitTo(const std::string& config_path, const std::string& secret_path,
                       const AppConfig& cfg, std::string& error);

// 上述重载的薄封装：配置 %APPDATA%\CampusNet-Keeper\config.json + 密码库
// %LOCALAPPDATA%\CampusNet-Keeper\secret.json
bool ConfigLoad(AppConfig& out, std::string& error);
bool ConfigSave(const AppConfig& cfg, std::string& error);

// 该 Profile 在 Dr.COM 下应拼到账号后的后缀（其他厂商由各自 Auth 使用）
std::string IspSuffix(const Profile& p);

// active_profile 命中的项；不存在时返回第一个；无 profile 时返回 nullptr
const Profile* ActiveProfile(const AppConfig& cfg);

// 为指定账号设置密码（供 --set-password 使用；调用方负责随后 ConfigSave）。
//   user 非空：命中同名档案则更新；无同名档案则新建一份（vendor=auto）
//   user 为空：更新 active_profile 对应的档案（或第一份档案）
// 空密码返回 false。密码在保存时由 DPAPI 加密写入 password_enc。
bool SetProfilePassword(AppConfig& cfg, const std::string& user,
                        const std::string& password, std::string& error);

}  // namespace cnk
