#pragma once
#include <string>
#include <vector>

namespace cnk {

enum class CliMode { Daemon, ProbeNow, TestLogin, Detect, Version, Help, Ui };

struct CliOptions {
    CliMode mode = CliMode::Daemon;
    std::string config_path;
    std::string simulate_vendor;
    std::string simulate_user;
    std::string simulate_pass;
    std::string simulate_fail;   // "" | "badcred" | "timeout"
    std::vector<std::string> unknown;
};

CliOptions ParseCli(const std::vector<std::string>& args);
std::string CliHelpText();

}  // namespace cnk
