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
    bool has_password = false;  // 反序列化后表示 password_enc 非空且解密成功
    std::string vendor = "auto";  // auto|drcom|srun|eportal|generic|custom
    IspConfig isp;
    CustomTemplate custom_template;
};

struct Settings {
    int probe_interval_seconds = 5;
    int probe_timeout_seconds = 3;
    std::string probe_url = "http://www.msftncsi.com/ncsi.txt";
    std::string probe_expect = "Microsoft NCSI";
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

// 序列化到 JSON 字符串；密码以 base64(DPAPI) 存入 password_enc 字段
bool ConfigToJson(const AppConfig& cfg, std::string& out_json);
// 从 JSON 反序列化；解密 password_enc 到 password。
// 任何格式/类型错误都返回 false 并填 error，不抛异常、不崩溃。
bool ConfigFromJson(const std::string& json, AppConfig& out, std::string& error);

// 可注入路径的版本（测试与 CLI 用）。path 必须为 UTF-8 编码（与 ConfigPath()
// 输出一致；非 UTF-8 路径会在 u8path 转换处抛 std::system_error）。
// 失败时不抛异常。写入采用「先写 .tmp 再改名」的原子替换；替换失败时保留
// 旧文件并清理 .tmp。读取遇损坏文件时备份为 <path>.bak 后返回失败并回落默认配置。
bool ConfigLoadFrom(const std::string& path, AppConfig& out, std::string& error);
bool ConfigSaveTo(const std::string& path, const AppConfig& cfg, std::string& error);

// 上述重载的薄封装，路径为 ConfigPath()（%APPDATA%\CampusNet-Keeper\config.json）
bool ConfigLoad(AppConfig& out, std::string& error);
bool ConfigSave(const AppConfig& cfg, std::string& error);

// 该 Profile 在 Dr.COM 下应拼到账号后的后缀（其他厂商由各自 Auth 使用）
std::string IspSuffix(const Profile& p);

// active_profile 命中的项；不存在时返回第一个；无 profile 时返回 nullptr
const Profile* ActiveProfile(const AppConfig& cfg);

}  // namespace cnk
