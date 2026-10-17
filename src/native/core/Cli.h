#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class CliMode { Daemon, ProbeNow, TestLogin, Detect, SetPassword, Version, Help, Ui, UiInfo };

struct CliOptions {
    CliMode mode = CliMode::Daemon;
    std::string config_path;
    std::string simulate_vendor;
    std::string simulate_user;
    std::string simulate_pass;
    std::string simulate_fail;   // "" | "badcred" | "timeout"
    std::string user;            // --set-password 的目标账号
    // 开机自启启动（静默）：不弹设置界面。普通双击（无任何参数）会打开界面。
    bool autostart_launch = false;
    std::vector<std::string> unknown;
};

CliOptions ParseCli(const std::vector<std::string>& args);
std::string CliHelpText();

}  // namespace cnk
