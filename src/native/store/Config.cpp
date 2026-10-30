#include "store/Config.h"
#include "core/Crypto.h"
#include "store/Dpapi.h"
#include "store/Secret.h"
#include "core/Paths.h"
#include "core/Log.h"
#include "core/Integrity.h"
#include "json.hpp"
#include <windows.h>
#include <cerrno>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace cnk {

namespace {

// 把档案写成 JSON。密文不在这里写死：enc_b64 返回本次加密出的密文，
// 由调用方决定放进 config.json（自包含）还是 secret.json（拆分）。
bool ProfileToJson(const Profile& p, json& out, std::string& enc_b64) {
    enc_b64.clear();
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
        enc_b64 = Base64Encode(blob);
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

// 解析（不解密）：password_enc 原样收进 password_enc_raw，解密交给两步解析。
// 不在这里解密的原因：同一份 JSON 有两种来源——自包含形态（密文就在 config.json，
// 只有它能解）与拆分形态（密文在本机 secret.json，那份优先）。
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

    p.password_enc_raw = j.value("password_enc", std::string{});

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

// 用一段 base64 密文解出明文。成功则档案进入「有可用密码」状态。
// 失败**不删除任何东西**：调用方保留原密文，等用户决定怎么办。
bool TryUnprotect(const std::string& enc_b64, Profile& p) {
    if (enc_b64.empty()) return false;
    std::string blob;
    if (!Base64Decode(enc_b64, blob) || blob.empty()) return false;
    std::string plain;
    if (!DpapiUnprotect(blob, plain)) return false;
    p.password = plain;
    p.has_password = true;
    p.password_foreign = false;
    p.password_enc_raw.clear();
    return true;
}

// 自包含形态：密文就在 config.json 里，逐条试解。
void ResolveInlinePasswords(AppConfig& cfg) {
    for (auto& p : cfg.profiles) {
        if (p.password_enc_raw.empty()) continue;
        if (TryUnprotect(p.password_enc_raw, p)) continue;
        p.has_password = false;
        p.password_foreign = true;
        // 已知行为：解不开的密文**原样保留**（password_enc_raw），保存时照写回去。
        // 旧实现会把它清空，于是「在另一台机器上点一次保存」就把原机器能用的密码
        // 永久抹掉了（且没有备份可回退）。
        LogWarn("配置中的密码无法解密（来自其他机器或 Windows 账号）：已保留原密文，请在本机重新输入");
    }
}

}  // namespace

std::string ConfigPath() {
    std::string dir = AppDataDir();
    if (dir.empty()) return {};
    return (fs::u8path(dir) / "config.json").u8string();
}

namespace {

// secrets 非空 = 拆分形态：本次加密出的密文交给它，config.json 只写外来密文。
// secrets 为空 = 自包含形态：密文直接写进 config.json 的 password_enc。
bool ConfigToJsonImpl(const AppConfig& cfg, std::string& out_json, SecretMap* secrets) {
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
            std::string enc;
            if (!ProfileToJson(p, pj, enc)) return false;  // 密码加密失败
            if (secrets) {
                if (!enc.empty()) (*secrets)[p.name] = enc;
                pj["password_enc"] = p.password_enc_raw;  // 外来密文跟着配置走
            } else {
                pj["password_enc"] = !enc.empty() ? enc : p.password_enc_raw;
            }
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

// 只解析、不解密密码（拆形态要先拿 secret.json 里的密文，见 ConfigLoadSplitFrom）
bool ParseConfigJson(const std::string& json_str, AppConfig& out, std::string& error) {
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
            // 缺键时回落到结构体默认值（不写字面量，避免默认值改了一处漏一处）
            t.probe_timeout_seconds = s.value("probe_timeout_seconds", t.probe_timeout_seconds);
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

}  // namespace

bool ConfigToJson(const AppConfig& cfg, std::string& out_json) {
    return ConfigToJsonImpl(cfg, out_json, nullptr);
}

bool ConfigFromJson(const std::string& json_str, AppConfig& out, std::string& error) {
    out = AppConfig{};
    if (!ParseConfigJson(json_str, out, error)) return false;
    ResolveInlinePasswords(out);
    return true;
}

namespace {

// 检测路径是否可被创建（成功返回 0 并清理探针文件；失败返回 Win32 错误码）
DWORD ProbeWritable(const fs::path& p) {
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(h);
    std::error_code e2;
    fs::remove(p, e2);
    return 0;
}

// 保存失败时的诊断：量化"写入被拒"的作用范围（仅失败路径调用）——
// 用于区分：只拒该文件名 / 只拒该目录 / 全盘拒绝 / 瞬时问题
std::string SaveFailureDiagnostics(const fs::path& tmp, DWORD last, int en) {
    const fs::path dir = tmp.parent_path();
    const DWORD e_tmp   = ProbeWritable(tmp);
    const DWORD e_txt   = ProbeWritable(dir / L"cnk_diag_probe.txt");
    const DWORD e_noext = ProbeWritable(dir / L"cnk_diag_probe");
    DWORD e_sys = 9999;
    {
        std::error_code e2;
        const fs::path t = fs::temp_directory_path(e2);
        if (!e2) e_sys = ProbeWritable(t / L"cnk_diag_probe.txt");
    }
    std::error_code e2;
    const bool dir_ok = fs::is_directory(dir, e2);

    std::string s = "（路径=" + tmp.u8string() +
                    "，Win32=" + std::to_string(last) +
                    "，errno=" + std::to_string(en) +
                    "，父目录" + (dir_ok ? "存在" : "不存在") +
                    "，进程=" + cnk::ProcessTokenContext() +
                    "，探测[原名=" + std::to_string(e_tmp) +
                    ", txt=" + std::to_string(e_txt) +
                    ", 无后缀=" + std::to_string(e_noext) +
                    ", 系统临时目录=" + std::to_string(e_sys) + "]）";
    return s;
}

}  // namespace

namespace {

// 原子写：先写 .tmp 再改名，写入中断不会留下半截文件
bool WriteJsonAtomic(const std::string& path, const std::string& json_str,
                     std::string& error) {
    if (path.empty()) { error = "配置路径为空"; return false; }

    // 目标目录可能尚不存在（如 --config 指向新目录）——确保父目录存在
    {
        std::error_code ec;
        fs::path parent = fs::u8path(path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
    }

    fs::path tmp = fs::u8path(path + ".tmp");
    {
        // 清掉可能卡住的旧残留（上次异常中断留下的 .tmp 会让后续每次保存
        // 都失败）。只读属性会阻止删除，先清属性再删；不存在或删除失败
        // 都不影响后续正常路径。
        {
            std::error_code e2;
            if (!fs::remove(tmp, e2) && fs::exists(tmp, e2)) {
                fs::permissions(tmp, fs::perms::owner_all,
                                fs::perm_options::add, e2);
                fs::remove(tmp, e2);
            }
        }
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            // 诊断增强：量化被拒范围（仅拒该文件名/该目录/全盘/瞬时），
            // 供环境侧问题一击定位（曾出现开发机多次无法复现的偶发失败）
            error = "无法写入临时配置文件" +
                    SaveFailureDiagnostics(tmp, GetLastError(), errno);
            return false;
        }
        ofs << json_str;
        ofs.flush();
        if (!ofs) {
            const std::string diag = SaveFailureDiagnostics(tmp, GetLastError(), errno);
            std::error_code e2;
            fs::remove(tmp, e2);   // 清理半截文件
            error = "写入临时配置文件失败" + diag;
            return false;
        }
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

}  // namespace

bool ConfigSaveTo(const std::string& path, const AppConfig& cfg, std::string& error) {
    error.clear();
    std::string json_str;
    if (!ConfigToJson(cfg, json_str)) { error = "序列化失败（密码加密失败）"; return false; }
    return WriteJsonAtomic(path, json_str, error);
}

bool ConfigSaveSplitTo(const std::string& config_path, const std::string& secret_path,
                       const AppConfig& cfg, std::string& error) {
    error.clear();
    if (config_path.empty()) { error = "配置路径为空"; return false; }
    if (secret_path.empty()) {   // 拿不到本机目录时退回自包含，宁可内联也不丢密码
        LogWarn("无法确定本机密码库路径，本次以自包含形态保存");
        return ConfigSaveTo(config_path, cfg, error);
    }

    SecretMap existing;
    std::string serr;
    if (!SecretLoadFrom(secret_path, existing, serr)) {
        // 读不出来时**先备份再继续**：坏文件里的条目已经解不开了，
        // 但备份留着，人工还能抢救；直接拿空库写盘才是真的删数据。
        LogWarn("密码库读取失败（" + serr + "），已备份为 " + secret_path + ".bak");
        std::error_code e2;
        fs::copy_file(fs::u8path(secret_path), fs::u8path(secret_path + ".bak"),
                      fs::copy_options::overwrite_existing, e2);
        existing.clear();
    }

    SecretMap fresh = existing;
    std::string json_str;
    if (!ConfigToJsonImpl(cfg, json_str, &fresh)) {
        error = "序列化失败（密码加密失败）";
        return false;
    }

    // 先落密码库再落配置：配置写失败时密码已经在本机了，下次保存会自动补上；
    // 反过来（先配置后密码）会短暂出现「配置说没密码、密码库有」的错位。
    if (fresh != existing)
        if (!SecretSaveTo(secret_path, fresh, serr)) {
            error = "保存密码库失败：" + serr;
            return false;
        }
    return WriteJsonAtomic(config_path, json_str, error);
}

bool ConfigSave(const AppConfig& cfg, std::string& error) {
    const std::string path = ConfigPath();
    if (path.empty()) { error = "无法确定配置目录"; return false; }
    return ConfigSaveSplitTo(path, SecretPath(), cfg, error);
}

namespace {

bool ReadConfigText(const std::string& path, std::string& content,
                    std::string& error) {
    content.clear();
    if (path.empty()) { error = "配置路径为空"; return false; }

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        error = "配置文件不存在";
        return false;
    }
    std::ifstream ifs(fs::u8path(path), std::ios::binary);
    if (!ifs) { error = "无法读取配置文件"; return false; }
    content.assign((std::istreambuf_iterator<char>(ifs)),
                   std::istreambuf_iterator<char>());
    return true;
}

// 损坏：备份后回落默认配置，绝不让上层拿到半个配置
void BackupCorruptConfig(const std::string& path, const std::string& why) {
    const fs::path bak = fs::u8path(path + ".bak");
    std::error_code e2;
    fs::copy_file(fs::u8path(path), bak, fs::copy_options::overwrite_existing, e2);
    if (e2) {
        LogError("配置文件损坏，且备份失败（" + e2.message() + "），已回落默认配置：" + why);
    } else {
        LogError("配置文件损坏，已备份到 " + path + ".bak：" + why);
    }
}

}  // namespace

bool ConfigLoadFrom(const std::string& path, AppConfig& out, std::string& error) {
    error.clear();
    out = AppConfig{};
    std::string content;
    if (!ReadConfigText(path, content, error)) return false;

    std::string parse_err;
    if (!ConfigFromJson(content, out, parse_err)) {
        BackupCorruptConfig(path, parse_err);
        out = AppConfig{};
        error = parse_err;
        return false;
    }
    return true;
}

bool ConfigLoadSplitFrom(const std::string& config_path, const std::string& secret_path,
                         AppConfig& out, std::string& error) {
    error.clear();
    out = AppConfig{};
    std::string content;
    if (!ReadConfigText(config_path, content, error)) return false;

    AppConfig cfg;
    std::string parse_err;
    if (!ParseConfigJson(content, cfg, parse_err)) {
        BackupCorruptConfig(config_path, parse_err);
        error = parse_err;
        return false;
    }

    // 密码库读不出来不是致命错误：配置里的非密码部分仍然可用，
    // 只是这次会话拿不到明文（并按「外来密文」提示重输）。
    SecretMap secrets;
    std::string serr;
    const bool have_secrets = SecretLoadFrom(secret_path, secrets, serr);
    if (!have_secrets) LogWarn("密码库读取失败（" + serr + "）：本次将按「无密码」处理");

    for (auto& p : cfg.profiles) {
        // 本机密码库优先（拆分形态的正路）
        if (have_secrets) {
            auto it = secrets.find(p.name);
            if (it != secrets.end()) {
                if (TryUnprotect(it->second, p)) continue;
                // 解不开的条目**留在库里**：SecretSaveTo 只更新有明文的档案，
                // 从不删除其它条目，所以这里什么都不做就等于「保全」。
                p.has_password = false;
                p.password_foreign = true;
                LogWarn("密码库中的密码无法解密（来自其他机器或 Windows 账号）：请在本机重新输入");
                continue;
            }
        }
        // 旧格式：密文还内联在 config.json 里（v0.1.5 及更早）
        if (p.password_enc_raw.empty()) continue;
        if (TryUnprotect(p.password_enc_raw, p)) continue;  // 解出来了，下次保存会搬进密码库
        p.has_password = false;
        p.password_foreign = true;  // 密文原样保留在 password_enc_raw
        LogWarn("配置中的密码无法解密（来自其他机器或 Windows 账号）：已保留原密文，请在本机重新输入");
    }

    out = std::move(cfg);
    return true;
}

bool ConfigLoad(AppConfig& out, std::string& error) {
    const std::string path = ConfigPath();
    if (path.empty()) {
        out = AppConfig{};  // 与 ConfigLoadFrom 语义一致：失败时不向调用方留下旧值
        error = "无法确定配置目录";
        return false;
    }
    return ConfigLoadSplitFrom(path, SecretPath(), out, error);
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

bool SetProfilePassword(AppConfig& cfg, const std::string& user,
                        const std::string& password, std::string& error) {
    error.clear();
    if (password.empty()) {
        error = "密码为空";
        return false;
    }

    Profile* target = nullptr;
    if (!user.empty()) {
        for (auto& p : cfg.profiles)
            if (p.username == user) { target = &p; break; }
        if (!target) {
            // 无同名档案：新建（首次录入的常规路径）
            Profile p;
            p.name = user;
            p.username = user;
            cfg.profiles.push_back(p);
            if (cfg.profiles.size() == 1) cfg.active_profile = p.name;
            target = &cfg.profiles.back();
        }
    } else {
        for (auto& p : cfg.profiles)
            if (p.name == cfg.active_profile) { target = &p; break; }
        if (!target && !cfg.profiles.empty()) target = &cfg.profiles.front();
        if (!target) {
            error = "配置中没有档案，请用 --user=账号 指定账号";
            return false;
        }
    }

    target->password = password;
    target->has_password = true;
    target->password_foreign = false;
    target->password_enc_raw.clear();  // 新密码取代外来密文，旧的可以放手了
    return true;
}

}  // namespace cnk
