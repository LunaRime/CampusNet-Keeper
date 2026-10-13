#pragma once
#include <string>
#include "store/Config.h"

namespace cnk {

struct NetContext {
    std::string local_ip;
    std::string portal_url;     // 探测阶段发现的重定向目标
    std::string portal_body;    // 已转 UTF-8 的页面文本
    std::string server_header;
};

struct LoginResult {
    bool success = false;
    std::string message;
    int vendor_code = -1;
    std::string request_url;    // 供诊断输出（未脱敏原文，调用方负责脱敏）
    std::string request_body;
};

class IAuth {
public:
    virtual ~IAuth() = default;
    virtual LoginResult Login(const Profile& profile, const NetContext& ctx) = 0;
    virtual const char* Name() const = 0;
};

}  // namespace cnk
