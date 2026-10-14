#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "core/Version.h"
#include "core/Cli.h"
#include "core/Encoding.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Probe.h"
#include "core/ProbeTarget.h"
#include "core/Scheduler.h"
#include "core/Url.h"
#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include "auth/Detector.h"
#include "store/Config.h"
#include "sim/SimServer.h"
#include "sys/SingleInstance.h"
#include "tray/TrayIcon.h"

namespace {

std::vector<std::string> WideArgsToUtf8() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) { out.push_back({}); continue; }
        // 缓冲区按含 NUL 的长度分配，转换后去掉 NUL——避免写入越界
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        s.resize(static_cast<size_t>(n - 1));
        out.push_back(std::move(s));
    }
    LocalFree(argv);
    return out;
}

void PrintLine(const std::string& s) {
    std::printf("%s\n", s.c_str());
}

int RunProbeNow(cnk::AppConfig& cfg) {
    PrintLine("== 探测 ==");
    PrintLine("探测地址: " + cfg.settings.probe_url);
    auto r = cnk::ProbeOnce(cfg.settings);
    const char* names[] = {"Unknown", "Online（网络正常）",
                           "PortalDetected（被 Portal 劫持）", "Offline（无连接）"};
    PrintLine(std::string("结果: ") + names[(int)r.state]);
    PrintLine("说明: " + r.message);
    if (r.response.status)
        PrintLine("HTTP 状态: " + std::to_string(r.response.status));
    if (!r.response.server.empty())
        PrintLine("Server: " + r.response.server);
    if (!r.response.location.empty())
        PrintLine("Location: " + r.response.location);
    if (!r.response.content_type.empty())
        PrintLine("Content-Type: " + r.response.content_type);
    return (r.state == cnk::NetState::Online) ? 0 : 1;
}

int RunDetect(cnk::AppConfig& cfg) {
    auto pr = cnk::ProbeOnce(cfg.settings);
    if (pr.response.status == 0) {
        PrintLine("探测失败：" + pr.message);
        return 1;
    }
    cnk::Vendor v = cnk::DetectVendor(pr.response);
    PrintLine("识别厂商: " + std::string(cnk::VendorName(v)));
    if (!pr.response.server.empty())
        PrintLine("Server: " + pr.response.server);

    std::string body = cnk::DecodeBody(pr.response);
    cnk::CarrierList cl;
    if (cnk::ParseCarrier(body, cl)) {
        PrintLine("运营商列表（服务器下发）:");
        for (const auto& o : cl.options)
            PrintLine("  [" + o.id + "] " + o.name +
                      (o.suffix.empty() ? "（无后缀）" : "  后缀=" + o.suffix) +
                      (o.id == cl.default_id ? "  <= 默认" : ""));
    } else {
        PrintLine("服务器未下发运营商列表");
    }

    std::vector<std::string> bl;
    if (cnk::ParseVisitBlacklist(body, bl)) {
        PrintLine("免劫持地址清单:");
        for (const auto& b : bl) PrintLine("  " + b);
        cnk::ProbeTarget t;
        if (cnk::SelectProbeTarget(bl, t))
            PrintLine("建议探测地址: " + t.url + "  （期望正文: " + t.expect + "）");
    } else {
        PrintLine("服务器未下发免劫持地址清单");
    }
    return 0;
}

int RunTestLogin(cnk::AppConfig& cfg) {
    const cnk::Profile* p = cnk::ActiveProfile(cfg);
    if (!p) {
        PrintLine("配置中没有账号档案，请先运行程序并用 --ui 打开设置填写");
        return 1;
    }

    PrintLine("== 探测 ==");
    auto pr = cnk::ProbeOnce(cfg.settings);
    PrintLine("状态: " + std::string(pr.state == cnk::NetState::Online ? "在线"
                       : pr.state == cnk::NetState::Offline ? "离线"
                       : pr.state == cnk::NetState::Unknown ? "未配置探测地址"
                                                            : "被劫持"));
    PrintLine("说明: " + pr.message);
    if (pr.state == cnk::NetState::Online) {
        PrintLine("当前已在线，无需登录。若要测试登录请先注销或等待会话失效。");
        return 0;
    }

    cnk::Vendor v = cnk::DetectVendor(pr.response);
    PrintLine(std::string("识别厂商: ") + cnk::VendorName(v));

    cnk::NetContext ctx;
    // 与调度器保持一致：portal_url 是 resolve 后的重定向目标（无则为空）
    ctx.portal_url = pr.response.location.empty()
        ? std::string{}
        : cnk::ResolveUrl(pr.response.location, cfg.settings.probe_url);
    ctx.portal_body = cnk::DecodeBody(pr.response);
    ctx.server_header = pr.response.server;

    auto auth = cnk::CreateAuth(v, *p);
    PrintLine(std::string("认证策略: ") + auth->Name());
    PrintLine("运营商后缀: " +
              (cnk::IspSuffix(*p).empty() ? std::string("无") : cnk::IspSuffix(*p)));

    auto lr = auth->Login(*p, ctx);

    PrintLine("");
    PrintLine("== 请求（已脱敏）==");
    PrintLine("POST " + cnk::RedactSecrets(lr.request_url));
    PrintLine(cnk::RedactSecrets(lr.request_body));
    PrintLine("");
    PrintLine("== 结果 ==");
    PrintLine(std::string("成功: ") + (lr.success ? "是" : "否"));
    PrintLine("信息: " + lr.message);

    if (lr.success) {
        PrintLine("");
        PrintLine("复测探测...");
        auto after = cnk::ProbeOnce(cfg.settings);
        PrintLine(std::string("复测结果: ") +
                  (after.state == cnk::NetState::Online ? "已在线 OK" : "仍不在线"));
    }
    return lr.success ? 0 : 2;
}

// 守护形态：若进程拥有独占控制台（双击 / 开机自启等独立启动方式），
// 隐藏它以避免黑窗常驻；从终端启动时控制台是共享的（不能隐藏，
// 否则会把用户自己的终端窗口一起隐藏）。
// 注意：曾尝试 WIN32 子系统 + AttachConsole 方案（完全无黑窗），
// 但在真实 cmd 环境下 attach 链路失效导致所有 CLI 输出丢失
//（2026-10-14 用户实测），已回退为控制台子系统 + 选择性隐藏。
void HideConsoleIfStandalone() {
    DWORD pids[2] = {0, 0};
    if (GetConsoleProcessList(pids, 2) == 1) {
        HWND wnd = GetConsoleWindow();
        if (wnd) ShowWindow(wnd, SW_HIDE);
    }
}

int RunDaemon(cnk::AppConfig& cfg, HINSTANCE hinst) {
    std::printf("CampusNet-Keeper 守护模式已启动（托盘驻留）。\n");
    std::printf("从终端启动时可按 Ctrl+C 退出；其它情况请使用托盘菜单退出。\n");
    std::fflush(stdout);
    HideConsoleIfStandalone();
    if (!cnk::RunDaemonLoop(hinst, cfg)) {
        std::printf("守护循环启动失败\n");
        return 1;
    }
    return 0;
}

// 交互式读取密码（不回显）。仅适用于真实控制台；非控制台环境返回空串，
// 由调用方提示改用环境变量 CNK_PASSWORD。
std::string ReadPasswordFromConsole() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!hIn || hIn == INVALID_HANDLE_VALUE || !GetConsoleMode(hIn, &mode)) {
        return {};
    }
    std::printf("请输入校园网密码（输入不回显，回车确认）：");
    std::fflush(stdout);
    SetConsoleMode(hIn, mode & ~ENABLE_ECHO_INPUT);

    std::string pw;
    for (;;) {
        wchar_t wc = 0;
        DWORD n = 0;
        if (!ReadConsoleW(hIn, &wc, 1, &n, nullptr) || n == 0) break;
        if (wc == L'\r' || wc == L'\n') break;
        if (wc == L'\b') { if (!pw.empty()) pw.pop_back(); continue; }
        char buf[8] = {0};
        int len = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf,
                                      (int)sizeof(buf), nullptr, nullptr);
        if (len > 0) pw.append(buf, (size_t)len);
    }
    SetConsoleMode(hIn, mode);
    FlushConsoleInputBuffer(hIn);
    std::printf("\n");
    return pw;
}

int RunSetPassword(cnk::AppConfig& cfg, const cnk::CliOptions& opts) {
    std::string password;
    // 环境变量优先：脚本化/自动化场景（也是失败提示里推荐的替代方式）
    const char* envpw = std::getenv("CNK_PASSWORD");
    if (envpw && *envpw) {
        password = envpw;
        std::printf("已从环境变量 CNK_PASSWORD 读取密码。\n");
    } else {
        password = ReadPasswordFromConsole();
        if (password.empty()) {
            std::printf("未能读取密码。请在终端窗口中交互输入，\n"
                        "或先设置环境变量（如 set CNK_PASSWORD=... ）后重试。\n");
            return 1;
        }
    }

    std::string err;
    if (!cnk::SetProfilePassword(cfg, opts.user, password, err)) {
        std::printf("设置密码失败：%s\n", err.c_str());
        return 1;
    }
    // 保存回配置的加载来源（与 wmain 的加载逻辑对称）：
    // 指定了 --config 就写该文件，否则写默认路径
    const bool saved = opts.config_path.empty()
        ? cnk::ConfigSave(cfg, err)
        : cnk::ConfigSaveTo(opts.config_path, cfg, err);
    if (!saved) {
        std::printf("保存配置失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("密码已用 Windows DPAPI 加密保存。\n");
    if (!opts.user.empty()) {
        std::printf("目标账号：%s\n", opts.user.c_str());
        std::printf("提示：还需在配置文件中确认 auth_url 与 isp.preset 指向你的学校。\n");
    }
    return 0;
}

// 模拟模式：启动本机模拟服务器，并把配置里的地址全部改指向它。
// 这些覆盖只作用于内存中的配置，绝不写回配置文件。
struct SimSession {
    cnk::SimServer server;
    bool active = false;
};

bool SetupSimulation(const cnk::CliOptions& opts, cnk::AppConfig& cfg,
                     SimSession& sim) {
    if (opts.simulate_vendor.empty()) return false;

    cnk::SimOptions so;
    so.vendor = opts.simulate_vendor;
    so.valid_user = opts.simulate_user;
    so.valid_pass = opts.simulate_pass;
    // 未显式提供模拟凭据时，优先沿用首份配置的账号密码，最后回落到内置默认——
    // 保证「配置为空 + 不给凭据」开箱可用，且用配置账号即可验证登录链路
    if (so.valid_user.empty() || so.valid_pass.empty()) {
        const cnk::Profile* p0 = cfg.profiles.empty() ? nullptr : &cfg.profiles.front();
        if (so.valid_user.empty())
            so.valid_user = (p0 && !p0->username.empty()) ? p0->username : "alice";
        if (so.valid_pass.empty())
            so.valid_pass = (p0 && !p0->password.empty()) ? p0->password : "pw123";
    }
    if (opts.simulate_fail == "badcred") so.fail_mode = 1;
    else if (opts.simulate_fail == "timeout") so.fail_mode = 2;

    std::string err;
    if (!sim.server.Start(so, err)) {
        std::printf("启动模拟服务器失败：%s\n", err.c_str());
        return false;
    }
    sim.active = true;

    // 探测指向 /sim_probe：登录前返回门户页（判劫持），登录成功后返回 nCSI 应答
    //——这正是「探测 → 劫持 → 登录 → 恢复」整链可端到端验证的关键
    cfg.settings.probe_url = sim.server.BaseUrl() + "/sim_probe";
    cfg.settings.probe_expect = "Microsoft NCSI";
    cfg.settings.probe_timeout_seconds = 2;
    for (auto& p : cfg.profiles) {
        p.auth_url = sim.server.BaseUrl() + "/";
        // 档案存在但凭据为空时（如拷贝来的配置）用模拟凭据填充内存副本，
        // 保证 --simulate 的登录链路可测；profiles 循环只改内存，不写盘
        if (p.username.empty()) p.username = so.valid_user;
        if (p.password.empty()) p.password = so.valid_pass;
        p.custom_template.url = sim.server.BaseUrl()
                                + "/eportal/?c=ACSetting&a=Login";
        p.custom_template.body = "DDDDD={username}&upass={password}";
        p.custom_template.success_regex = "Dr\\.COMWebLoginID_3";
    }
    if (cfg.profiles.empty()) {
        cnk::Profile p;
        p.name = "sim";
        p.auth_url = sim.server.BaseUrl() + "/";
        p.username = so.valid_user;
        p.password = so.valid_pass;
        p.vendor = (so.vendor == "eportal") ? "eportal" : "drcom";
        p.isp.preset = "campus";
        cfg.profiles.push_back(p);
        cfg.active_profile = "sim";
    }

    std::printf("== 模拟模式 ==\n");
    std::printf("厂商: %s\n", so.vendor.c_str());
    std::printf("地址: %s\n", sim.server.BaseUrl().c_str());
    if (!opts.simulate_fail.empty())
        std::printf("故障注入: %s\n", opts.simulate_fail.c_str());
    std::printf("\n");
    return true;
}

}  // namespace

int wmain(int, wchar_t**) {
    SetConsoleOutputCP(CP_UTF8);

    auto args = WideArgsToUtf8();
    auto opts = cnk::ParseCli(args);

    if (opts.mode == cnk::CliMode::Help) {
        std::printf("%s", cnk::CliHelpText().c_str());
        return 0;
    }
    if (opts.mode == cnk::CliMode::Version) {
        std::printf("CampusNet-Keeper %s\n", cnk::VersionString());
        return 0;
    }

    // 给了参数却一个都不认识且未显式指定任何模式：疑似拼写错误，
    // 绝不能静默落入守护模式（否则是"按了没反应"的脚枪）
    if (opts.mode == cnk::CliMode::Daemon && !args.empty() && !opts.unknown.empty()) {
        PrintLine("无法识别的参数：");
        for (const auto& u : opts.unknown) PrintLine("  " + u);
        PrintLine("运行 --help 查看用法。");
        return 64;  // 用法错误
    }

    std::string log_dir = cnk::AppDataSubDir("logs");
    if (log_dir.empty()) {
        // 此前该场景是静默的：日志系统不工作且无任何提示，事后难以诊断
        //（用户环境曾出现"配置保存失败且无日志"的偶发情况，与此吻合）。
        std::printf("[警告] 无法访问应用数据目录（%%APPDATA%%\\CampusNet-Keeper）。\n");
        std::printf("       配置保存与日志写入将不可用，请检查目录是否存在、权限是否正常，\n");
        std::printf("       或是否有安全软件阻止了本程序写入该目录。\n");
    } else {
        cnk::LogInit(log_dir, cnk::LogLevel::Info, 512 * 1024, 3);
    }
    cnk::LogInfo("启动，模式=" + std::to_string((int)opts.mode));

    cnk::AppConfig cfg;
    std::string err;
    if (!opts.config_path.empty())
        cnk::ConfigLoadFrom(opts.config_path, cfg, err);
    else
        cnk::ConfigLoad(cfg, err);

    if (!err.empty()) cnk::LogWarn("配置加载：" + err);

    SimSession sim;
    SetupSimulation(opts, cfg, sim);

    // 守护形态获取单实例；已有实例则请求它弹出设置界面后退出
    cnk::SingleInstance single;
    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (opts.mode == cnk::CliMode::Daemon && !single.Acquire()) {
        if (cnk::SingleInstance::NotifyExistingInstance())
            PrintLine("CampusNet-Keeper 已在运行，已请求其打开设置界面。");
        else
            PrintLine("CampusNet-Keeper 已在运行。");
        if (sim.active) sim.server.Stop();
        cnk::LogShutdown();
        return 0;
    }

    int rc = 0;
    switch (opts.mode) {
        case cnk::CliMode::ProbeNow:  rc = RunProbeNow(cfg); break;
        case cnk::CliMode::Detect:    rc = RunDetect(cfg); break;
        case cnk::CliMode::TestLogin: rc = RunTestLogin(cfg); break;
        case cnk::CliMode::SetPassword: rc = RunSetPassword(cfg, opts); break;
        case cnk::CliMode::Ui:
            // UI 形态在任务 23-26 实现；当前明确提示而不是静默进入守护循环
            PrintLine("设置界面将在后续版本提供（当前为 M1 命令行形态）。");
            PrintLine("可先用 --probe-now / --test-login / --detect 诊断，或直接运行守护。");
            rc = 0;
            break;
        case cnk::CliMode::Daemon:
        default:                      rc = RunDaemon(cfg, hinst); break;
    }

    if (sim.active) sim.server.Stop();
    cnk::LogShutdown();
    return rc;
}
