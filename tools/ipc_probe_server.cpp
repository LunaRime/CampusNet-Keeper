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
            } else {
                r.ok = false;
                r.error = "未知命令（探针仅支持 ping/echo/blob）: " + req.cmd;
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
