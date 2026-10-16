#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "core/Version.h"
#include "core/Cli.h"
#include "core/Encoding.h"
#include "core/Integrity.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Probe.h"
#include "core/ProbeTarget.h"
#include "core/PortalPage.h"
#include "core/Scheduler.h"
#include "auth/AuthFactory.h"
#include "auth/Carrier.h"
#include "auth/Detector.h"
#include "store/Config.h"
#include "sim/SimServer.h"
#include "sys/SingleInstance.h"
#include "sys/Autostart.h"
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

    // spec §7：识别的输入是 Portal 的响应头与页面 HTML。
    //  - 劫持态：抓取重定向目标页（302 本身可能无指纹，实测 2026-10-16）；
    //  - 其余状态（含在线）：尝试从配置的 auth_url 抓取门户页；
    //  - 抓不到时退回探测响应（可能识别不出，但流程不中断）。
    cnk::HttpResponse page = pr.response;
    if (pr.state == cnk::NetState::PortalDetected) {
        auto pp = cnk::ResolvePortalPage(pr, cfg.settings.probe_url);
        page = std::move(pp.response);
        if (pp.fetched)
            PrintLine("识别来源: " + pp.portal_url + "（已抓取门户页面）");
        else if (!pp.fetch_error.empty())
            PrintLine("识别来源: 探测响应（门户页抓取失败：" +
                      cnk::RedactSecrets(pp.fetch_error) + "）");
    } else {
        const cnk::Profile* p = cnk::ActiveProfile(cfg);
        if (p && !p->auth_url.empty()) {
            cnk::HttpResponse fetched;
            std::string ferr;
            if (cnk::FetchPortalPage(p->auth_url, fetched, ferr)) {
                page = std::move(fetched);
                PrintLine("识别来源: " + p->auth_url + "（从认证网址抓取）");
            } else {
                PrintLine("识别来源: 探测响应（认证网址抓取失败：" +
                          cnk::RedactSecrets(ferr) + "）");
            }
        }
    }

    cnk::Vendor v = cnk::DetectVendor(page);
    PrintLine("识别厂商: " + std::string(cnk::VendorName(v)));
    if (!page.server.empty())
        PrintLine("Server: " + page.server);

    std::string body = cnk::DecodeBody(page);
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

    // spec §7：识别的输入是 Portal 的响应头与页面 HTML。真实劫持 302 常不含
    // 指纹（本机实测 2026-10-16），先抓取 Location 目标页再识别；抓取失败
    // 退回探测响应（不阻断链路）。
    auto pp = cnk::ResolvePortalPage(pr, cfg.settings.probe_url);
    if (pp.fetched)
        PrintLine("门户页面: " + pp.portal_url + "（已抓取，识别基于页面响应）");
    else if (!pp.fetch_error.empty())
        PrintLine("门户页面: 抓取失败（" + cnk::RedactSecrets(pp.fetch_error) +
                  "），识别基于探测响应");

    cnk::Vendor v = cnk::DetectVendor(pp.response);
    PrintLine(std::string("识别厂商: ") + cnk::VendorName(v));
    if (!pp.response.server.empty())
        PrintLine("Server: " + pp.response.server);

    cnk::NetContext ctx;
    // 与调度器保持一致：portal_url 是 resolve 后的重定向目标（无则为空）
    ctx.portal_url = pp.portal_url;
    ctx.portal_body = cnk::DecodeBody(pp.response);
    ctx.server_header = pp.response.server;

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

// 低完整性导致写入被拒时的指引。低完整性有两种来源，处置方式相反，必须分开：
//   A) 可执行文件自身带低完整性标签——Windows 取「min(父进程令牌, 可执行文件
//      标签)」作为新进程完整性，因此从任何终端启动都是低完整性，换终端无效；
//   B) 启动链（终端/启动器/沙盒）本身是低完整性——换常规终端即可。
// 给错分支会让人白折腾（曾把 A 当成 B，指引"改用常规 cmd"，在 A 下永远不生效）。
void PrintLowIntegrityHint() {
    const std::string exe = cnk::ExecutablePath();
    const long long rid = cnk::FileIntegrityRid(exe);
    std::printf("\n[提示] 进程完整性=低：低完整性进程对 %%APPDATA%%、%%TEMP%% 等常规目录的\n");
    std::printf("       一切写入都会被 Windows 的 No-Write-Up 规则拒绝，与文件权限无关。\n");
    if (rid >= 0 && rid < SECURITY_MANDATORY_MEDIUM_RID) {
        std::printf("       原因：本程序的可执行文件自身带「低」完整性标签，"
                    "Windows 取 min(父进程, 可执行\n");
        std::printf("       文件标签) 作为新进程完整性，所以从任何终端启动都是低完整性——"
                    "换常规 cmd 无效。\n");
        std::printf("       诊断：icacls \"%s\"\n", exe.c_str());
        std::printf("       修复：icacls \"<上面路径所在目录>\" /setintegritylevel medium /t\n");
        std::printf("             或把本程序复制到其它目录（如 %%LOCALAPPDATA%%\\CampusNet-Keeper）\n");
        std::printf("             运行：副本不继承原标签。\n");
    } else if (rid == -1) {
        std::printf("       本程序文件没有低完整性标签，说明是启动它的终端/启动器处于受限\n");
        std::printf("       （沙盒/隔离）环境。请改用常规终端重试：开始菜单 → 搜索 cmd → 打开，\n");
        std::printf("       然后执行：\"%s\" --set-password --user=你的账号\n", exe.c_str());
    } else {
        std::printf("       未能读到可执行文件的完整性标签，请依次排查：\n");
        std::printf("       1) icacls \"%s\" 是否显示 Low Mandatory Level；\n", exe.c_str());
        std::printf("          是则：icacls \"<其所在目录>\" /setintegritylevel medium /t\n");
        std::printf("       2) 否则从开始菜单新开 cmd 重试（脱离受限启动链）。\n");
    }
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
        if (err.find("完整性=低") != std::string::npos) PrintLowIntegrityHint();
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

    // 模拟器的账号校验必须与客户端实际提交的账号一致：客户端按**生效档案**
    // 拼运营商后缀（如 @cmcc），若不设置 isp_suffix，真实配置（preset=cmcc）
    // 下会把正确凭据误判为错误（审阅 P1-1）。
    const cnk::Profile* ap = cnk::ActiveProfile(cfg);

    cnk::SimOptions so;
    so.vendor = opts.simulate_vendor;
    so.valid_user = opts.simulate_user;
    so.valid_pass = opts.simulate_pass;
    if (ap) so.isp_suffix = cnk::IspSuffix(*ap);
    // 未显式提供模拟凭据时，优先沿用生效档案的账号密码，最后回落到内置默认——
    // 保证「配置为空 + 不给凭据」开箱可用，且用配置账号即可验证登录链路
    if (so.valid_user.empty() || so.valid_pass.empty()) {
        if (so.valid_user.empty())
            so.valid_user = (ap && !ap->username.empty()) ? ap->username : "alice";
        if (so.valid_pass.empty())
            so.valid_pass = (ap && !ap->password.empty()) ? ap->password : "pw123";
    }
    if (opts.simulate_fail == "badcred") so.fail_mode = 1;
    else if (opts.simulate_fail == "timeout") so.fail_mode = 2;
    else if (opts.simulate_fail == "hijack302") so.fail_mode = 5;

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
