#include "ipc/PipeServer.h"

#include "core/Log.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace cnk {

namespace {

constexpr int kDefaultReadTimeoutMs = 3000;   // 单条请求的读取总预算
constexpr DWORD kWriteTimeoutMs = 3000;       // 单个响应的写出总预算
constexpr size_t kMaxRequestBytes = 1024 * 1024;  // 单条请求上限（与计划一致）
constexpr DWORD kPipeBufferBytes = 64 * 1024;
// 响应上限取缓冲区一半：超过就可能写阻塞（客户端不读时把 IPC 线程钉死），
// 故超限一律换成错误响应，由客户端分页拉取（任务 25 的日志面板用 args 分页）。
constexpr size_t kMaxResponseBytes = kPipeBufferBytes / 2;
// 写完响应后等客户端「读走并关闭」的上限。
// ⚠ MSDN: DisconnectNamedPipe 会**丢弃未被读取的数据**，所以这个等待是响应送达的必要条件；
// 它必须有界（客户端连上不读时不能把服务线程钉死），因此协议要求客户端**及时读取并关闭**
//（见 PipeServer.h 的契约）。超时未关会记 WARN，便于现场发现"慢客户端丢响应"。
constexpr DWORD kClientCloseWaitMs = 500;
// 管道创建失败后的重试退避（名字被占用、资源紧张都可能临时失败）。
// 若永久失败也不放弃线程：IsRunning() 仍为 true，LastError() 非空，退避后继续试。
constexpr DWORD kCreateFailBackoffMs = 500;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// 当前用户 SID 字符串（形如 S-1-5-21-...）；失败返回空
std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};

    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::wstring sid;
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
            LPWSTR str = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &str)) {
                sid = str;
                LocalFree(str);
            }
        }
    }
    CloseHandle(token);
    return sid;
}

// 等一个 overlapped 操作完成；超时则取消，并等取消真正完成再返回
// （OVERLAPPED 与缓冲区随后离开作用域，未完成的 I/O 会写已释放内存）
bool WaitOrCancel(HANDLE pipe, OVERLAPPED& ov, DWORD timeout_ms, DWORD& transferred) {
    const DWORD w = WaitForSingleObject(ov.hEvent, timeout_ms);
    if (w == WAIT_OBJECT_0) {
        DWORD got = 0;
        if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) return false;
        transferred = got;
        return true;
    }
    CancelIoEx(pipe, &ov);
    // 等取消完成：失败（极少见）时 IRP 可能仍在飞，此处只能尽力并记录，
    // 由调用方把连接整体丢弃（不再复用该 OVERLAPPED/缓冲区）。
    if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0) {
        LogError("IPC 取消 I/O 后等待完成超时，已放弃该连接");
    }
    return false;
}

enum class ReadResult { Ok, Closed, Timeout };

ReadResult IssueRead(HANDLE pipe, OVERLAPPED& ov, char* buf, DWORD cap, DWORD timeout_ms,
                     DWORD& got) {
    ResetEvent(ov.hEvent);
    DWORD read = 0;
    if (ReadFile(pipe, buf, cap, &read, &ov)) {
        got = read;
        return ReadResult::Ok;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
        err == ERROR_OPERATION_ABORTED) {
        return ReadResult::Closed;
    }
    if (err != ERROR_IO_PENDING) return ReadResult::Closed;
    DWORD n = 0;
    if (WaitOrCancel(pipe, ov, timeout_ms, n)) {
        got = n;
        return ReadResult::Ok;
    }
    return ReadResult::Timeout;
}

bool IssueWrite(HANDLE pipe, OVERLAPPED& ov, const char* data, DWORD len, DWORD timeout_ms) {
    ResetEvent(ov.hEvent);
    DWORD written = 0;
    if (WriteFile(pipe, data, len, &written, &ov)) return written == len;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    DWORD got = 0;
    return WaitOrCancel(pipe, ov, timeout_ms, got) && got == len;
}

// 可被 Stop 打断的睡眠：避免退避把 Stop() 拖住
template <typename Pred>
void SleepInterruptible(DWORD ms, Pred still_running) {
    const DWORD deadline = GetTickCount() + ms;
    while (still_running() && GetTickCount() < deadline) Sleep(20);
}

}  // namespace

std::string PipeNameForCurrentUser() {
    std::wstring name = L"\\\\.\\pipe\\";
    name += Utf8ToWide(kPipeName);
    const std::wstring sid = CurrentUserSidString();
    if (!sid.empty()) {
        name += L".";
        name += sid;
    }
    return WideToUtf8(name);
}

struct PipeServer::Impl {
    std::wstring pipe_name;      // 完整路径（宽字符）
    std::wstring custom_name;    // SetName() 指定的路径（测试用）；空则用当前用户管道名
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;  // 必须活到所有 CreateNamedPipeW 之后
    HANDLE stop_event = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    Handler handler;
    int read_timeout_ms = kDefaultReadTimeoutMs;

    mutable std::mutex err_mutex;  // 保护 last_error（工作线程写，其它线程读）
    std::string last_error;

    void SetError(const std::string& e) {
        std::lock_guard<std::mutex> lk(err_mutex);
        last_error = e;
    }
    void ClearError() {
        std::lock_guard<std::mutex> lk(err_mutex);
        last_error.clear();
    }
    std::string Error() const {
        std::lock_guard<std::mutex> lk(err_mutex);
        return last_error;
    }

    void Loop();
    bool ServeOne(HANDLE pipe, OVERLAPPED& ov);  // 返回是否写出了响应
};

bool PipeServer::Impl::ServeOne(HANDLE pipe, OVERLAPPED& ov) {
    // 读一行（'\n' 结尾）。
    //  · Ok      → 正常拿到一行
    //  · Closed  → 客户端已关闭；若已收到部分数据，按"宽限"当成一条完整请求处理
    //              （计划原文就是这么做的，客户端漏发换行不该得不到任何响应）
    //  · Timeout → 客户端连上却不发；同样按宽限处理已收到的数据，没有数据则丢弃连接
    std::string line;
    {
        const DWORD start = GetTickCount();
        char buf[4096];
        for (;;) {
            const DWORD elapsed = GetTickCount() - start;
            if (elapsed >= (DWORD)read_timeout_ms) {
                if (!line.empty()) LogWarn("IPC 客户端未发送完整请求（无结尾换行），已按已收内容处理");
                break;
            }
            DWORD got = 0;
            const ReadResult rr =
                IssueRead(pipe, ov, buf, sizeof(buf), (DWORD)read_timeout_ms - elapsed, got);
            if (rr == ReadResult::Closed) break;  // 有数据就按宽限处理
            if (rr == ReadResult::Timeout) {
                if (!line.empty())
                    LogWarn("IPC 客户端未发送完整请求（无结尾换行），已按已收内容处理");
                break;
            }
            if (got == 0) break;
            line.append(buf, got);
            const size_t nl = line.find('\n');
            if (nl != std::string::npos) {
                line.resize(nl);
                break;
            }
            if (line.size() > kMaxRequestBytes) {
                line.clear();  // 超长请求：丢弃（下面按空处理 → 回错误响应）
                break;
            }
        }
    }

    IpcResponse resp;
    IpcRequest req;
    std::string err;
    if (!IpcParseRequest(line, req, err)) {
        resp.id = 0;  // id 都解析不出来时只能回 0，客户端据 ok=false 判断
        resp.ok = false;
        resp.error = err;
    } else if (!handler) {
        resp = IpcResponse{req.id, false, "{}", "无处理器"};
    } else {
        try {
            resp = handler(req);
        } catch (const std::exception& e) {
            resp = IpcResponse{req.id, false, "{}", std::string("处理器异常：") + e.what()};
        } catch (...) {
            resp = IpcResponse{req.id, false, "{}", "处理器异常"};
        }
        resp.id = req.id;  // 客户端靠 id 配对，处理器不得改写
    }

    // 序列化也必须在 try 里：dump() 可能因非法 UTF-8 抛异常（Protocol 已用 replace 策略
    // 兜底，这里是第二道保险）——异常逃出服务线程会 terminate 整个守护进程。
    std::string out;
    try {
        out = IpcSerialize(resp);
    } catch (const std::exception& e) {
        out = std::string("{\"id\":") + std::to_string(resp.id) +
              ",\"ok\":false,\"error\":\"序列化失败\"}";
        LogError(std::string("IPC 响应序列化失败：") + e.what());
    } catch (...) {
        out = std::string("{\"id\":") + std::to_string(resp.id) +
              ",\"ok\":false,\"error\":\"序列化失败\"}";
    }

    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());  // 单行契约
    if (out.size() > kMaxResponseBytes) {
        out = IpcSerialize(IpcResponse{resp.id, false, "{}",
                                       "响应过大（超过 " + std::to_string(kMaxResponseBytes) +
                                           " 字节），请分批请求"});
    }
    out += "\n";
    return IssueWrite(pipe, ov, out.data(), (DWORD)out.size(), kWriteTimeoutMs);
}

void PipeServer::Impl::Loop() {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        SetError("创建事件失败，错误码 " + std::to_string(GetLastError()));
        LogError("IPC " + Error());
        running.store(false);
        return;
    }

    int fail_count = 0;
    while (running.load()) {
        // 安全描述符**必须**真的传给 CreateNamedPipeW：计划原文构造了 SA 却传了
        // nullptr，「仅授予当前用户 SID」这条要求会完全落空（实测：那种写法会得到
        // 5 条 ACE，含 Everyone 与 ANONYMOUS LOGON 的读取权限）。
        // FILE_FLAG_OVERLAPPED：让连接/读/写都能限时，客户端挂起不会钉死服务。
        HANDLE pipe = CreateNamedPipeW(
            pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 5000, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            // 不永久放弃：记错误 + 退避重试（名字被占用、资源紧张都可能临时失败）
            const DWORD e = GetLastError();
            ++fail_count;
            SetError("创建命名管道失败，错误码 " + std::to_string(e));
            if (fail_count == 1 || fail_count % 20 == 0) {
                LogError("IPC " + Error() + "（已重试 " + std::to_string(fail_count) + " 次）");
            }
            SleepInterruptible(kCreateFailBackoffMs, [this] { return running.load(); });
            continue;
        }
        fail_count = 0;
        ClearError();

        ResetEvent(ov.hEvent);
        bool connected = false;
        const BOOL ok = ConnectNamedPipe(pipe, &ov);
        if (ok) {
            connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;  // 客户端抢在 ConnectNamedPipe 之前连上了
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {stop_event, ov.hEvent};
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) {  // Stop()
                    CancelIoEx(pipe, &ov);
                    WaitForSingleObject(ov.hEvent, 2000);
                } else if (w == WAIT_OBJECT_0 + 1) {
                    DWORD dummy = 0;
                    connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != FALSE;
                }
            }
        }

        if (connected) {
            bool wrote = false;
            if (running.load()) wrote = ServeOne(pipe, ov);
            // 等到客户端把响应读走并关闭再断开：MSDN 明确 DisconnectNamedPipe 会丢弃
            // 未读数据，所以这里既不能省（会丢响应），也不能无界（会被挂起客户端钉死）。
            if (wrote && running.load()) {
                const DWORD deadline = GetTickCount() + kClientCloseWaitMs;
                for (;;) {
                    DWORD avail = 0;
                    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
                        break;  // 客户端已关闭 → 数据已读走
                    if (GetTickCount() >= deadline) {
                        LogWarn("IPC 客户端未在 " + std::to_string(kClientCloseWaitMs) +
                                "ms 内读取响应，断开该连接（响应可能丢失）");
                        break;
                    }
                    Sleep(5);
                }
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }

    CloseHandle(ov.hEvent);
    ov.hEvent = nullptr;
}

PipeServer::PipeServer() : impl_(new Impl) {}

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(Handler handler, std::string& error) {
    error.clear();
    if (impl_->running.load()) {
        error = "IPC 服务已在运行";
        return false;
    }
    // 上一轮的线程若还在（理论上 Stop 已 join），先收干净再起新的，
    // 绝不能对 joinable 线程重新赋值（会 std::terminate）。
    if (impl_->worker.joinable()) impl_->worker.join();

    impl_->pipe_name = impl_->custom_name.empty()
        ? Utf8ToWide(PipeNameForCurrentUser())
        : impl_->custom_name;
    if (impl_->pipe_name.empty()) {
        error = "无法生成管道名";
        return false;
    }

    // 安全：D:(A;;GA;;;<当前用户 SID>) —— 只授予当前用户。
    // 拿不到 SID 或构造失败时**拒绝启动**（失败即关闭），绝不以宽松默认描述符兜底：
    // 管道里传的是账号、配置与操作指令。
    const std::wstring sid = CurrentUserSidString();
    if (sid.empty()) {
        error = "无法获取当前用户 SID（不能以宽松权限启动 IPC）";
        return false;
    }
    const std::wstring sddl = L"D:(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &impl_->sd, nullptr)) {
        error = "构造管道安全描述符失败，错误码 " + std::to_string(GetLastError());
        impl_->sd = nullptr;
        return false;
    }
    impl_->sa = SECURITY_ATTRIBUTES{};
    impl_->sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    impl_->sa.lpSecurityDescriptor = impl_->sd;
    impl_->sa.bInheritHandle = FALSE;

    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stop_event) {
        error = "创建停止事件失败，错误码 " + std::to_string(GetLastError());
        LocalFree(impl_->sd);
        impl_->sd = nullptr;
        return false;
    }

    impl_->ClearError();
    impl_->handler = std::move(handler);  // 必须在建线程之前赋值（工作线程会读它）
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->Loop(); });
    LogInfo("IPC 已启动：" + WideToUtf8(impl_->pipe_name));
    return true;
}

void PipeServer::Stop() {
    if (!impl_) return;
    impl_->running.store(false);
    if (impl_->stop_event) SetEvent(impl_->stop_event);  // 唤醒等待连接的线程
    // ⚠ 无论 running 之前是什么值都必须 join：工作线程会因故障自行退出并把 running
    // 置 false，若这里再按 running 判断就会漏 join，而 ~std::thread 对 joinable 线程
    // 会直接 std::terminate（2026-10-16 审阅 P1-1）。
    if (impl_->worker.joinable()) impl_->worker.join();

    if (impl_->stop_event) {
        CloseHandle(impl_->stop_event);
        impl_->stop_event = nullptr;
    }
    // 到这里所有 CreateNamedPipeW 都已结束，描述符可以释放
    if (impl_->sd) {
        LocalFree(impl_->sd);
        impl_->sd = nullptr;
    }
    impl_->handler = nullptr;
}

void PipeServer::SetReadTimeoutMs(int ms) {
    if (ms > 0) impl_->read_timeout_ms = ms;
}

void PipeServer::SetName(const std::string& full_name) {
    impl_->custom_name = Utf8ToWide(full_name);
}

std::string PipeServer::Name() const {
    if (!impl_->pipe_name.empty()) return WideToUtf8(impl_->pipe_name);
    if (!impl_->custom_name.empty()) return WideToUtf8(impl_->custom_name);
    return PipeNameForCurrentUser();
}

bool PipeServer::IsRunning() const { return impl_->running.load(); }

std::string PipeServer::LastError() const { return impl_->Error(); }

}  // namespace cnk
