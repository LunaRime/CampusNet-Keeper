#include "tray/TrayIcon.h"
#include "sys/SingleInstance.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/NetworkChange.h"
#include <shellapi.h>
#include <memory>
#include <thread>

namespace cnk {

namespace {

// 注意：窗口类名使用 sys/SingleInstance.h 导出的 kHiddenWindowClass——
// 第二实例的 NotifyExistingInstance 依赖 FindWindowW 找到本窗口，两处必须一致
constexpr UINT kTrayCallbackMsg = WM_APP + 1;   // 已与 kMsgShowUiRequest(+100) 错开
constexpr UINT kTimerId = 1;
constexpr UINT kTimerIntervalMs = 500;

enum MenuId {
    kMenuStatus = 1,
    kMenuOpenUi = 2,
    kMenuReconnect = 3,
    kMenuOpenLog = 4,
    kMenuQuit = 5,
};

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

void OpenPathInExplorer(const std::string& path) {
    ShellExecuteW(nullptr, L"open", Utf8ToWide(path).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

HICON TrayIcon::IconForState(RunState s) const {
    // 使用系统内置图标区分状态，避免外部资源依赖（M2 可替换为自绘图标）
    LPCWSTR id = IDI_APPLICATION;
    switch (s) {
        case RunState::Online:            id = IDI_INFORMATION; break;
        case RunState::Disconnected:      id = IDI_WARNING; break;
        case RunState::AuthFailed:
        case RunState::NotCampusNetwork:  id = IDI_ERROR; break;
        case RunState::Probing:
        case RunState::PortalDetected:
        case RunState::Authenticating:
        default:                          break;   // 默认图标
    }
    return LoadIconW(nullptr, id);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayIcon* self = (TrayIcon*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case kTrayCallbackMsg:
            self->OnTrayMessage(lp);
            return 0;
        case kMsgShowUiRequest:
            if (self->on_open_ui_) self->on_open_ui_();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kMenuOpenUi:    if (self->on_open_ui_) self->on_open_ui_(); break;
                case kMenuReconnect: if (self->on_reconnect_) self->on_reconnect_(); break;
                case kMenuOpenLog:   if (self->on_open_log_) self->on_open_log_(); break;
                case kMenuQuit:      if (self->on_quit_) self->on_quit_(); break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool TrayIcon::Create(HINSTANCE hinst, const char* tooltip) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kHiddenWindowClass;
    RegisterClassExW(&wc);   // 已注册时返回 0，可忽略

    hwnd_ = CreateWindowExW(0, kHiddenWindowClass, L"CampusNetKeeper",
                            0, 0, 0, 0, 0, nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    // 放行跨完整性级别的「弹 UI 请求」：提权启动或未来服务化时，
    // 第二实例的 PostMessage 会被 UIPI 静默拦截（FindWindow 不受影响，故障隐蔽）
    ChangeWindowMessageFilterEx(hwnd_, kMsgShowUiRequest, MSGFLT_ALLOW, nullptr);

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kTrayCallbackMsg;
    nid_.hIcon = IconForState(RunState::Disconnected);
    last_tooltip_ = Utf8ToWide(tooltip);
    wcsncpy_s(nid_.szTip, last_tooltip_.c_str(), _TRUNCATE);

    created_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    return created_;
}

void TrayIcon::Destroy() {
    if (created_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        created_ = false;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TrayIcon::SetState(RunState state, const std::string& tooltip) {
    state_ = state;
    last_tooltip_ = Utf8ToWide(tooltip);
    if (!created_) return;
    nid_.hIcon = IconForState(state);
    wcsncpy_s(nid_.szTip, last_tooltip_.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::ShowBalloon(const std::string& title, const std::string& text) {
    if (!created_) return;
    nid_.uFlags = NIF_INFO;
    wcsncpy_s(nid_.szInfoTitle, Utf8ToWide(title).c_str(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, Utf8ToWide(text).c_str(), _TRUNCATE);
    nid_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void TrayIcon::SetCallbacks(std::function<void()> on_open_ui,
                            std::function<void()> on_reconnect,
                            std::function<void()> on_open_log,
                            std::function<void()> on_quit) {
    on_open_ui_ = std::move(on_open_ui);
    on_reconnect_ = std::move(on_reconnect);
    on_open_log_ = std::move(on_open_log);
    on_quit_ = std::move(on_quit);
}

void TrayIcon::OnTrayMessage(LPARAM lparam) {
    switch (LOWORD(lparam)) {
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            if (on_open_ui_) on_open_ui_();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu();
            break;
    }
}

void TrayIcon::ShowContextMenu() {
    HMENU menu = CreatePopupMenu();
    // 状态行：显示最近一次的工具提示文本（即调度器的状态说明）
    std::wstring status = L"状态：" + last_tooltip_;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuOpenUi, L"设置…");
    AppendMenuW(menu, MF_STRING, kMenuReconnect, L"立即重连");
    AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"打开日志目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出");

    SetForegroundWindow(hwnd_);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

bool RunDaemonLoop(HINSTANCE hinst, AppConfig cfg) {
    auto sch = std::make_shared<Scheduler>(cfg);
    TrayIcon tray;

    // 地址变更 / 睡眠唤醒时立即探测，是达成 15 秒恢复目标的关键
    NetworkChangeWatcher netwatch;
    netwatch.Start([sch]() { sch->NotifyNetworkChanged(); });

    if (!tray.Create(hinst, "CampusNet-Keeper 启动中…")) {
        LogError("托盘图标创建失败");
        netwatch.Stop();
        return false;
    }
    LogInfo("进入托盘守护循环");

    auto open_ui = [&tray]() {
        // M2 会替换为启动 --ui 子进程
        LogInfo("收到打开设置界面请求（M2 前为气泡提示）");
        tray.ShowBalloon("设置界面", "设置界面将在下一版本提供。\n"
                                     "当前可手动编辑 %APPDATA%\\CampusNet-Keeper\\config.json");
    };

    tray.SetCallbacks(
        open_ui,
        [sch, &tray]() {
            sch->ForceReconnect();
            tray.ShowBalloon("CampusNet-Keeper", "已触发立即重连");
        },
        [&tray]() {
            std::string dir = AppDataDir();
            OpenPathInExplorer(dir);
        },
        [&tray]() { tray.Destroy(); PostQuitMessage(0); });

    SetTimer(tray.Hwnd(), kTimerId, kTimerIntervalMs, nullptr);

    RunState last_state = RunState::Disconnected;
    bool has_last = false;          // 首次状态更新（含 tooltip 初始化）必发生
    bool ever_was_online = false;   // 曾进入过 Online（用于区分「首次连通」与「掉线恢复」）
    bool prev_online = false;       // 上一状态是否为 Online

    MSG msg;
    for (;;) {
        // 抽出所有排队消息
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                KillTimer(tray.Hwnd(), kTimerId);
                netwatch.Stop();
                tray.Destroy();
                LogInfo("守护循环退出");
                return true;
            }
            if (msg.message == WM_TIMER && msg.wParam == kTimerId) {
                sch->Tick();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const RunState st = sch->State();
        if (!has_last || st != last_state) {
            std::string tip = "CampusNet-Keeper — " + sch->LastMessage();
            tray.SetState(st, tip);

            // 恢复通知：必须「曾经在线过」再「离开」再「回来」才弹——
            // 启动瞬态的初始 Disconnected（Tick 之前的显示）与「首次连通」
            // 都不是恢复，否则会把「启动即在线」误报为「已自动重新登录」
            if (st == RunState::Online && ever_was_online && !prev_online) {
                tray.ShowBalloon("网络已恢复", "已自动重新登录校园网");
                LogInfo("已自动重新登录并恢复在线");
            }
            if (st == RunState::AuthFailed || st == RunState::NotCampusNetwork) {
                tray.ShowBalloon("需要处理", sch->LastMessage());
            }
            if (st == RunState::Online) ever_was_online = true;
            prev_online = (st == RunState::Online);
            last_state = st;
            has_last = true;
        }

        // 空闲等待：有消息立刻醒，否则最多等 20ms
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
}

}  // namespace cnk
