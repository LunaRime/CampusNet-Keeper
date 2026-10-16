#include "ipc/DaemonCommands.h"

#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include "auth/Detector.h"
#include "core/ConfigForUi.h"
#include "core/Encoding.h"
#include "core/Log.h"
#include "core/Probe.h"
#include "store/Config.h"
#include "json.hpp"

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
            return Fail(req, "该档案没有可用密码（请先在设置界面输入并保存）");

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

    return Fail(req, "未知命令: " + req.cmd);
}

}  // namespace cnk
