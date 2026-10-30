#include "ipc/DaemonCommands.h"

#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include "auth/Detector.h"
#include "core/ConfigForUi.h"
#include "core/Encoding.h"
#include "core/Log.h"
#include "core/Probe.h"
#include "store/Config.h"
#include "store/PortableSecret.h"
#include "sys/Autostart.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>

namespace cnk {

namespace {

IpcResponse Fail(const IpcRequest& req, const std::string& error) {
    IpcResponse r;
    r.id = req.id;
    r.ok = false;
    r.error = error;
    return r;
}

IpcResponse Ok(const IpcRequest& req, const std::string& data_json) {
    IpcResponse r;
    r.id = req.id;
    r.ok = true;
    r.data_json = data_json;
    return r;
}

// 取字符串参数：缺键/类型不符都返回空串。
// 不用 nlohmann 的 value()——它在类型不匹配时**抛异常**，而这里是服务线程，
// 异常逃出去就是进程静默死亡（曾被数值型 id 坑过一次）。
std::string StrArg(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return {};
    const auto& v = j[key];
    if (!v.is_string()) return {};
    return v.get<std::string>();
}

}  // namespace

IpcResponse HandleDaemonCommand(const IpcRequest& req, Scheduler& sch) {
    if (req.cmd == "ping") {
        // 界面的存活探测。必须有这条：否则界面只能靠"未知命令"这种带错误的应答判断活着，
        // 语义含糊（2026-10-17 对真实守护进程跑契约时发现）。
        return Ok(req, R"({"state":"alive"})");
    }

    if (req.cmd == "get_status") {
        nlohmann::json d;
        d["state"] = RunStateName(sch.State());
        d["message"] = sch.LastMessage();
        d["local_ip"] = sch.LocalIp();
        // 真实的"上次登录成功时间"（Scheduler::LastSuccessUnix 一直存在；此前误判为缺失）
        d["last_success"] = sch.LastSuccessUnix();
        return Ok(req, d.dump());
    }

    if (req.cmd == "reconnect_now") {
        sch.ForceReconnect();
        LogInfo("设置界面触发了立即重连");
        return Ok(req, "{}");
    }

    if (req.cmd == "get_log_tail") {
        // 界面每 2 秒拉一次；行数上限压在响应上限（32KB）以内
        nlohmann::json d;
        d["lines"] = LogTail(200);
        return Ok(req, d.dump());
    }

    if (req.cmd == "get_config") {
        AppConfig cfg;
        std::string err;
        if (!ConfigLoad(cfg, err)) {
            // 配置损坏时也把错误送回去，而不是让界面以空白示人
            return Fail(req, "读取配置失败：" + err);
        }
        // ⚠ 这里必须走 ConfigForUiJson：它会剥掉 password_enc（DPAPI 密文，
        // 同用户的 UI 进程能解开它）只留 has_password。
        return Ok(req, ConfigForUiJson(cfg));
    }

    if (req.cmd == "set_config") {
        const nlohmann::json args = nlohmann::json::parse(req.args_json, nullptr, false);
        if (args.is_discarded() || !args.is_object() || !args.contains("config")) {
            return Fail(req, "set_config 缺少 config 参数");
        }
        // config 可以是对象，也可以是字符串形式的 JSON（两种客户端写法都容忍）
        std::string ui_json = args["config"].is_string() ? args["config"].get<std::string>()
                                                        : args["config"].dump();

        AppConfig existing;
        std::string lerr;
        const bool have_existing = ConfigLoad(existing, lerr);

        AppConfig merged;
        std::string merr;
        if (!MergeUiConfig(ui_json, have_existing ? existing : AppConfig{}, merged, merr)) {
            return Fail(req, merr);
        }

        std::string serr;
        if (!ConfigSave(merged, serr)) {
            return Fail(req, "保存失败：" + serr);
        }
        sch.SetConfig(merged);   // 热重载：不重启即生效

        // 开机自启是「配置项 → 注册表」的副作用：界面里一开关就要立刻生效，
        // 不能等下次启动（否则用户会以为开关没生效，重启后才发现没自启）。
        {
            std::string aerr;
            const bool want = merged.settings.autostart;
            if (!SyncAutostart(want, aerr)) {
                LogWarn("同步开机自启失败：" + aerr);
            } else {
                LogInfo(std::string("设置界面更新了开机自启：") +
                        (want ? "已登记（HKCU Run 键）" : "已关闭（已移除 Run 键）"));
            }
        }

        LogInfo("配置已由设置界面更新并热重载");
        return Ok(req, "{}");
    }

    if (req.cmd == "test_profile") {
        // 用指定档案做一次「探测 → 识别 → 登录」，把全过程回给界面（现场排查用）。
        // ⚠ 这是**用户主动触发**的联网动作（会真的尝试登录），不是后台自动行为。
        const nlohmann::json args = nlohmann::json::parse(req.args_json, nullptr, false);
        const std::string want =
            args.is_object() ? args.value("profile", std::string{}) : std::string{};

        AppConfig cfg;
        std::string err;
        if (!ConfigLoad(cfg, err)) return Fail(req, "读取配置失败：" + err);

        const Profile* target = nullptr;
        for (const auto& p : cfg.profiles)
            if (p.name == want) {
                target = &p;
                break;
            }
        if (target == nullptr && !cfg.profiles.empty()) target = &cfg.profiles.front();
        if (target == nullptr) return Fail(req, "没有可用的配置档案");
        if (target->password.empty())
            return Fail(req, target->password_foreign
                                 ? "该档案的密码来自其他电脑/账号，本机解不开：请重新输入，或用「密码迁移」导出导入"
                                 : "该档案没有可用密码（请先在设置界面输入并保存）");

        const ProbeResult pr = ProbeOnce(cfg.settings);
        const Vendor vendor = DetectVendor(pr.response);

        NetContext ctx;
        ctx.local_ip = sch.LocalIp();
        ctx.portal_url = cfg.settings.probe_url;
        ctx.portal_body = DecodeBody(pr.response);
        ctx.server_header = pr.response.server;

        auto auth = CreateAuth(vendor, *target);
        const LoginResult lr = auth->Login(*target, ctx);

        nlohmann::json d;
        d["probe_state"] = pr.state == NetState::Online    ? "online"
                           : pr.state == NetState::Offline ? "offline"
                                                           : "portal";
        d["probe_message"] = pr.message;
        d["vendor"] = VendorName(vendor);
        d["strategy"] = auth->Name();
        d["success"] = lr.success;
        d["message"] = lr.message;
        // 请求一律脱敏后再回给界面（与 --test-login 的输出同规矩）
        d["request_url"] = RedactSecrets(lr.request_url);
        d["request_body"] = RedactSecrets(lr.request_body);
        LogInfo(std::string("界面触发测试登录：") + (lr.success ? "成功" : "失败") +
                "，策略=" + auth->Name());
        return Ok(req, d.dump());
    }

    if (req.cmd == "get_carrier_options") {
        // 从最近一次门户页面解析运营商；取不到就回空数组，界面会回退内置预设
        nlohmann::json d;
        d["options"] = nlohmann::json::array();
        CarrierList cl;
        if (ParseCarrier(sch.LastPortalBody(), cl)) {
            d["title"] = cl.title;
            d["default_id"] = cl.default_id;
            for (const auto& o : cl.options) {
                d["options"].push_back({{"id", o.id}, {"name", o.name}, {"suffix", o.suffix}});
            }
        }
        return Ok(req, d.dump());
    }

    if (req.cmd == "export_secret" || req.cmd == "import_secret") {
        // 整段包住：路径可能含非法 UTF-8（fs::u8path 会抛），文件 IO 也可能抛。
        // 服务线程里异常逃出去 = 进程静默死亡（铁律）。
        try {
            // 密码迁移的两个动作都由守护进程做：明文只在守护进程里出现，
            // 界面只负责选路径、收口令（与 password_plain 同一条安全边界）。
            const nlohmann::json args = nlohmann::json::parse(req.args_json, nullptr, false);
            if (args.is_discarded() || !args.is_object())
                return Fail(req, "参数不是合法 JSON 对象");
            const std::string path = StrArg(args, "path");
            const std::string passphrase = StrArg(args, "passphrase");
            const std::string want = StrArg(args, "profile");
            if (path.empty()) return Fail(req, "缺少文件路径");
            if (passphrase.empty()) return Fail(req, "口令不能为空（至少 6 位）");

            if (req.cmd == "export_secret") {
                AppConfig cfg;
                std::string err;
                if (!ConfigLoad(cfg, err)) return Fail(req, "读取配置失败：" + err);

                const Profile* target = nullptr;
                for (const auto& p : cfg.profiles)
                    if (p.name == want || p.username == want) { target = &p; break; }
                if (target == nullptr) target = ActiveProfile(cfg);
                if (target == nullptr) return Fail(req, "没有可导出的档案");
                if (!target->has_password || target->password.empty())
                    return Fail(req, target->password_foreign
                                         ? "该档案的密码来自其他电脑/账号，本机解不开，无法导出"
                                         : "该档案还没有密码，无法导出");

                PortableSecret s;
                s.profile = target->name;
                s.username = target->username;
                s.password = target->password;

                std::string json, xerr;
                if (!ExportPortableSecret(s, passphrase, json, xerr))
                    return Fail(req, "导出失败：" + xerr);

                std::error_code ec;
                const std::filesystem::path p = std::filesystem::u8path(path);
                if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
                std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
                if (!ofs) return Fail(req, "无法写入该路径：" + path);
                ofs << json;
                ofs.flush();
                if (!ofs) return Fail(req, "写入失败：" + path);

                nlohmann::json d;
                d["path"] = path;
                d["profile"] = s.profile;
                d["username"] = s.username;
                LogInfo("界面导出了密码（档案=" + s.profile + "，文件=" + path + "）");
                return Ok(req, d.dump());
            }

            // import_secret
            std::ifstream ifs(std::filesystem::u8path(path), std::ios::binary);
            if (!ifs) return Fail(req, "无法读取该文件：" + path);
            const std::string text((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());

            PortableSecret s;
            std::string xerr;
            if (!ImportPortableSecret(text, passphrase, s, xerr))
                return Fail(req, "导入失败：" + xerr);

            AppConfig existing;
            std::string lerr;
            const bool have_existing = ConfigLoad(existing, lerr);
            AppConfig cfg = have_existing ? existing : AppConfig{};

            const std::string target_user = want.empty() ? s.username : want;
            if (target_user.empty()) return Fail(req, "文件里没有账号信息，请指定目标档案");
            const size_t before = cfg.profiles.size();
            if (!SetProfilePassword(cfg, target_user, s.password, xerr))
                return Fail(req, "写入密码失败：" + xerr);

            std::string serr;
            if (!ConfigSave(cfg, serr)) return Fail(req, "保存失败：" + serr);
            sch.SetConfig(cfg);

            nlohmann::json d;
            d["username"] = target_user;
            d["created_new_profile"] = cfg.profiles.size() > before;
            LogInfo("界面导入了密码（档案=" + target_user + "）");
            return Ok(req, d.dump());
        } catch (const std::exception& e) {
            return Fail(req, std::string("密码迁移失败：") + e.what());
        }
    }

    return Fail(req, "未知命令: " + req.cmd);
}

}  // namespace cnk
