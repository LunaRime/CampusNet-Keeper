#include "core/Cli.h"

namespace cnk {

namespace {

// 取 "prefix=value" 或 "prefix value" 两种写法的值。
// 返回 true 表示本轮已消费该参数（无论是否取到值）。
// 注意：末尾的 prefix 无值时安全忽略（不得对字符串越界 substr 取子串）。
bool TakePrefixed(const std::vector<std::string>& a, size_t& i,
                  const std::string& prefix, std::string& out) {
    const std::string& cur = a[i];
    if (cur == prefix) {
        if (i + 1 < a.size()) out = a[++i];
        return true;
    }
    if (cur.rfind(prefix + "=", 0) == 0) {
        out = cur.substr(prefix.size() + 1);
        return true;
    }
    return false;
}

}  // namespace

CliOptions ParseCli(const std::vector<std::string>& args) {
    CliOptions o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];

        if (a == "--probe-now")      { o.mode = CliMode::ProbeNow; continue; }
        if (a == "--test-login")     { o.mode = CliMode::TestLogin; continue; }
        if (a == "--detect")         { o.mode = CliMode::Detect; continue; }
        if (a == "--version" || a == "-v") { o.mode = CliMode::Version; continue; }
        if (a == "--help" || a == "-h" || a == "/?") {
            o.mode = CliMode::Help; continue;
        }
        if (a == "--ui")             { o.mode = CliMode::Ui; continue; }
        if (a == "--daemon")         { o.mode = CliMode::Daemon; continue; }

        // 带值的参数：--simulate-user/pass/fail 必须排在 --simulate 之前，
        // 否则会被 --simulate 的 "=" 前缀逻辑抢先匹配成厂商名
        if (TakePrefixed(args, i, "--simulate-user", o.simulate_user)) continue;
        if (TakePrefixed(args, i, "--simulate-pass", o.simulate_pass)) continue;
        if (TakePrefixed(args, i, "--simulate-fail", o.simulate_fail)) continue;
        if (TakePrefixed(args, i, "--simulate", o.simulate_vendor)) continue;
        if (TakePrefixed(args, i, "--config", o.config_path)) continue;

        o.unknown.push_back(a);
    }
    return o;
}

std::string CliHelpText() {
    return
        "CampusNet-Keeper —— 校园网自动重连守护\n"
        "\n"
        "用法：CampusNet-Keeper [选项]\n"
        "\n"
        "  （无选项）            以守护模式运行，常驻托盘\n"
        "  --ui                  打开设置界面\n"
        "  --probe-now           执行一次网络探测并输出结果，然后退出\n"
        "  --test-login          执行一次探测 + 登录尝试，输出完整请求（已脱敏）\n"
        "  --detect              只做厂商识别并输出结果\n"
        "  --config=<路径>       使用指定的配置文件\n"
        "  --simulate=<厂商>     使用本地模拟 Portal 服务器（测试用）\n"
        "                        drcom 为完整实现；其余厂商待后续任务\n"
        "  --simulate-user=<账号>  模拟服务器的有效账号（配合 --simulate）\n"
        "  --simulate-pass=<密码>  模拟服务器的有效密码（配合 --simulate）\n"
        "  --simulate-fail=<模式>  故障注入：badcred（凭据错误）| timeout（挂起）\n"
        "  --version             显示版本号\n"
        "  --help                显示本帮助\n"
        "\n"
        "配置文件默认位于 %APPDATA%\\CampusNet-Keeper\\config.json\n"
        "日志位于 %APPDATA%\\CampusNet-Keeper\\logs\\keeper.log\n";
}

}  // namespace cnk
