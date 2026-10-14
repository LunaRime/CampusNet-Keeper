#pragma once
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATAW 等托盘 API 类型
#include <string>
#include <functional>
#include "core/Scheduler.h"

namespace cnk {

class TrayIcon {
public:
    bool Create(HINSTANCE hinst, const char* tooltip);
    void Destroy();
    void SetState(RunState state, const std::string& tooltip);
    void ShowBalloon(const std::string& title, const std::string& text);
    HWND Hwnd() const { return hwnd_; }

    void SetCallbacks(std::function<void()> on_open_ui,
                      std::function<void()> on_reconnect,
                      std::function<void()> on_open_log,
                      std::function<void()> on_quit);

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

    void OnTrayMessage(LPARAM lparam);
    void ShowContextMenu();
    HICON IconForState(RunState s) const;
};

// 阻塞式守护循环：创建托盘、装 500ms 定时器驱动 Scheduler、
// 接入地址变更监听，直到退出消息。
bool RunDaemonLoop(HINSTANCE hinst, AppConfig cfg);

}  // namespace cnk
