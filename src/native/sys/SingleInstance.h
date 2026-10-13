#pragma once
#include <string>
#include <windows.h>

namespace cnk {

// 隐藏消息窗口的类名（任务 17 的守护消息循环注册；跨进程通知的寻址目标）
inline constexpr const wchar_t* kHiddenWindowClass = L"CampusNetKeeperHidden";

// 自定义窗口消息：请求守护进程弹出设置界面。
// 守护进程应在 kHiddenWindowClass 窗口上处理此消息。
constexpr UINT kMsgShowUiRequest = WM_APP + 1;

// 基于命名互斥体的单实例保证。
// 使用会话本地（Local\）命名空间：普通账户无需任何特权即可创建，
// 且多用户切换登录时各自拥有独立实例（配置与凭据本就是按用户隔离的）。
class SingleInstance {
public:
    // mutex_name 为空时使用默认互斥体名；测试可传独立名字，
    // 避免与真实常驻守护实例互相干扰
    explicit SingleInstance(const wchar_t* mutex_name = nullptr);
    ~SingleInstance();

    // 成功获取返回 true；已有实例持有则返回 false。
    bool Acquire();
    void Release();

    // 通知已在运行的实例弹出设置界面。返回是否成功送达。
    static bool NotifyExistingInstance();

    // 守护进程在消息循环中轮询此标志，为 true 时弹 UI。
    // 完整实现依赖任务 17 的消息窗口；当前为占位。
    static bool ConsumeUiRequest();

private:
    void* handle_ = nullptr;
    std::wstring name_;
};

}  // namespace cnk
