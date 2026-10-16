#include "doctest.h"
#include "core/Log.h"
#include "ipc/PipeServer.h"
#include "ipc/Protocol.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---- 测试侧的极简客户端（直接 Win32，不依赖产品代码） ----

struct ConnectResult {
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD last_error = 0;  // 失败原因（写进断言消息，避免"连不上"这种无信息失败）
};

// 连接服务端。两种"暂时性"失败都要重试：
//  · ERROR_FILE_NOT_FOUND：服务线程还没把实例建出来（启动竞态）
//  · ERROR_PIPE_BUSY：实例正在服务别的连接
// 这两条也是任务 23 的 C# 客户端必须实现的契约（NamedPipeClientStream 内建）。
ConnectResult ConnectToServerEx(const std::string& pipe_name, DWORD timeout_ms = 3000) {
    const std::wstring wname(pipe_name.begin(), pipe_name.end());
    const DWORD deadline = GetTickCount() + timeout_ms;
    DWORD last = 0;
    for (;;) {
        HANDLE h = CreateFileW(wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return {h, 0};
        last = GetLastError();
        if (last != ERROR_PIPE_BUSY && last != ERROR_FILE_NOT_FOUND)
            return {INVALID_HANDLE_VALUE, last};
        if (last == ERROR_PIPE_BUSY) WaitNamedPipeW(wname.c_str(), 100);
        if (GetTickCount() > deadline) return {INVALID_HANDLE_VALUE, last};
        Sleep(10);
    }
}

HANDLE ConnectToServer(const std::string& pipe_name, DWORD timeout_ms = 3000) {
    return ConnectToServerEx(pipe_name, timeout_ms).h;
}

// 受限沙箱（DSH confined 模式）禁止**打开**命名管道：此时连「不给安全描述符、
// 系统默认权限」的管道都返回 ACCESS_DENIED（已实测，见开发日志）。
// 这类环境无法验证管道链路，用例明确跳过并打印原因——不伪装通过。
// 真实结论以「非沙箱运行」为准（cmake 构建目录下直接跑 cnk_tests.exe）。
bool PipeEnvAvailable() {
    static const bool cached = [] {
        HANDLE p = CreateNamedPipeW(L"\\\\.\\pipe\\CnkEnvProbe", PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096,
                                    4096, 1000, nullptr);
        if (p == INVALID_HANDLE_VALUE) return false;
        HANDLE c = CreateFileW(L"\\\\.\\pipe\\CnkEnvProbe", GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        const bool ok = c != INVALID_HANDLE_VALUE;
        if (ok) CloseHandle(c);
        CloseHandle(p);
        if (!ok) {
            std::cout << "[IPC 测试] 本环境禁止打开命名管道（DSH 受限沙箱）→ 跳过管道用例；"
                         "协议与生命周期用例仍会真跑；真实结论须在非沙箱下运行 cnk_tests.exe"
                      << std::endl;
        }
        return ok;
    }();
    return cached;
}

#define CNK_REQUIRE_PIPE_ENV()                                                          \
    do {                                                                                \
        if (!PipeEnvAvailable()) {                                                      \
            MESSAGE("跳过：本环境禁止打开命名管道（DSH 受限沙箱），管道用例须在非沙箱下运行"); \
            return;                                                                     \
        }                                                                               \
    } while (false)

bool WriteAll(HANDLE h, const std::string& s) {
    DWORD written = 0;
    return WriteFile(h, s.data(), (DWORD)s.size(), &written, nullptr) && written == s.size();
}

// 读一行（\n 结尾）；带超时，避免测试挂死
std::string ReadLine(HANDLE h, DWORD timeout_ms = 3000) {
    std::string buf;
    const DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return buf;
        if (avail > 0) {
            char chunk[512];
            DWORD got = 0;
            const DWORD want = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
            if (!ReadFile(h, chunk, want, &got, nullptr) || got == 0) return buf;
            buf.append(chunk, got);
            if (buf.find('\n') != std::string::npos) break;
        } else {
            if (GetTickCount() > deadline) break;
            Sleep(5);
        }
    }
    const size_t nl = buf.find('\n');
    return nl == std::string::npos ? buf : buf.substr(0, nl);
}

// 一次完整往返
std::string RoundTrip(const std::string& pipe_name, const std::string& request,
                      DWORD* connect_error = nullptr) {
    const ConnectResult c = ConnectToServerEx(pipe_name);
    if (connect_error) *connect_error = c.last_error;
    if (c.h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    if (WriteAll(c.h, request + "\n")) out = ReadLine(c.h);
    CloseHandle(c.h);
    return out;
}

}  // namespace

// ================= Protocol =================

TEST_CASE("IPC 请求解析正确") {
    cnk::IpcRequest r;
    std::string err;
    REQUIRE(cnk::IpcParseRequest(R"({"id":7,"cmd":"get_status","args":{}})", r, err));
    CHECK(r.id == 7);
    CHECK(r.cmd == "get_status");
    CHECK(r.args_json == "{}");
}

TEST_CASE("IPC 缺 id 或 cmd 的请求被拒绝并给出错误") {
    cnk::IpcRequest r;
    std::string err;
    CHECK_FALSE(cnk::IpcParseRequest(R"({"cmd":"x"})", r, err));
    CHECK(err.size() > 0);
    CHECK_FALSE(cnk::IpcParseRequest(R"({"id":1})", r, err));
    CHECK(err.size() > 0);
}

TEST_CASE("IPC 非 JSON / 非对象被拒绝而不崩溃") {
    cnk::IpcRequest r;
    std::string err;
    CHECK_FALSE(cnk::IpcParseRequest("not json", r, err));
    CHECK(err.size() > 0);
    CHECK_FALSE(cnk::IpcParseRequest("[1,2,3]", r, err));
    CHECK(err.size() > 0);
    CHECK_FALSE(cnk::IpcParseRequest("", r, err));
    CHECK(err.size() > 0);
}

TEST_CASE("IPC id 类型不符必须拒绝（不能靠异常）") {
    cnk::IpcRequest r;
    std::string err;
    CHECK_FALSE(cnk::IpcParseRequest(R"({"id":"7","cmd":"x"})", r, err));
    CHECK(err.find("id") != std::string::npos);
    CHECK_FALSE(cnk::IpcParseRequest(R"({"id":7.5,"cmd":"x"})", r, err));  // 浮点不是整数
    CHECK_FALSE(cnk::IpcParseRequest(R"({"id":7,"cmd":9})", r, err));
}

TEST_CASE("IPC args 原样保留，缺省为 {}") {
    cnk::IpcRequest r;
    std::string err;
    REQUIRE(cnk::IpcParseRequest(R"({"id":1,"cmd":"c","args":{"profile":"默认"}})", r, err));
    CHECK(r.args_json.find("默认") != std::string::npos);
    REQUIRE(cnk::IpcParseRequest(R"({"id":2,"cmd":"c"})", r, err));
    CHECK(r.args_json == "{}");
}

TEST_CASE("IPC 响应序列化为单行且可往返") {
    cnk::IpcResponse resp;
    resp.id = 3;
    resp.ok = true;
    resp.data_json = R"({"state":"online"})";
    const std::string line = cnk::IpcSerialize(resp);
    CHECK(line.find('\n') == std::string::npos);

    cnk::IpcResponse parsed;
    std::string err;
    REQUIRE(cnk::IpcParseResponse(line, parsed, err));
    CHECK(parsed.id == 3);
    CHECK(parsed.ok);
    CHECK(parsed.data_json == R"({"state":"online"})");
}

TEST_CASE("IPC 错误响应序列化带上 error 字段") {
    cnk::IpcResponse resp;
    resp.id = 1;
    resp.ok = false;
    resp.error = "未知命令";
    cnk::IpcResponse parsed;
    std::string err;
    REQUIRE(cnk::IpcParseResponse(cnk::IpcSerialize(resp), parsed, err));
    CHECK_FALSE(parsed.ok);
    CHECK(parsed.error == "未知命令");
}

TEST_CASE("IPC 非法 data_json 降级为 {} 而不是抛异常") {
    cnk::IpcResponse resp;
    resp.id = 5;
    resp.ok = true;
    resp.data_json = "not json";
    cnk::IpcResponse parsed;
    std::string err;
    REQUIRE(cnk::IpcParseResponse(cnk::IpcSerialize(resp), parsed, err));
    CHECK(parsed.ok);
    CHECK(parsed.data_json == "{}");
}

TEST_CASE("IPC 响应解析对类型不符必须宽松（不抛异常）") {
    // value() 在键存在但类型不符时会抛 type_error；若逃出服务线程会把服务打死
    cnk::IpcResponse parsed;
    std::string err;
    REQUIRE(cnk::IpcParseResponse(R"({"id":"x","ok":1,"data":[]})", parsed, err));
    CHECK(parsed.id == 0);        // 类型不符 → 取默认值
    CHECK_FALSE(parsed.ok);
    CHECK(parsed.data_json == "[]");
    CHECK_FALSE(cnk::IpcParseResponse("not json", parsed, err));
    CHECK(err.size() > 0);
}

// ================= PipeServer =================

TEST_CASE("IPC 管道名限定当前用户") {
    const std::string name = cnk::PipeNameForCurrentUser();
    CHECK(name.rfind("\\\\.\\pipe\\CampusNetKeeper.", 0) == 0);
    CHECK(name.find("S-1-") != std::string::npos);  // 含当前用户 SID
}

TEST_CASE("IPC 往返：请求进、响应出，且可连续服务多个连接") {
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    std::string err;
    REQUIRE(srv.Start(
        [](const cnk::IpcRequest& req) {
            cnk::IpcResponse r;
            r.id = req.id;
            if (req.cmd == "get_status") {
                r.ok = true;
                r.data_json = R"({"state":"online"})";
            } else {
                r.ok = false;
                r.error = "未知命令: " + req.cmd;
            }
            return r;
        },
        err));
    const std::string name = cnk::PipeNameForCurrentUser();

    const std::string a = RoundTrip(name, R"({"id":7,"cmd":"get_status","args":{}})");
    REQUIRE_FALSE(a.empty());
    CHECK(a.find("\"id\":7") != std::string::npos);
    CHECK(a.find("\"ok\":true") != std::string::npos);
    CHECK(a.find("online") != std::string::npos);

    // 第二个连接：证明实例会被重新创建（不是一次性服务）
    const std::string b = RoundTrip(name, R"({"id":8,"cmd":"nope"})");
    REQUIRE_FALSE(b.empty());
    CHECK(b.find("\"id\":8") != std::string::npos);
    CHECK(b.find("\"ok\":false") != std::string::npos);
    CHECK(b.find("未知命令") != std::string::npos);

    srv.Stop();
}

TEST_CASE("IPC 收到非法请求时回错误响应而不是打死服务") {
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest& req) {
        cnk::IpcResponse r;
        r.id = req.id;
        r.ok = true;
        return r;
    }, err));
    const std::string name = cnk::PipeNameForCurrentUser();

    const std::string bad = RoundTrip(name, "this is not json");
    REQUIRE_FALSE(bad.empty());
    CHECK(bad.find("\"ok\":false") != std::string::npos);
    CHECK(bad.find("\"id\":0") != std::string::npos);

    // 服务仍可用
    const std::string good = RoundTrip(name, R"({"id":1,"cmd":"ping"})");
    REQUIRE_FALSE(good.empty());
    CHECK(good.find("\"ok\":true") != std::string::npos);

    srv.Stop();
}

TEST_CASE("IPC 管道 ACL 只授予当前用户（不得是 Everyone）") {
    // 计划原文构造了 SA 却把 CreateNamedPipeW 的 lpSecurityAttributes 传成 nullptr，
    // 使「仅授予当前用户 SID」这条安全要求完全落空。本用例打开管道读其 DACL 来钉住。
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err));

    HANDLE h = ConnectToServer(cnk::PipeNameForCurrentUser());
    REQUIRE(h != INVALID_HANDLE_VALUE);

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                     nullptr, &dacl, nullptr, &sd);
    REQUIRE(rc == ERROR_SUCCESS);
    REQUIRE(dacl != nullptr);

    // 当前用户 SID 字符串（用于比对 ACE）
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    REQUIRE(GetTokenInformation(token, TokenUser, buf.data(), len, &len));
    LPWSTR sid_str = nullptr;
    REQUIRE(ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &sid_str));
    const std::wstring me = sid_str;
    LocalFree(sid_str);
    CloseHandle(token);

    ACL_SIZE_INFORMATION info{};
    REQUIRE(GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation));
    int allow_aces = 0;
    bool grants_everyone = false;
    bool grants_me = false;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* ace = nullptr;
        REQUIRE(GetAce(dacl, i, &ace));
        auto* hdr = (ACE_HEADER*)ace;
        if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        ++allow_aces;
        auto* a = (ACCESS_ALLOWED_ACE*)ace;
        PSID sid = (PSID)&a->SidStart;
        const DWORD sid_len = GetLengthSid(sid);
        std::vector<BYTE> tmp(sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_len);
        auto* a2 = (ACCESS_ALLOWED_ACE*)tmp.data();
        a2->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
        memcpy(&a2->SidStart, sid, sid_len);
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW((PSID)&a2->SidStart, &s)) {
            if (me == s) grants_me = true;
            if (wcscmp(s, L"S-1-1-0") == 0) grants_everyone = true;  // Everyone
            LocalFree(s);
        }
    }

    CHECK(grants_me);
    CHECK_FALSE(grants_everyone);  // 关键：不得给 Everyone
    CHECK(allow_aces >= 1);

    CloseHandle(h);
    LocalFree(sd);
    srv.Stop();
}

TEST_CASE("IPC 挂起的客户端不得永久占住服务（有读取超时）") {
    // 客户端连上但不发请求：旧实现会阻塞在 ReadFile 上，把服务永久占住，
    // 后续任何连接都得不到响应（本地 UI 崩溃即可触发）。
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    srv.SetReadTimeoutMs(300);  // 测试用短超时；默认 3000
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest& req) {
        cnk::IpcResponse r;
        r.id = req.id;
        r.ok = true;
        return r;
    }, err));
    const std::string name = cnk::PipeNameForCurrentUser();

    HANDLE hung = ConnectToServer(name);
    REQUIRE(hung != INVALID_HANDLE_VALUE);
    // 连上但什么都不发
    Sleep(600);  // 超过读取超时，服务应已放弃并回收该实例

    const std::string after = RoundTrip(name, R"({"id":42,"cmd":"ping"})");
    REQUIRE_FALSE(after.empty());
    CHECK(after.find("\"id\":42") != std::string::npos);
    CHECK(after.find("\"ok\":true") != std::string::npos);

    CloseHandle(hung);
    srv.Stop();
}

TEST_CASE("IPC 处理器抛异常时回错误响应而不是打死服务") {
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest& req) -> cnk::IpcResponse {
        if (req.cmd == "boom") throw std::runtime_error("处理器内部爆炸");
        cnk::IpcResponse r;
        r.id = req.id;
        r.ok = true;
        return r;
    }, err));
    const std::string name = cnk::PipeNameForCurrentUser();

    const std::string boom = RoundTrip(name, R"({"id":1,"cmd":"boom"})");
    REQUIRE_FALSE(boom.empty());
    CHECK(boom.find("\"ok\":false") != std::string::npos);
    CHECK(boom.find("\"id\":1") != std::string::npos);

    // 服务必须还活着（异常绝不允许逃出服务线程 → 会 terminate 整个进程）
    const std::string ok = RoundTrip(name, R"({"id":2,"cmd":"ping"})");
    REQUIRE_FALSE(ok.empty());
    CHECK(ok.find("\"ok\":true") != std::string::npos);

    srv.Stop();
}

TEST_CASE("IPC 响应过大时明确拒绝而不是写出半截") {
    // 守护进程的日志面板会拉日志；若某次响应超过管道缓冲区，
    // 而客户端又不读，阻塞写会把 IPC 线程钉死。故超限一律换成错误响应。
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest& req) {
        cnk::IpcResponse r;
        r.id = req.id;
        r.ok = true;
        r.data_json = "{\"blob\":\"" + std::string(40000, 'x') + "\"}";
        return r;
    }, err));
    const std::string name = cnk::PipeNameForCurrentUser();

    const std::string big = RoundTrip(name, R"({"id":3,"cmd":"dump"})");
    REQUIRE_FALSE(big.empty());
    CHECK(big.find("\"ok\":false") != std::string::npos);
    CHECK(big.find("过大") != std::string::npos);
    CHECK(big.size() < 40000);  // 没有把 40KB 发出来

    srv.Stop();
}

TEST_CASE("IPC 客户端发完请求就不读响应，也不得钉死服务") {
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    srv.SetReadTimeoutMs(500);
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest& req) {
        cnk::IpcResponse r;
        r.id = req.id;
        r.ok = true;
        r.data_json = "{\"pad\":\"" + std::string(2000, 'y') + "\"}";
        return r;
    }, err));
    const std::string name = cnk::PipeNameForCurrentUser();

    HANDLE rude = ConnectToServer(name);
    REQUIRE(rude != INVALID_HANDLE_VALUE);
    REQUIRE(WriteAll(rude, std::string(R"({"id":9,"cmd":"dump"})") + "\n"));
    // 知道我们会回 1 条就够：此刻它是有效的，只是我们回完就关
    // （服务端要能自己走出来，不能等客户端读）
    Sleep(300);

    const std::string after = RoundTrip(name, R"({"id":10,"cmd":"ping"})");
    REQUIRE_FALSE(after.empty());
    CHECK(after.find("\"id\":10") != std::string::npos);
    CHECK(after.find("\"ok\":true") != std::string::npos);

    CloseHandle(rude);
    srv.Stop();
}

TEST_CASE("IPC 序列化遇到非法 UTF-8 不抛异常") {
    // 审阅 P1-2：nlohmann 的 dump() 对非法 UTF-8 默认抛 type_error.316，而这个 dump
    // 发生在服务线程里——异常逃出去就是 std::terminate（守护进程整体崩）。
    // 服务端的 error 文案可能来自系统 API（GBK/ANSI 字节），并非总是合法 UTF-8。
    cnk::IpcResponse resp;
    resp.id = 1;
    resp.ok = false;
    resp.error = std::string("坏字节 ") + "\xC3\x28";  // 非法 UTF-8 序列
    std::string out;
    CHECK_NOTHROW(out = cnk::IpcSerialize(resp));
    CHECK(out.find('\n') == std::string::npos);
    cnk::IpcResponse back;
    std::string err;
    CHECK(cnk::IpcParseResponse(out, back, err));
    CHECK_FALSE(back.ok);
}

TEST_CASE("IPC 生命周期与命名冲突：不得漏 join（否则 std::terminate）") {
    // 审阅 P1-1：工作线程自己因故退出（例如管道名被别的进程以不同参数占用）时，
    // 若 Stop 只依据 running 标志决定是否 join，就会漏 join，而
    // ~std::thread 对 joinable 线程会直接 std::terminate。
    {
        cnk::PipeServer srv;
        std::string err;
        CHECK(srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err));
        CHECK(srv.IsRunning());
        // 二次 Start 必须被拒绝（不能悄悄丢掉旧线程）
        CHECK_FALSE(srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err));
        srv.Stop();
        srv.Stop();  // 幂等
        CHECK_FALSE(srv.IsRunning());
        // Stop 之后可以重新 Start（线程已 join）
        CHECK(srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err));
        srv.Stop();
    }
    {
        cnk::PipeServer srv;  // 不显式 Stop 直接析构
        std::string err;
        REQUIRE(srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err));
    }
    {
        // 命名冲突：先用**不同参数**占住同名管道 → 后者的 CreateNamedPipeW 必然失败
        const std::string name = cnk::PipeNameForCurrentUser();
        const std::wstring wname(name.begin(), name.end());
        HANDLE squatter = CreateNamedPipeW(
            wname.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 1000, nullptr);
        REQUIRE(squatter != INVALID_HANDLE_VALUE);

        cnk::PipeServer srv;
        std::string err;
        srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err);
        Sleep(1200);                           // 让工作线程走到失败分支
        CHECK_FALSE(srv.LastError().empty());  // 失败必须留可诊断信息，而不是静默
        CloseHandle(squatter);
    }  // ← 析构不得 terminate（本条用例的核心）
}

TEST_CASE("IPC 客户端发完请求不关也不换行，也要得到应答") {
    // 审阅 P2-1：计划原文在 EOF 时会解析已缓冲的数据，我的首版直接丢弃——
    // 对"忘了发换行"的客户端就是完全没有响应。这里钉住宽限行为。
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    srv.SetReadTimeoutMs(400);
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest& req) {
        cnk::IpcResponse r;
        r.id = req.id;
        r.ok = true;
        return r;
    }, err));
    const std::string name = cnk::PipeNameForCurrentUser();

    const ConnectResult c = ConnectToServerEx(name);
    REQUIRE(c.h != INVALID_HANDLE_VALUE);
    REQUIRE(WriteAll(c.h, R"({"id":5,"cmd":"ping"})"));  // 故意不发换行
    const std::string resp = ReadLine(c.h, 3000);
    CHECK(resp.find("\"id\":5") != std::string::npos);
    CHECK(resp.find("\"ok\":true") != std::string::npos);
    CloseHandle(c.h);
    srv.Stop();
}

TEST_CASE("IPC Stop 后端口关闭且不挂死") {
    CNK_REQUIRE_PIPE_ENV();
    cnk::PipeServer srv;
    std::string err;
    REQUIRE(srv.Start([](const cnk::IpcRequest&) { return cnk::IpcResponse{}; }, err));
    const DWORD t0 = GetTickCount();
    srv.Stop();
    CHECK(GetTickCount() - t0 < 3000);  // Stop 必须能及时返回
    CHECK(ConnectToServer(cnk::PipeNameForCurrentUser(), 300) == INVALID_HANDLE_VALUE);
}
