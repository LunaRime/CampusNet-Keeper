// 跨语言契约自检用的**探针服务端**（测试工具，不随产品发布）。
//
// 用途：让任务 23 的 C# 客户端能对着**真实的 C++ PipeServer** 跑契约验证
//（受限沙箱禁止打开命名管道，故本工具需在非沙箱下运行）。
// 打印管道名后进入服务循环，按回车退出。
//
// 构建：随主工程一起（CMake 目标 cnk_ipc_probe）
// 用法：build-m1\Release\cnk_ipc_probe.exe [--seconds=N]
//       --seconds=N 只服务 N 秒后自行退出（脚本里用；不给则等回车）
#include "ipc/PipeServer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    int seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--seconds=", 0) == 0) seconds = std::atoi(a.c_str() + 10);
    }

    cnk::PipeServer srv;
    std::string err;
    const bool ok = srv.Start(
        [](const cnk::IpcRequest& req) {
            cnk::IpcResponse r;
            r.id = req.id;
            if (req.cmd == "ping") {
                r.ok = true;
                r.data_json = R"({"state":"online"})";
            } else if (req.cmd == "echo") {
                // 把 args 原样回传，用于验证 UTF-8 与 JSON 往返
                r.ok = true;
                r.data_json = req.args_json;
            } else if (req.cmd == "blob") {
                // 超过 32KB，触发服务端的「响应过大」保护
                r.ok = true;
                r.data_json = "{\"blob\":\"" + std::string(40000, 'x') + "\"}";
            } else if (req.cmd == "get_status") {
                // 示例状态：让设置界面能显示"在线"这一真实形态（任务 25 才接真状态机）。
                // IP 用 TEST-NET-1 文档网段，避免把任何真实地址写进仓库。
                r.ok = true;
                r.data_json =
                    R"({"state":"online","message":"在线（示例数据）","local_ip":"192.0.2.5",)"
                    R"("last_success":1750000000})";
            } else if (req.cmd == "get_config") {
                // 示例配置：**全部为假数据**（不涉及任何真实账号）
                r.ok = true;
                r.data_json =
                    R"({"version":1,"active_profile":"默认","settings":{)"
                    R"("probe_interval_seconds":5,"probe_timeout_seconds":6,)"
                    R"("probe_url":"http://captive.apple.com/hotspot-detect.html",)"
                    R"("probe_expect":"Success","retry_backoff_seconds":[5,10,20,30,60],)"
                    R"("autostart":true,"notify_on_reconnect":true,"log_level":"info",)"
                    R"("log_max_kb":512,"log_keep_files":3},"profiles":[{)"
                    R"("name":"默认","auth_url":"http://10.0.0.1/","username":"202400000001@cmcc",)"
                    R"("vendor":"auto","isp":{"preset":"cmcc","custom":""},)"
                    R"("custom_template":{"method":"POST","url":"","body":"",)"
                    R"("success_regex":"","headers":{}},"has_password":true}]})";
            } else if (req.cmd == "get_carrier_options") {
                // 形态与守护进程一致：{"options":[{"id","name","suffix"}], title, default_id}
                r.ok = true;
                r.data_json =
                    R"({"title":"请选择运营商","default_id":"cmcc","options":[)"
                    R"({"id":"cmcc","name":"中国移动","suffix":"@cmcc"},)"
                    R"({"id":"telecom","name":"中国电信","suffix":"@telecom"},)"
                    R"({"id":"campus","name":"校园网（默认）","suffix":""}]})";
            } else if (req.cmd == "get_log_tail") {
                // 形态与守护进程一致：{"lines":[...]}
                r.ok = true;
                r.data_json =
                    R"({"lines":[)"
                    R"("2026-10-16 23:10:01 [INFO ] 进入托盘守护循环（示例数据）",)"
                    R"("2026-10-16 23:10:06 [INFO ] 探测结果：Online（第 1 次探测）",)"
                    R"("2026-10-16 23:10:06 [INFO ] 设置界面 IPC 已就绪",)"
                    R"("2026-10-16 23:10:11 [INFO ] 探测结果：Online（第 2 次探测）"]})";
            } else {
                r.ok = false;
                r.error =
                    "未知命令（探针支持 ping/echo/blob/get_status/get_config/get_carrier_options）: " +
                    req.cmd;
            }
            return r;
        },
        err);
    if (!ok) {
        std::fprintf(stderr, "探针启动失败: %s\n", err.c_str());
        return 2;
    }

    // 供脚本抓取；同时给出默认名字规则的结果，二者应一致
    std::printf("PIPE=%s\n", srv.Name().c_str());
    std::fflush(stdout);
    if (seconds > 0) {
        std::fprintf(stderr, "探针服务端已就绪（命令: ping / echo / blob），服务 %d 秒\n", seconds);
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } else {
        std::fprintf(stderr, "探针服务端已就绪（命令: ping / echo / blob），按回车退出\n");
        std::getchar();
    }

    srv.Stop();
    return 0;
}
