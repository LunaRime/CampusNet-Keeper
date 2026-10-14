#pragma once
#include <string>
#include <windows.h>

namespace cnk {

// 隐藏消息窗口的类名（任务 17 的守护消息循环注册；跨进程通知的寻址目标）
inline constexpr const wchar_t* kHiddenWindowClass = L"CampusNetKeeperHidden";

// 自定义窗口消息：请求守护进程弹出设置界面。
// 守护进程应在 kHiddenWindowClass 窗口上处理此消息。
// 编号刻意避开 WM_APP+1（任务 17 的托盘图标回调消息惯用编号），防止同一
// 窗口的 WndProc 里两套语义相撞。
// ⚠ 任务 17 注记（2026-10-14 审阅）：守护窗口创建时需调用
// ChangeWindowMessageFilterEx(hwnd, kMsgShowUiRequest, MSGFLT_ALLOW, nullptr)
// 放行本消息——WM_APP+100 > WM_USER，跨完整性级别（提权启动/未来服务化）时
// 第二实例的 PostMessage 会被 UIPI 静默拦截，且 FindWindow 不受影响、故障隐蔽。
inline constexpr UINT kMsgShowUiRequest = WM_APP + 100;

// 基于命名互斥体的单实例保证。
// 使用会话本地（Local\）命名空间：普通账户无需任何特权即可创建。
// 注意语义是「每会话」而非「每用户」——同一用户的控制台 + RDP 双会话
// 会各有一个守护实例（配置读写由任务 14 的写入原子性保证互不损坏）。
class SingleInstance {
public:
    // mutex_name 为 nullptr 时使用默认互斥体名；测试可传独立名字，
    // 避免与真实常驻守护实例互相干扰
    explicit SingleInstance(const wchar_t* mutex_name = nullptr);
    ~SingleInstance();

    // 成功获取返回 true；已有实例持有则返回 false。
    bool Acquire();
    void Release();

    // 通知已在运行的实例弹出设置界面。返回是否成功送达。
    // 接收端在守护进程的隐藏消息窗口（TrayIcon 的 WndProc）里处理。
    static bool NotifyExistingInstance();

private:
    HANDLE handle_ = nullptr;
    std::wstring name_;
};

}  // namespace cnk
