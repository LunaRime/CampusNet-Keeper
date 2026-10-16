#pragma once
//
// 任务 26：按需加载 CLR（hostfxr）与 UI 形态。
//
// 刻意**不用 nethost**：链接 nethost.lib 会让 exe 依赖 nethost.dll，而它只随 .NET SDK
// 分发（只装运行时的用户机器上没有），会在进程启动时直接加载失败——这正是本项目
// 不允许引入的第三方依赖（Global Constraints）。所以 hostfxr.dll 的定位由我们自己找：
//   ① 载荷目录里的 hostfxr.dll（自包含发布产物，零依赖单 exe 用这条）；
//   ② 注册表 HKLM\SOFTWARE\dotnet\Setup\InstalledVersions\x64\InstallLocation；
//   ③ %DOTNET_ROOT%；④ %ProgramFiles%\dotnet；
// 在 <root>\host\fxr\<版本>\ 里挑**最高版本**再 LoadLibrary。
#include <string>

namespace cnk {

// UI 形态（--ui）：释放内嵌 UI → 用 hostfxr 加载 .NET 运行时 → 跑托管入口。
// 返回进程退出码；失败时已弹过原生提示框并把原因写进日志（守护不受影响）。
int RunUiMode();

// 守护形态：以 --ui 启动自身的副本（不等待其退出）。返回是否成功创建进程。
bool LaunchUiProcess();

// 是否已有设置界面进程在运行（靠界面进程持有的命名互斥体判断）。
// 托盘连点/双击时用它挡住"开出一堆窗口"；--ui 自己也用它兜底。
bool UiProcessRunning();

// 供 --ui-info（诊断）与自检使用：解析 hostfxr.dll 的完整路径（UTF-8）。
bool ResolveHostfxrPath(const std::string& ui_dir, std::string& out_path,
                        std::string& error);

// --ui-info 用的载荷/运行时状态（不弹出界面、不释放载荷）
struct UiPayloadStatus {
    bool present = false;                 // exe 里有没有内嵌界面载荷
    unsigned long long payload_bytes = 0;
    std::string payload_id;               // 版本戳：<版本>+<载荷指纹>
    std::string ui_dir;                   // %LOCALAPPDATA%\CampusNet-Keeper\ui
    bool extracted = false;               // 目录里已有解压产物
    bool up_to_date = false;              // 版本戳与当前载荷一致（打开界面会直接复用）
    bool hostfxr_found = false;
    std::string hostfxr_path;
    std::string hostfxr_error;            // 找不到时的排查说明
};

UiPayloadStatus InspectUiPayload();

}  // namespace cnk
