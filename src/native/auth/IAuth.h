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
    // 「终端 IP 已在线」型成功：服务器说在线，但**没有真正完成一次认证**。
    // 多半是好事（会话还在、链路本来通），也可能是**僵尸会话**：AC 数据库里记着
    // 「这个 IP 已认证」，转发面却已经废了 —— 此时只重试永远好不了，必须先注销。
    // 故调用方必须复测一次才能决定，不能直接当成"已登录"。
    bool terminal_online = false;
    // 会话被占：服务器回「已在线 / 已在其它设备登录」但没说是本终端。
    // 这类失败重试一万次也是同一句话，必须走「注销 → 重登」。
    bool in_use = false;
    std::string request_url;    // 供诊断输出（未脱敏原文，调用方负责脱敏）
    std::string request_body;
};

class IAuth {
public:
    virtual ~IAuth() = default;
    virtual LoginResult Login(const Profile& profile, const NetContext& ctx) = 0;

    // 主动注销（杀掉服务器侧会话）。默认不支持：只有协议里真有注销接口的厂商才实现，
    // 免得对别的厂商乱发请求。返回 true 表示服务器接受了注销（或至少没拒绝）。
    virtual bool Logout(const Profile& profile, std::string& message) {
        (void)profile;
        message = "该认证方式不支持注销";
        return false;
    }

    virtual const char* Name() const = 0;
};

}  // namespace cnk
