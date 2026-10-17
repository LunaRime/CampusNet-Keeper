#pragma once
#include <functional>
#include <memory>
#include <string>

#include "ipc/Protocol.h"

namespace cnk {

// 当前用户的完整管道路径：`\\.\pipe\CampusNetKeeper.<用户 SID>`。
// 多用户机器上按 SID 隔离（不占用 Global\ 命名空间）。
// 客户端必须用同一规则拼名；C# 侧等价写法：
//     @"\\.\pipe\CampusNetKeeper." + WindowsIdentity.GetCurrent().User.Value
std::string PipeNameForCurrentUser();

// 命名管道 IPC 服务端（Windows 专用）。
//
// · **协议**：**一个连接只处理一条请求**（读到换行 → 应答一条 → 关闭连接）。
//   客户端每发一条请求新建一次连接（本地开销极小）。任务 23 的 C# 端按此实现。
// · **安全**：管道 ACL 只授予**当前用户 SID**，并置 PIPE_REJECT_REMOTE_CLIENTS，
//   不让本机其他账户或其他机器读到配置（配置里有账号与 DPAPI 密文引用）。
// · **并发**：同一时刻只有一个实例在监听；并发客户端会拿到 ERROR_PIPE_BUSY，
//   应 WaitNamedPipe 重试；**服务还没建出实例时会拿到 ERROR_FILE_NOT_FOUND，同样要重试**
//   （C# 的 NamedPipeClientStream 内建这两者的重试）。
// · **健壮性**：以下情况都不会占住或堵死服务线程——客户端连上不发数据、只发一半、
//   发完不读响应、处理器抛异常、响应过大。读/写都有超时，Stop() 有界返回。
class PipeServer {
public:
    using Handler = std::function<IpcResponse(const IpcRequest&)>;

    PipeServer();
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool Start(Handler handler, std::string& error);
    void Stop();

    // 读一条请求的超时（毫秒）。默认 3000。必须在 Start 之前调用。
    void SetReadTimeoutMs(int ms);

    // 自定义完整管道路径（含 `\\.\pipe\` 前缀）。必须在 Start 之前调用。
    // 不给就用 PipeNameForCurrentUser()。**测试专用**：本机可能正跑着守护进程，
    // 测试若用正式管道名会连到真守护上（"未知命令"、Stop 后仍有监听）→ 一串假失败。
    void SetName(const std::string& full_name);

    // 当前使用的完整管道路径（Start 之前也会算出来，便于日志与排查）
    std::string Name() const;

    // 服务线程是否在跑（Start 之后、Stop 之前）
    bool IsRunning() const;

    // 最近一次管道故障（例如管道名被别的进程以不同参数占用导致 CreateNamedPipeW 失败）。
    // 非空表示当前无法正常服务；服务会按退避自动重试，恢复后清空。
    std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
