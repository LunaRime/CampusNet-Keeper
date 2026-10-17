#include "tray/TrayIcon.h"
#include "sys/SingleInstance.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/NetworkChange.h"
#include "ipc/DaemonCommands.h"
#include "ipc/PipeServer.h"
#include "ui_host/Hostfxr.h"
#include <shellapi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace cnk {

namespace {

// 注意：窗口类名使用 sys/SingleInstance.h 导出的 kHiddenWindowClass——
// 第二实例的 NotifyExistingInstance 依赖 FindWindowW 找到本窗口，两处必须一致
constexpr UINT kTrayCallbackMsg = WM_APP + 1;   // 已与 kMsgShowUiRequest(+100) 错开
constexpr UINT kTimerId = 1;
constexpr UINT kTimerIntervalMs = 500;

// 应用图标资源 id：必须与 src/native/app.rc 的 `#define IDI_APPICON` 一致
constexpr int kAppIconResourceId = 101;
constexpr int kStateCount = 8;   // RunState 目前 7 个，留一个余量

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

// ---------------------------------------------------------------- 托盘图标

// 状态徽标颜色（与 DESIGN.md 的语义令牌同源：成功绿 / 信息蓝 / 危险红 / 中性灰）
COLORREF BadgeColor(RunState s) {
    switch (s) {
        case RunState::Online: return RGB(0x16, 0xA3, 0x4A);
        case RunState::Probing:
        case RunState::PortalDetected:
        case RunState::Authenticating: return RGB(0x25, 0x63, 0xEB);
        case RunState::AuthFailed:
        case RunState::NotCampusNetwork: return RGB(0xDC, 0x26, 0x26);
        case RunState::Disconnected:
        default: return RGB(0x6B, 0x72, 0x80);
    }
}

// 从本 exe 资源加载应用图标；资源缺失（例如测试程序没编进 app.rc）时回退系统图标
HICON LoadAppIcon(HINSTANCE hinst, int size) {
    HICON icon = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(kAppIconResourceId),
                                   IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    if (!icon) {
        icon = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, size, size,
                                 LR_DEFAULTCOLOR | LR_SHARED);
    }
    return icon;
}

// 合成"应用图标 + 右下角状态圆点"的托盘图标。
// 为什么手写像素而不是用 GDI 画圆：GDI 往 32bpp DIB 上画**不会补 alpha**，
// 画出来的圆点在托盘里会变透明（经典坑）。这里逐像素写 ARGB，并给圆点加 1px 深色
// 描边，保证在深/浅任务栏上都有边界感。
HICON ComposeStateIcon(HICON base, COLORREF color, int size) {
    if (!base || size <= 0) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;   // 负高度 = 自上而下，行序与直觉一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem || !color_bmp || !bits) {
        if (color_bmp) DeleteObject(color_bmp);
        if (mem) DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(mem, color_bmp);
    std::memset(bits, 0, (size_t)size * (size_t)size * 4u);
    DrawIconEx(mem, 0, 0, base, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(mem, old);

    auto* px = static_cast<uint32_t*>(bits);
    const int r = std::max(3, size / 3);      // 徽标半径
    const int cx = size - r - 1;
    const int cy = size - r - 1;
    const uint8_t cr = GetRValue(color);
    const uint8_t cg = GetGValue(color);
    const uint8_t cb = GetBValue(color);
    const int r_in = std::max(1, r - 1);      // 描边内圈
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            if (x < 0 || y < 0 || x >= size || y >= size) continue;
            const int dx = x - cx;
            const int dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            uint32_t argb = 0;
            if (d2 <= r_in * r_in) {
                argb = 0xFF000000u | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
            } else if (d2 <= r * r) {
                argb = 0xFF1F1F1Fu;           // 深色描边：浅色任务栏上也看得见边界
            } else {
                continue;
            }
            px[(size_t)y * size + x] = argb;
        }
    }

    // 32bpp 图标靠 alpha 通道；mask 全 0（不透明）即可，现代 shell 不看 mask
    std::vector<uint8_t> mask_bits((size_t)(((size + 15) / 16) * 2) * (size_t)size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color_bmp;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color_bmp);
    if (mask) DeleteObject(mask);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return icon;
}

}  // namespace

HICON TrayIcon::IconForState(RunState s) {
    const int idx = (int)s;
    if (idx < 0 || idx >= kStateCount) return app_icon_;
    if (!state_icons_[idx]) {
        state_icons_[idx] = ComposeStateIcon(app_icon_, BadgeColor(s),
                                             GetSystemMetrics(SM_CXSMICON));
    }
    return state_icons_[idx] ? state_icons_[idx] : app_icon_;
}

void TrayIcon::DestroyIcons() {
    for (auto& icon : state_icons_) {
        if (icon) {
            DestroyIcon(icon);
            icon = nullptr;
        }
    }
    if (app_icon_) {
        DestroyIcon(app_icon_);
        app_icon_ = nullptr;
    }
}

TrayIcon::~TrayIcon() { DestroyIcons(); }

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

    // 托盘/任务栏用**应用自己的图标**（exe 资源 IDI_APPICON），不再用 Windows 通用图标
    const int icon_size = GetSystemMetrics(SM_CXSMICON);
    app_icon_ = LoadAppIcon(hinst, icon_size > 0 ? icon_size : 16);
    if (app_icon_) {
        LogInfo("托盘图标：应用图标 IDI_APPICON（" + std::to_string(icon_size) + "px，按状态合成徽标）");
    } else {
        LogWarn("加载应用图标失败（app.rc 里的 IDI_APPICON？），托盘将用系统默认图标");
    }

    hwnd_ = CreateWindowExW(0, kHiddenWindowClass, L"CampusNetKeeper",
                            0, 0, 0, 0, 0, nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    // 放行跨完整性级别的「弹 UI 请求」：提权启动或未来服务化时，
    // 第二实例的 PostMessage 会被 UIPI 静默拦截（FindWindow 不受影响，故障隐蔽）
    if (!ChangeWindowMessageFilterEx(hwnd_, kMsgShowUiRequest, MSGFLT_ALLOW, nullptr)) {
        LogWarn("放行 UI 请求消息失败（跨权限场景下第二实例的唤起可能不生效）");
    }

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
    DestroyIcons();
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
    // 标准收尾（MSDN KB135788）：确保菜单在点击别处时可靠消失
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

bool RunDaemonLoop(HINSTANCE hinst, AppConfig cfg, bool open_ui_on_start) {
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
        // 任务 26：真的启动设置界面（自身副本 + --ui）。
        // 已经在运行时只提示、不再开第二个窗口（托盘单击/双击都会走到这里）。
        if (UiProcessRunning()) {
            LogInfo("设置界面已在运行，忽略本次打开请求");
            tray.ShowBalloon("设置界面", "设置界面已在运行。");
            return;
        }
        if (LaunchUiProcess()) {
            LogInfo("已启动设置界面进程（--ui）");
        } else {
            LogError("启动设置界面进程失败");
            tray.ShowBalloon("设置界面", "无法启动设置界面，详见日志目录中的 keeper.log。");
        }
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

    if (!SetTimer(tray.Hwnd(), kTimerId, kTimerIntervalMs, nullptr)) {
        LogError("定时器创建失败，守护循环无法驱动探测");
        netwatch.Stop();
        tray.Destroy();
        return false;
    }

    // ---- 设置界面的 IPC（任务 25）----
    // 起命名管道服务并把命令交给 DaemonCommands；管道不可用时只记日志、不影响守护主功能。
    // 生命周期由局部变量管理：本函数返回时 ~PipeServer 会 Stop()（有界、不阻塞退出）。
    PipeServer ipc;
    {
        std::string ipc_err;
        if (ipc.Start([sch](const IpcRequest& req) { return HandleDaemonCommand(req, *sch); },
                      ipc_err)) {
            LogInfo("设置界面 IPC 已就绪：" + ipc.Name());
        } else {
            LogWarn("设置界面 IPC 启动失败：" + ipc_err + "（界面将连不上，守护功能不受影响）");
        }
    }

    // 用户**双击 exe**（无参数）首次启动时，托盘就绪后自动打开设置界面。
    // 否则双击后只有托盘图标（还可能被折叠进溢出区）——对用户来说就像"什么都没发生"。
    // 开机自启带 --autostart，不会走到这里。
    if (open_ui_on_start) {
        LogInfo("检测到双击启动（无参数）：自动打开设置界面");
        open_ui();
    }

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
            // 都不是恢复，否则会把「启动即在线」误报为「已自动重新登录」。
            // 文案保持中性：网络抖动（WiFi 重连/无 Portal）的恢复并不经过登录。
            if (st == RunState::Online && ever_was_online && !prev_online &&
                cfg.settings.notify_on_reconnect) {
                tray.ShowBalloon("网络已恢复", "网络连接已恢复正常");
                LogInfo("连接恢复：网络已回到在线状态");
            }
            if (st == RunState::AuthFailed || st == RunState::NotCampusNetwork) {
                tray.ShowBalloon("需要处理", sch->LastMessage());
            }
            if (st == RunState::Online) ever_was_online = true;
            prev_online = (st == RunState::Online);
            last_state = st;
            has_last = true;
        }

        // 空闲等待：有消息（含定时器到期）立刻醒，否则最多等 250ms。
        // 状态变化只由 WM_TIMER→Tick 产生，等待会被其唤醒，无需高频空转。
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
    }
}

}  // namespace cnk
