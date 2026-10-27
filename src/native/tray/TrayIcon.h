#pragma once
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATAW 等托盘 API 类型
#include <string>
#include <functional>
#include "core/Scheduler.h"

namespace cnk {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Create(HINSTANCE hinst, const char* tooltip);
    void Destroy();
    void SetState(RunState state, const std::string& tooltip);
    void ShowBalloon(const std::string& title, const std::string& text);
    HWND Hwnd() const { return hwnd_; }

    void SetCallbacks(std::function<void()> on_open_ui,
                      std::function<void()> on_reconnect,
                      std::function<void()> on_open_log,
                      std::function<void()> on_quit);

    // 退出来历：**谁**让守护进程结束的（托盘菜单 / 外部 WM_CLOSE / 系统关机…）。
    // 为什么要记：守护进程"莫名其妙不在了"是最难查的故障——日志里既没有崩溃也没有
    // 退出记录时，事后无从判断是自己退的还是被杀的（2026-10-27 真机上就这么消失过一次）。
    // 由 WndProc 与托盘「退出」回调写入，守护循环退出时统一落进日志与退出标记。
    void SetQuitReason(const std::string& r);
    const std::string& QuitReason() const { return quit_reason_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool created_ = false;
    RunState state_ = RunState::Disconnected;
    std::wstring last_tooltip_;

    std::function<void()> on_open_ui_;
    std::function<void()> on_reconnect_;
    std::function<void()> on_open_log_;
    std::function<void()> on_quit_;
    std::string quit_reason_;   // 退出来历（见 SetQuitReason）

    void OnTrayMessage(LPARAM lparam);
    void ShowContextMenu();
    // 应用图标 + 状态徽标（按状态缓存；不再是 Windows 通用图标）
    HICON IconForState(RunState s);
    void DestroyIcons();

    HICON app_icon_ = nullptr;        // exe 资源里的应用图标（IDI_APPICON）
    HICON state_icons_[8] = {};       // 按 RunState 缓存的合成图标
};

// 阻塞式守护循环：创建托盘、装 500ms 定时器驱动 Scheduler、
// 接入地址变更监听，直到退出消息。
// open_ui_on_start：用户**双击 exe**（无参数）启动时，托盘就绪后自动打开设置界面——
// 否则双击后只看得到托盘图标（还可能被折叠进溢出区），像是"什么都没发生"。
// 开机自启带 --autostart，不会传这个标志。
bool RunDaemonLoop(HINSTANCE hinst, AppConfig cfg, bool open_ui_on_start = false);

}  // namespace cnk
