#include "store/Config.h"
#include "store/Dpapi.h"
#include "core/Paths.h"
#include "core/Log.h"
#include "json.hpp"
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace cnk {

namespace {

// base64 累积器用 unsigned：加密后的 blob 有数百字节，编码要累积 3 字节
// 一组、解码累积 4 字符一组，若用 int，中间态会越过 2^31 触发有符号左移
// 溢出（UB）。无符号回绕是良定义的，且低位语义不受影响。
std::string Base64Encode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned int val = 0;
    int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += tbl[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += tbl[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::string Base64Decode(const std::string& in) {
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned int val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        size_t pos = tbl.find((char)c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + (unsigned int)pos;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

bool ProfileToJson(const Profile& p, json& out) {
    json j;
    j["name"] = p.name;
    j["auth_url"] = p.auth_url;
    j["username"] = p.username;
    j["vendor"] = p.vendor;
    j["isp"] = {{"preset", p.isp.preset}, {"custom", p.isp.custom}};

    if (!p.password.empty()) {
        std::string blob;
        // 加密失败必须显式失败：静默跳过会让「保存成功」变成密码丢失
        if (!DpapiProtect(p.password, blob)) return false;
        j["password_enc"] = Base64Encode(blob);
    } else {
        j["password_enc"] = "";
    }

    j["custom_template"] = {
        {"method", p.custom_template.method},
        {"url", p.custom_template.url},
        {"body", p.custom_template.body},
        {"headers", p.custom_template.headers},
        {"success_regex", p.custom_template.success_regex},
    };
    out = std::move(j);
    return true;
}

Profile ProfileFromJson(const json& j) {
    Profile p;
    p.name = j.value("name", std::string{});
    p.auth_url = j.value("auth_url", std::string{});
    p.username = j.value("username", std::string{});
    p.vendor = j.value("vendor", std::string{"auto"});

    if (j.contains("isp") && j["isp"].is_object()) {
        p.isp.preset = j["isp"].value("preset", std::string{"campus"});
        p.isp.custom = j["isp"].value("custom", std::string{});
    }

    std::string enc = j.value("password_enc", std::string{});
    if (!enc.empty()) {
        std::string blob = Base64Decode(enc);
        if (DpapiUnprotect(blob, p.password)) {
            p.has_password = true;
        } else {
            // 已知行为：解密失败后 password 为空，后续任何 ConfigSave 都会把
            // password_enc 清空。该密文在本机本不可解（换机/换用户才会发生），
            // spec §8 要求此时提示用户重新输入，故此处不保留原密文。
            LogWarn("配置中的密码无法解密（可能来自其他机器或用户），需重新输入");
            p.has_password = false;
        }
    }

    if (j.contains("custom_template") && j["custom_template"].is_object()) {
        const auto& ct = j["custom_template"];
        p.custom_template.method = ct.value("method", std::string{"POST"});
        p.custom_template.url = ct.value("url", std::string{});
        p.custom_template.body = ct.value("body", std::string{});
        p.custom_template.success_regex = ct.value("success_regex", std::string{});
        if (ct.contains("headers") && ct["headers"].is_object())
            for (auto& [k, v] : ct["headers"].items())
                p.custom_template.headers[k] = v.get<std::string>();
    }
    return p;
}

}  // namespace

std::string ConfigPath() {
    std::string dir = AppDataDir();
    if (dir.empty()) return {};
    return (fs::u8path(dir) / "config.json").u8string();
}

bool ConfigToJson(const AppConfig& cfg, std::string& out_json) {
    try {
        json j;
        j["version"] = cfg.version;
        j["active_profile"] = cfg.active_profile;

        j["settings"] = {
            {"probe_interval_seconds", cfg.settings.probe_interval_seconds},
            {"probe_timeout_seconds", cfg.settings.probe_timeout_seconds},
            {"probe_url", cfg.settings.probe_url},
            {"probe_expect", cfg.settings.probe_expect},
            {"retry_backoff_seconds", cfg.settings.retry_backoff_seconds},
            {"autostart", cfg.settings.autostart},
            {"notify_on_reconnect", cfg.settings.notify_on_reconnect},
            {"log_level", cfg.settings.log_level},
            {"log_max_kb", cfg.settings.log_max_kb},
            {"log_keep_files", cfg.settings.log_keep_files},
        };

        j["profiles"] = json::array();
        for (const auto& p : cfg.profiles) {
            json pj;
            if (!ProfileToJson(p, pj)) return false;  // 密码加密失败
            j["profiles"].push_back(std::move(pj));
        }

        out_json = j.dump(2);
        return true;
    } catch (const std::exception&) {
        // 与 ConfigFromJson 对称：序列化任何异常（如字段含非法 UTF-8 时
        // dump 抛 type_error.316）都转为显式失败，不向上抛
        return false;
    }
}

bool ConfigFromJson(const std::string& json_str, AppConfig& out, std::string& error) {
    out = AppConfig{};
    error.clear();
    json j;
    try {
        j = json::parse(json_str);
    } catch (const std::exception& e) {
        error = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "配置根节点不是对象";
        return false;
    }

    // 字段解析单独保护：nlohmann 的 value() 在类型不匹配时抛 type_error。
    // 用户手改配置写错类型（如 "version":"x"）必须得到普通失败，
    // 由上层走「备份 + 回落默认」路径，绝不允许异常穿出或崩溃。
    try {
        out.version = j.value("version", 1);
        out.active_profile = j.value("active_profile", std::string{"默认"});

        if (j.contains("settings") && j["settings"].is_object()) {
            const auto& s = j["settings"];
            auto& t = out.settings;
            t.probe_interval_seconds = s.value("probe_interval_seconds", 5);
            t.probe_timeout_seconds = s.value("probe_timeout_seconds", 3);
            t.probe_url = s.value("probe_url", t.probe_url);
            t.probe_expect = s.value("probe_expect", t.probe_expect);
            t.autostart = s.value("autostart", true);
            t.notify_on_reconnect = s.value("notify_on_reconnect", true);
            t.log_level = s.value("log_level", std::string{"info"});
            t.log_max_kb = s.value("log_max_kb", 512);
            t.log_keep_files = s.value("log_keep_files", 3);
            if (s.contains("retry_backoff_seconds") &&
                s["retry_backoff_seconds"].is_array()) {
                t.retry_backoff_seconds.clear();
                for (auto& v : s["retry_backoff_seconds"])
                    t.retry_backoff_seconds.push_back(v.get<int>());
                if (t.retry_backoff_seconds.empty())
                    t.retry_backoff_seconds = {5, 10, 20, 30, 60};
            }
        }

        if (j.contains("profiles") && j["profiles"].is_array())
            for (auto& pj : j["profiles"]) out.profiles.push_back(ProfileFromJson(pj));
    } catch (const std::exception& e) {
        out = AppConfig{};
        error = std::string("配置字段类型错误: ") + e.what();
        return false;
    }

    return true;
}

bool ConfigSaveTo(const std::string& path, const AppConfig& cfg, std::string& error) {
    error.clear();
    if (path.empty()) { error = "配置路径为空"; return false; }

    std::string json_str;
    if (!ConfigToJson(cfg, json_str)) { error = "序列化失败"; return false; }

    // 先写临时文件再改名，避免写入中断留下半截文件
    fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) { error = "无法写入临时配置文件"; return false; }
        ofs << json_str;
        ofs.flush();
        if (!ofs) { error = "写入临时配置文件失败"; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, fs::u8path(path), ec);  // MSVC 的 rename 可覆盖已存在目标（依赖平台行为）
    if (ec) {
        // rename 失败（权限/占用等）时保留旧配置原样，只清理 .tmp 并报错。
        // 绝不能「先删旧配置再重试」：第二次 rename 若因同样原因失败，
        // 用户配置将彻底丢失（杀毒软件瞬时锁定 .tmp 即可触发）。
        std::error_code e2;
        fs::remove(tmp, e2);
        error = "替换配置文件失败: " + ec.message();
        return false;
    }
    return true;
}

bool ConfigSave(const AppConfig& cfg, std::string& error) {
    const std::string path = ConfigPath();
    if (path.empty()) { error = "无法确定配置目录"; return false; }
    return ConfigSaveTo(path, cfg, error);
}

bool ConfigLoadFrom(const std::string& path, AppConfig& out, std::string& error) {
    error.clear();
    out = AppConfig{};
    if (path.empty()) { error = "配置路径为空"; return false; }

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        error = "配置文件不存在";
        return false;
    }

    std::ifstream ifs(fs::u8path(path), std::ios::binary);
    if (!ifs) { error = "无法读取配置文件"; return false; }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    std::string parse_err;
    if (!ConfigFromJson(content, out, parse_err)) {
        // 损坏：备份后回落默认配置，绝不让上层拿到半个配置
        fs::path bak = fs::u8path(path + ".bak");
        std::error_code e2;
        fs::copy_file(fs::u8path(path), bak, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            LogError("配置文件损坏，且备份失败（" + e2.message() + "），已回落默认配置：" + parse_err);
        } else {
            LogError("配置文件损坏，已备份到 " + path + ".bak：" + parse_err);
        }
        out = AppConfig{};
        error = parse_err;
        return false;
    }
    return true;
}

bool ConfigLoad(AppConfig& out, std::string& error) {
    const std::string path = ConfigPath();
    if (path.empty()) {
        out = AppConfig{};  // 与 ConfigLoadFrom 语义一致：失败时不向调用方留下旧值
        error = "无法确定配置目录";
        return false;
    }
    return ConfigLoadFrom(path, out, error);
}

std::string IspSuffix(const Profile& p) {
    if (p.isp.preset == "custom") return p.isp.custom;
    const bool drcom = (p.vendor == "drcom") || (p.vendor == "auto");
    if (p.isp.preset == "cmcc")    return drcom ? "@cmcc" : (p.vendor == "srun" ? "@cmcc" : "cmcc");
    if (p.isp.preset == "telecom") return drcom ? "@dx"   : (p.vendor == "srun" ? "@telecom" : "telecom");
    if (p.isp.preset == "unicom")  return drcom ? "@lt"   : (p.vendor == "srun" ? "@unicom" : "unicom");
    if (p.isp.preset == "campus")  return drcom ? ""      : (p.vendor == "srun" ? "@campus" : "");
    return {};
}

const Profile* ActiveProfile(const AppConfig& cfg) {
    if (cfg.profiles.empty()) return nullptr;
    for (const auto& p : cfg.profiles)
        if (p.name == cfg.active_profile) return &p;
    return &cfg.profiles.front();
}

}  // namespace cnk
