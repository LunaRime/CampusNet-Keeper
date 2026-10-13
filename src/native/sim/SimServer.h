#pragma once
#include <string>
#include <memory>

namespace cnk {

struct SimOptions {
    std::string vendor = "drcom";   // "drcom" | "eportal" | "srun" | "generic"
    std::string valid_user;
    std::string valid_pass;
    std::string isp_suffix;         // 期望的账号后缀，如 "@cmcc"
    int fail_mode = 0;              // 0=正常 1=凭据错误 2=连接挂起 3=302 重定向
                                    // 4=失败并回显含 authsuccess/authfail 双标志的登录页
    std::string redirect_location;  // fail_mode == 3 时返回的 Location 值
};

class SimServer {
public:
    SimServer();
    ~SimServer();
    SimServer(const SimServer&) = delete;
    SimServer& operator=(const SimServer&) = delete;

    bool Start(const SimOptions& opts, std::string& error);
    void Stop();

    int Port() const;
    std::string BaseUrl() const;
    std::string LoginPath() const;
    int LoginCount() const;
    std::string LastLoginBody() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
