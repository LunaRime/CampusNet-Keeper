#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <chrono>
#include <thread>
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

int RunDaemon(cnk::AppConfig& cfg) {
    cnk::Scheduler sch(cfg);
    PrintLine("CampusNet-Keeper 守护模式已启动，按 Ctrl+C 退出。");
    PrintLine("（任务 16-17 会替换为托盘驻留；当前为控制台轮询）");
    for (;;) {
        sch.Tick();
        std::printf("\r状态: %-16s %s          ",
                    cnk::RunStateName(sch.State()), sch.LastMessage().c_str());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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

    std::string log_dir = cnk::AppDataSubDir("logs");
    if (!log_dir.empty())
        cnk::LogInit(log_dir, cnk::LogLevel::Info, 512 * 1024, 3);
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

    int rc = 0;
    switch (opts.mode) {
        case cnk::CliMode::ProbeNow:  rc = RunProbeNow(cfg); break;
        case cnk::CliMode::Detect:    rc = RunDetect(cfg); break;
        case cnk::CliMode::TestLogin: rc = RunTestLogin(cfg); break;
        case cnk::CliMode::Daemon:
        case cnk::CliMode::Ui:        // UI 形态在任务 23-26 实现，当前回落到守护
        default:                      rc = RunDaemon(cfg); break;
    }

    if (sim.active) sim.server.Stop();
    cnk::LogShutdown();
    return rc;
}
