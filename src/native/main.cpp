#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>
#include "core/Version.h"
#include "core/Cli.h"
#include "core/Encoding.h"
#include "core/Integrity.h"
#include "core/LastExit.h"
#include "core/LocalIp.h"
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
#include "ui_host/Hostfxr.h"
#include "ui_host/PayloadExtract.h"

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

bool FileExists(const std::string& utf8_path) {
    if (utf8_path.empty()) return false;
    const std::wstring w = cnk::Utf8ToWide(utf8_path);
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 控制台控制事件（Ctrl+C / 关终端 / 注销 / 关机）。此前这些路径**什么都不记**，
// 于是"守护进程怎么没的"永远查不出来。这里只留痕迹，处理仍交回默认行为（终止进程）。
BOOL WINAPI OnConsoleCtrl(DWORD type) {
    const char* name = "未知";
    switch (type) {
        case CTRL_C_EVENT:        name = "Ctrl+C"; break;
        case CTRL_BREAK_EVENT:    name = "Ctrl+Break"; break;
        case CTRL_CLOSE_EVENT:    name = "关闭控制台窗口"; break;
        case CTRL_LOGOFF_EVENT:   name = "用户注销"; break;
        case CTRL_SHUTDOWN_EVENT: name = "系统关机"; break;
        default: break;
    }
    // LogWrite 每行都 flush，所以这里写完就已落盘，随后被默认处理终止也不会丢
    cnk::LogInfo(std::string("收到控制台控制事件：") + name + "（进程即将结束）");
    cnk::RecordExit(std::string("控制台事件：") + name);
    return FALSE;   // FALSE = 继续默认处理（Ctrl+C/CLOSE 即终止进程）
}

// 守护形态**启动时**核对上一次运行的退出画像：
//   读到标记 → 上次是正常退出的，把来源与时间补进日志；
//   读不到 → 上次不是正常退出（被强制结束 / 崩溃 / 掉电），明确记一条警告，
//            否则"日志里什么都没有"会让人以为程序从没跑过。
// 只在**非首次运行**时报警（判据：keeper.log 是否已存在），免得第一次安装就误报。
void AuditLastExit(const std::string& log_dir) {
    const bool first_run = !FileExists(log_dir + "\\keeper.log");
    const cnk::LastExit le = cnk::TakeLastExit();
    if (le.recorded) {
        cnk::LogInfo("上次运行正常退出（来源=" + le.reason + "，于 " + le.at + "）");
    } else if (!first_run) {
        cnk::LogWarn("上次运行没有留下正常退出记录：可能在退出前被强制结束、异常终止或掉电"
                     "（日志里不会有痕迹，只能从这条推断）");
    } else {
        cnk::LogInfo("首次运行（无上次退出记录）");
    }
}

// ---------------------------------------------------------------- stdio 绑定

// 把 CRT 的某个标准流接到给定句柄上（句柄所有权转交给 CRT）。
bool BindStream(HANDLE h, int fd) {
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    const int os = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_TEXT);
    if (os == -1) return false;
    _dup2(os, fd);
    return true;
}

HANDLE OpenConsoleDevice(const wchar_t* name, DWORD access) {
    return CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

// GUI 子系统没有控制台，CLI 输出要靠这里显式接上（见 wWinMain 的注释）。
// 顺序很重要：先认继承来的句柄（管道/重定向），**只有在完全没有输出通道时**才 attach 父控制台。
//
// 为什么不能"stdin 没绑上就 attach"：AttachConsole 会把进程的标准句柄重置成新控制台的，
// PowerShell 只重定向 stdout/stderr、不重定向 stdin，于是刚绑好的管道被它冲掉，
// 表现就是"PowerShell 里捕获不到任何输出，bash 里却正常"（2026-10-27 实测踩到）。
void BindStandardStreams() {
    const bool out = BindStream(GetStdHandle(STD_OUTPUT_HANDLE), 1);
    const bool err = BindStream(GetStdHandle(STD_ERROR_HANDLE), 2);
    const bool in = BindStream(GetStdHandle(STD_INPUT_HANDLE), 0);

    // 已有输出通道（脚本管道 / 文件重定向）→ 保持原样，绝不能碰控制台
    if (out || err) return;

    // 完全没有输出通道：父进程若有控制台（cmd / PowerShell 直接运行），接上去，
    // 输出就落在用户眼前那个窗口里，Ctrl+C 也照常有效。
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    BindStream(OpenConsoleDevice(L"CONOUT$", GENERIC_READ | GENERIC_WRITE), 1);
    BindStream(OpenConsoleDevice(L"CONOUT$", GENERIC_READ | GENERIC_WRITE), 2);
    if (!in) BindStream(OpenConsoleDevice(L"CONIN$", GENERIC_READ | GENERIC_WRITE), 0);
}

// 网络状态的中文名（探测输出与日志共用）
const char* StateText(cnk::NetState s) {
    switch (s) {
        case cnk::NetState::Online: return "在线";
        case cnk::NetState::Offline: return "离线";
        case cnk::NetState::PortalDetected: return "被劫持";
        case cnk::NetState::Unknown: return "未配置探测地址";
    }
    return "未知";
}

// 登录成功后的复探参数：AC 数据面放行有秒级延迟（2026-10-16 真机实测：
// 紧跟登录的单次复探得「仍不在线」，稍后探测即 Online）
constexpr int kAfterLoginProbes = 5;
constexpr int kAfterLoginDelayMs = 1500;
// 决策前探测参数：单次探测偶发超时（错误码 12002）会得「离线」，
// 而离线会让整条链路走成「厂商 unknown → GenericFormAuth 尚未实现」的死路
constexpr int kDecideProbes = 3;
constexpr int kDecideDelayMs = 1500;

int RunProbeNow(cnk::AppConfig& cfg) {
    PrintLine("== 探测 ==");
    PrintLine("探测地址: " + cfg.settings.probe_url);
    // 先报「本机到门户是什么样子」：没有地址 / 链路本地 / 有地址但网关不通，
    // 这三种在"连不上"时结论完全不同，必须一眼看见（2026-10-26 事故的教训）
    {
        const cnk::Profile* active = cnk::ActiveProfile(cfg);
        const std::string portal_url =
            (active && !active->auth_url.empty()) ? active->auth_url : cfg.settings.probe_url;
        PrintLine("本机: " + cnk::DescribeRoute(cnk::RouteForUrl(portal_url)) +
                  "（目标 " + portal_url + "）");
    }
    // 离线可能是单次超时（实测），故策略定为「确定结论即停、离线才重试」
    int tries = 0;
    auto r = cnk::ProbeUntilDecided(
        [&cfg, &tries] { ++tries; return cnk::ProbeOnce(cfg.settings); }, kDecideProbes,
        kDecideDelayMs);
    const char* names[] = {"Unknown", "Online（网络正常）",
                           "PortalDetected（被 Portal 劫持）", "Offline（无连接）"};
    PrintLine(std::string("结果: ") + names[(int)r.state]);
    PrintLine("说明: " + r.message);
    if (tries > 1)
        PrintLine("备注: 首次判定为离线，第 " + std::to_string(tries) + " 次探测才得到上述结果");
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
    // 决策前探测：离线可能只是一次超时，若据此下结论，整条链路会走成
    // 「厂商 unknown → GenericFormAuth 尚未实现」的死路（2026-10-16 真机实测）
    int probe_tries = 0;
    auto pr = cnk::ProbeUntilDecided(
        [&cfg, &probe_tries] { ++probe_tries; return cnk::ProbeOnce(cfg.settings); },
        kDecideProbes, kDecideDelayMs);
    PrintLine(std::string("状态: ") + StateText(pr.state));
    if (probe_tries > 1)
        PrintLine("（首次探测为离线，第 " + std::to_string(probe_tries) + " 次才得到上述状态）");
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

    // 与调度器保持一致：配置默认 vendor="auto"，而 IspSuffix 按**档案里的** vendor 分支
    //（auto 被当作 Dr.COM → @dx/@cmcc/@lt）。把「按识别结果生效的厂商」写进档案副本，
    // 否则锐捷/深澜用户会带着 Dr.COM 后缀去认证而报「账号或密码错误」。
    cnk::Profile effective = *p;
    if (effective.vendor == "auto" || effective.vendor.empty())
        effective.vendor = cnk::VendorName(v);

    cnk::NetContext ctx;
    // 与调度器保持一致：portal_url 是 resolve 后的重定向目标（无则为空）
    ctx.portal_url = pp.portal_url;
    ctx.portal_body = cnk::DecodeBody(pp.response);
    ctx.server_header = pp.response.server;

    auto auth = cnk::CreateAuth(v, effective);
    PrintLine(std::string("认证策略: ") + auth->Name());
    PrintLine("运营商后缀: " + (cnk::IspSuffix(effective).empty()
                                   ? std::string("无")
                                   : cnk::IspSuffix(effective)));

    auto lr = auth->Login(effective, ctx);

    PrintLine("");
    PrintLine("== 请求（已脱敏）==");
    PrintLine(cnk::RedactSecrets(lr.request_url));
    PrintLine(cnk::RedactSecrets(lr.request_body));
    PrintLine("");
    PrintLine("== 结果 ==");
    PrintLine(std::string("成功: ") + (lr.success ? "是" : "否"));
    PrintLine("信息: " + lr.message);
    // 把结果也写进日志（含时间戳），便于事后核对「登录成功 → 何时真正放行」
    cnk::LogInfo(std::string("--test-login 结果：") + (lr.success ? "成功" : "失败") +
                 "，信息=" + cnk::RedactSecrets(lr.message));

    if (lr.success) {
        PrintLine("");
        PrintLine("复测探测...");
        // 登录成功后 AC 数据面放行常有秒级延迟：单次复探会误报「仍不在线」
        //（2026-10-16 真机实测），故轮询若干次，并如实报告用了几次、最后一态为何。
        int tries = 0;
        auto after = cnk::ProbeUntilOnline(
            [&cfg, &tries] { ++tries; return cnk::ProbeOnce(cfg.settings); },
            kAfterLoginProbes, kAfterLoginDelayMs);
        if (after.state == cnk::NetState::Online) {
            PrintLine("复测结果: 已在线 OK（第 " + std::to_string(tries) + " 次探测）");
            cnk::LogInfo("复测结果：已在线（第 " + std::to_string(tries) + " 次探测）");
        } else {
            PrintLine(std::string("复测结果: 仍不在线（") + StateText(after.state) +
                      "，共探测 " + std::to_string(tries) + " 次）");
            PrintLine("说明: " + after.message);
            cnk::LogWarn(std::string("复测结果：仍不在线（") + StateText(after.state) +
                         "，共探测 " + std::to_string(tries) + " 次）");
        }
    }
    return lr.success ? 0 : 2;
}

// 守护形态：**兜底**隐藏控制台（正常路径已在 wmain 里分离重启，见 SpawnDetachedCopy）。
// 注意：Win11 默认终端是 Windows Terminal 时，可见窗口属 WindowsTerminal.exe，
// GetConsoleWindow() 拿不到它 → 这里藏不住；所以真正的解法是"无控制台重启自身"。
// 另外：曾尝试 WIN32 子系统 + AttachConsole 方案（完全无黑窗），但在真实 cmd 环境下
// attach 链路失效导致所有 CLI 输出丢失（2026-10-14 用户实测），已回退。
void HideConsoleIfStandalone() {
    DWORD pids[2] = {0, 0};
    if (GetConsoleProcessList(pids, 2) == 1) {
        HWND wnd = GetConsoleWindow();
        if (wnd) ShowWindow(wnd, SW_HIDE);
    }
}

// 是否"独占"一个控制台：双击/开机自启时新开的控制台里只有我们自己；
// 从 cmd/pwsh 启动时 shell 与我们共享控制台 → 不能分离（用户要看输出、也要能 Ctrl+C）。
bool OwnsConsole() {
    DWORD pids[2] = {0, 0};
    return GetConsoleProcessList(pids, 2) == 1;
}

// 把参数列表拼成命令行（每个参数加引号；本项目的参数不含引号字符）
std::wstring BuildDetachedCommandLine(const std::vector<std::string>& args, bool open_ui) {
    const std::string exe = cnk::ExecutablePath();
    if (exe.empty()) return {};
    std::wstring cmd = L"\"" + cnk::Utf8ToWide(exe) + L"\"";
    for (const auto& a : args) {
        cmd += L" \"";
        cmd += cnk::Utf8ToWide(a);
        cmd += L"\"";
    }
    cmd += L" --detached";
    if (open_ui) cmd += L" --open-ui";
    return cmd;
}

// 以无控制台方式重新启动自身（成功则调用方应立即退出）。
// 为什么是"重启"而不是"隐藏窗口"：双击/自启时 Win11 的默认终端是 Windows Terminal，
// 可见窗口属于 WindowsTerminal.exe，GetConsoleWindow() 拿到的是伪控制台窗口 → 藏不住；
// 而用户关掉那个终端窗口时，Windows 会给控制台进程发 CTRL_CLOSE_EVENT，守护进程跟着被杀。
// CREATE_NO_WINDOW 启动的进程没有任何可见终端，也就不会因"关终端"而死。
bool SpawnDetachedCopy(const std::vector<std::string>& args, bool open_ui) {
    const std::wstring cmd = BuildDetachedCommandLine(args, open_ui);
    if (cmd.empty()) return false;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// open_ui_on_start：用户**双击 exe**（无参数）启动时，守护起来后自动打开设置界面。
// 否则双击后只有托盘图标（还可能被折叠进溢出区），用户会以为"什么都没发生"。
int RunDaemon(cnk::AppConfig& cfg, HINSTANCE hinst, bool open_ui_on_start) {
    std::printf("CampusNet-Keeper 守护模式已启动（托盘驻留）。\n");
    std::printf("从终端启动时可按 Ctrl+C 退出；其它情况请使用托盘菜单退出。\n");
    std::fflush(stdout);
    HideConsoleIfStandalone();
    if (open_ui_on_start) cnk::LogInfo("启动后自动打开设置界面（双击意图）");
    if (!cnk::RunDaemonLoop(hinst, cfg, open_ui_on_start)) {
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

// 任务 26：设置界面载荷自检（--ui-info）。不弹窗、不释放载荷，给构建脚本与排障用。
// 退出码：0=就绪；3=精简版（exe 里没有界面载荷）；4=有载荷但缺 .NET 运行时；1=载荷坏了。
int RunUiInfo() {
    const cnk::UiPayloadStatus st = cnk::InspectUiPayload();
    PrintLine("== 设置界面载荷自检 ==");
    if (!st.present) {
        PrintLine("载荷: 不存在（精简单文件版：exe 里没有内嵌设置界面）");
        PrintLine("构建完整版：pwsh -File build.ps1 -SingleExe");
        PrintLine("说明: 守护与命令行功能不受影响。");
        return 3;
    }
    PrintLine("载荷: 存在（" + std::to_string(st.payload_bytes) + " 字节）");
    PrintLine("版本戳: " + st.payload_id);
    PrintLine("解压目录: " +
              (st.ui_dir.empty() ? std::string("（无法确定 %LOCALAPPDATA%）") : st.ui_dir));
    if (st.up_to_date)
        PrintLine("状态: 已释放且与当前载荷一致（打开界面时直接复用）");
    else if (st.extracted)
        PrintLine("状态: 目录里是旧版本的界面（下次打开界面会重新解压）");
    else
        PrintLine("状态: 尚未释放（首次打开界面时解压到上述目录）");

    std::string load_err;
    const cnk::EmbeddedPayload payload = cnk::LoadEmbeddedPayload(load_err);

    // 干跑校验：结构 + 本地头 + 数据偏移 + CRC 逐条验完（不落盘）。
    // 只看"能列出条目"是不够的——那是 build.ps1 唯一的形态断言（2026-10-17 审阅 P2-2）。
    std::string zip_err;
    if (!cnk::VerifyStoredZip(payload.data, payload.size, zip_err)) {
        PrintLine("载荷校验: 失败 —— " + zip_err);
        return 1;
    }

    std::vector<cnk::ZipEntryInfo> entries;
    if (!cnk::ListStoredZip(payload.data, payload.size, entries, zip_err)) {
        PrintLine("载荷解析: 失败 —— " + zip_err);
        return 1;
    }
    size_t compressed = 0;
    unsigned long long total = 0;
    bool has_dll = false;
    bool has_config = false;
    for (const auto& e : entries) {
        if (!e.stored) ++compressed;
        total += e.size;
        if (e.name == "CampusNet.UI.dll") has_dll = true;
        if (e.name == "CampusNet.UI.runtimeconfig.json") has_config = true;
    }
    PrintLine("载荷条目: " + std::to_string(entries.size()) + " 个，解压后约 " +
              std::to_string(total / (1024 * 1024)) + " MB" +
              (compressed ? "（含 " + std::to_string(compressed) +
                                " 个压缩条目 → 会被拒绝！）"
                          : "，全部为存储型"));
    if (compressed) return 1;
    if (!has_dll || !has_config) {
        PrintLine("载荷内容: 缺少 CampusNet.UI.dll 或 CampusNet.UI.runtimeconfig.json"
                  "（不是界面发布产物？）");
        return 1;
    }
    // 放在最后打印：前面几步都过了才叫"校验通过"（复核提示的次序问题）
    PrintLine("载荷校验: 通过（结构/偏移/CRC 全部通过）");

    if (st.hostfxr_found) {
        PrintLine("hostfxr: " + st.hostfxr_path);
    } else {
        PrintLine("hostfxr: 未找到 —— " + st.hostfxr_error);
        PrintLine("提示: 安装 .NET 10 运行时（Runtime 或 Desktop Runtime 均可）后重试："
                  "https://dotnet.microsoft.com/download/dotnet/10.0");
        PrintLine("说明: 守护与命令行功能不受影响，只有界面需要 .NET 运行时。");
        return 4;
    }
    PrintLine("结论: 就绪（打开设置界面会释放/复用载荷并加载 .NET 运行时）");
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

    // 模拟模式先把各档案的厂商固定为「所模拟的厂商」（只改内存副本，不写盘）：
    //   ① 让 --simulate=<厂商> 的预期策略确定，不受门户页指纹识别结果影响；
    //   ② 必须在推导 isp_suffix **之前**——IspSuffix 按厂商分支
    //      （drcom: @dx / srun: @telecom / eportal: telecom），顺序反了会按错的后缀校验。
    for (auto& p : cfg.profiles) p.vendor = opts.simulate_vendor;

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
        p.vendor = so.vendor;
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

// 入口主体：与子系统无关（GUI 子系统下由 wWinMain 调用，先完成 stdio 绑定）。
int WinMainEntry() {
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

    // 双击 exe / 开机自启：本进程独占控制台（Win11 下是一个 Windows Terminal 窗口）。
    // 控制台子系统程序会把终端窗口留在任务栏，且**用户关掉终端就会杀掉守护进程**。
    // 这里把自己以 CREATE_NO_WINDOW 分离重启，父进程立即退出（终端标签随之关闭）：
    // 用户看不到终端，关掉任何窗口都不影响守护进程。从 cmd/pwsh 启动时控制台是共享的，
    // 保持原样（用户要看输出、也能用 Ctrl+C 退出）。
    // --detached 的实例不再走这里（避免无限重启）。
    //
    // 2026-10-27：主程序改成 **GUI 子系统**（见 wWinMain）后，双击/自启时压根没有控制台
    // （GetConsoleProcessList 返回 0），从 cmd/pwsh 启动则是 attach 到共享控制台（≥2），
    // 所以这段在正常路径下**不会触发**，只作为"万一拿到了独占控制台"的兜底保留。
    if (opts.mode == cnk::CliMode::Daemon && !opts.detached && OwnsConsole()) {
        const bool want_ui = args.empty() && !opts.autostart_launch;
        if (SpawnDetachedCopy(args, want_ui)) {
            cnk::LogInfo("检测到独立控制台：已分离到无控制台进程（不再留下终端窗口）");
            cnk::LogShutdown();
            return 0;
        }
        cnk::LogWarn("分离到无控制台进程失败，退化为隐藏控制台窗口");
    }

    // 守护形态：① 给控制台控制事件留痕（Ctrl+C / 关终端 / 注销 / 关机此前全是静默的）；
    // ② 核对上一次运行的退出画像。
    // ⚠ 顺序必须在"分离重启"**之后**：只有真正跑守护循环的那个进程才有权取走退出标记，
    //   否则父进程取走后子进程一启动就会误报"上次未正常退出"。
    if (opts.mode == cnk::CliMode::Daemon) {
        SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
        AuditLastExit(log_dir);
    }

    // 任务 26：UI 形态与载荷自检都不需要配置、模拟会话与单实例，提前分流。
    //（界面自己通过 IPC 从守护进程取配置；也绝不能去抢守护进程的单实例。）
    if (opts.mode == cnk::CliMode::Ui || opts.mode == cnk::CliMode::UiInfo) {
        int ui_rc = 1;
        // 兜底：RunUiMode 内部已有 catch，这里再兜一层，保证任何异常都不会变成
        // "命令行静默消失"（2026-10-17 审阅 P1 的同类要求）
        try {
            ui_rc = (opts.mode == cnk::CliMode::Ui) ? cnk::RunUiMode() : RunUiInfo();
        } catch (const std::exception& e) {
            PrintLine(std::string("内部错误：") + e.what());
            cnk::LogError(std::string("UI 分流未捕获异常：") + e.what());
        } catch (...) {
            PrintLine("内部错误：未知异常（详见日志）");
            cnk::LogError("UI 分流未捕获异常（未知类型）");
        }
        cnk::LogShutdown();
        return ui_rc;
    }

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

    // 开机自启：配置里的 settings.autostart 是唯一真源，启动时把注册表同步成期望状态。
    // （此前 SetAutostart 没有任何调用方 —— 界面里"开机自启"只写进 config.json，
    //   注册表 Run 键从未被写入，所以重启后不会自启。2026-10-18 用户反馈。）
    // --no-autostart 供验证脚本使用，避免脚本起守护进程时改掉用户真实的自启项。
    if (opts.mode == cnk::CliMode::Daemon && !opts.no_autostart) {
        std::string aerr;
        if (!cnk::SyncAutostart(cfg.settings.autostart, aerr)) {
            cnk::LogWarn("同步开机自启失败：" + aerr);
        } else {
            cnk::LogInfo(std::string("开机自启：") +
                         (cfg.settings.autostart ? "已登记（HKCU Run 键）" : "已关闭（已移除 Run 键）"));
        }
    }

    // 双击启动 = 一个参数都没有（不是 --autostart、不是任何显式开关）→ 守护起来后自动开界面。
    // 分离重启后的子进程没有参数里的"双击"信息，由父进程用 --open-ui 交代。
    const bool open_ui_on_start = opts.open_ui || (args.empty() && !opts.autostart_launch);

    int rc = 0;
    switch (opts.mode) {
        case cnk::CliMode::ProbeNow:  rc = RunProbeNow(cfg); break;
        case cnk::CliMode::Detect:    rc = RunDetect(cfg); break;
        case cnk::CliMode::TestLogin: rc = RunTestLogin(cfg); break;
        case cnk::CliMode::SetPassword: rc = RunSetPassword(cfg, opts); break;
        case cnk::CliMode::Daemon:
        default:                      rc = RunDaemon(cfg, hinst, open_ui_on_start); break;
    }

    if (sim.active) sim.server.Stop();
    cnk::LogShutdown();
    return rc;
}

// ------------------------------------------------------------------ 入口

// GUI 子系统的入口。**先绑 stdio 再做任何事**——否则 CLI 模式下 printf 会落进虚空。
// 三种启动场景都靠它统一：
//   ① 从 cmd / PowerShell 直接运行（继承的句柄是控制台）→ 输出进同一个窗口，Ctrl+C 可用；
//   ② 被脚本捕获（管道 / 文件重定向）→ 直接绑继承来的句柄，不能去 attach 控制台，
//      否则会把重定向冲掉（build.ps1 靠管道解析 --version / --ui-info）；
//   ③ 双击 / 开机自启（没有任何句柄，也没有父控制台）→ 保持无控制台，不弹窗、不闪窗。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    BindStandardStreams();
    SetConsoleOutputCP(CP_UTF8);  // 有控制台时统一 UTF-8；无控制台则是无害的空操作
    return WinMainEntry();
}
