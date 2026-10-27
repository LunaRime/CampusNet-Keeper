#pragma once
#include <string>

namespace cnk {

// 上一次运行的「退出画像」。
//
// 为什么需要它：守护进程"不在了"是最常见也最难查的故障——用户只看到"没联网"，
// 而日志里既没有崩溃记录也没有退出记录，事后永远分不清是「自己退的 / 被强杀 /
// 崩了 / 断电」。
//
// 做法：**正常退出时写一个标记文件（含来源），启动时读走并删除**。
// 启动时读不到标记 ⇒ 上次不是正常退出的（被强制结束、异常终止或掉电）。
// 标记只由守护形态读写：`--ui` 等一次性进程不参与（否则会误判成"没正常退出"）。
struct LastExit {
    bool recorded = false;   // 是否读到上一次的退出记录
    std::string reason;      // 退出来源（人类可读，见 TrayIcon / main 的写入点）
    std::string at;          // 记录时间（YYYY-MM-DD HH:MM:SS）
};

// 读取并**删除**上一次的退出标记（本次运行开始，标记由本次重新写入）。
// 进程崩溃/被强杀时无人调用 RecordExit，于是下一次启动这里读不到 → recorded=false。
LastExit TakeLastExit();

// 正常退出时调用：写入标记，内容为 reason。
bool RecordExit(const std::string& reason);

// 标记文件的完整路径（诊断与测试用）。
std::string LastExitFilePath();

// 测试专用：把标记文件重定向到指定目录（空串 = 恢复默认 %APPDATA% 目录）。
// ⚠ 铁律：测试不得与常驻进程共享命名资源——否则测试会顺手删掉正在运行的
//  守护进程的标记，把"上次是否正常退出"这个判据搅乱。
void SetLastExitDirForTest(const std::string& dir);

}  // namespace cnk
