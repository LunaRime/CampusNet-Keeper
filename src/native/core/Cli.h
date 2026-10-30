#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class CliMode { Daemon, ProbeNow, TestLogin, Detect, SetPassword,
                     ExportSecret, ImportSecret, Logout, Version, Help, Ui, UiInfo };

struct CliOptions {
    CliMode mode = CliMode::Daemon;
    std::string config_path;
    std::string simulate_vendor;
    std::string simulate_user;
    std::string simulate_pass;
    std::string simulate_fail;   // "" | "badcred" | "timeout"
    std::string user;            // --set-password / --export-secret / --import-secret 的目标账号
    std::string secret_path;     // --export-secret / --import-secret 的文件路径
    // 开机自启启动（静默）：不弹设置界面。普通双击（无任何参数）会打开界面。
    bool autostart_launch = false;
    // 内部标志：本进程是"分离重启"后的无控制台实例（见 main.cpp 的 SpawnDetachedCopy），
    // 不会再次分离；--open-ui 表示父进程交代过"就绪后打开设置界面"（双击启动）。
    bool detached = false;
    bool open_ui = false;
    // 内部标志（测试/脚本用）：不把配置里的 autostart 同步到注册表，
    // 免得验证脚本起守护进程时改掉用户真实的开机自启项。
    bool no_autostart = false;
    std::vector<std::string> unknown;
};

CliOptions ParseCli(const std::vector<std::string>& args);
std::string CliHelpText();

}  // namespace cnk
