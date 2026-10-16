#include "ipc/DaemonCommands.h"

#include "auth/Carrier.h"
#include "core/ConfigForUi.h"
#include "core/Log.h"
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
    if (req.cmd == "get_status") {
        nlohmann::json d;
        d["state"] = RunStateName(sch.State());
        d["message"] = sch.LastMessage();
        d["local_ip"] = sch.LocalIp();
        // TODO(任务 25 收尾)：Scheduler 目前没有暴露"上次成功时间"，先给 0（界面显示"从未"）。
        // 计划原文引用的 sch->LastSuccessUnix() 在代码里并不存在。
        d["last_success"] = 0;
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
