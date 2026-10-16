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
                                    // 5=裸 302 劫持（2026-10-16 实测真实形态：Server 为
                                    //   NetEngine（拦截器特征、非 Dr.COM 指纹）、空正文；
                                    //   未登录时 /sim_probe 302 到第二监听端口上的
                                    //   /a79.htm，客户端须抓取目标页才能识别）
    std::string redirect_location;  // fail_mode == 3 时返回的 Location 值

    // true=整个新式 API 都不注册（/drcom/chkstatus、/eportal/portal/page/loadConfig、
    // /eportal/portal/login 一律 404），用于测试客户端「新式接口不存在 → 回退老式协议」
    // 的路径（此时回退通道是唯一依据，其判定应当被采信）。
    bool disable_new_api = false;

    // true=新式登录一律回「该终端 IP 已在线」（2026-10-16 线上实测响应：
    // result=0/ret_code=2/msg="终端IP已在线！"），用于钉住客户端的判定语义。
    bool already_online = false;

    // true=新式登录端点返回 404，但 loadConfig/chkstatus 正常——用于测试
    // 「新式接口在场、却因登录步骤故障而回退老式协议」这一路径（不得把回退通道的
    // 判定当成凭据错误）。
    bool new_api_login_broken = false;

    // 非空时，新式登录一律回 {"result":0,"msg":<该文案>,...}——用于钉住失败文案的分类。
    std::string new_api_fail_msg;
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

    // fail_mode==5 时的第二监听端口（模拟「同 IP、不同端口」的门户服务，
    // 即裸 302 的跳转目标所在）；未启用时为 0。
    int PortalPort() const;

    int LoginCount() const;
    std::string LastLoginBody() const;

    // 深澜（vendor="srun"）最近一次 get_challenge 下发的令牌；未启用时为空。
    // 供测试**独立**复算 hmac-md5/info/chksum（不依赖产品代码的公式）。
    std::string LastChallenge() const;

    // 是否已有一次成功的登录（模拟会话建立状态）。
    // /sim_probe 路径据此在「劫持（门户页）」与「正常（nCSI 应答）」间切换，
    // 供端到端验证「探测 → 劫持 → 登录 → 恢复」整链。
    bool LoggedIn() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
