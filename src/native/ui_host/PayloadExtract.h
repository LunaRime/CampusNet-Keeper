#pragma once
//
// 任务 26：内嵌 UI 载荷的解析与释放。
//
// 载荷是 exe 里的 RCDATA 资源（一个 ZIP），内容是 Avalonia 界面的发布产物。
// **只支持"存储"（method=0）条目**：本项目不允许引入第三方解压库（Global Constraints），
// 而 Windows 自带的 compressapi.dll 没有 deflate，所以打包侧（scripts/embed_ui.ps1）
// 用 .NET 的 CompressionLevel.NoCompression 产出存储型 ZIP，C++ 侧只需解析中央目录 +
// 按偏移拷数据——零依赖、可单测。
//
// 遇到压缩/加密/ZIP64/路径穿越条目一律返回**明确错误**，绝不写出坏数据。
#include <filesystem>
#include <string>
#include <vector>

namespace cnk {

// 把路径按 UTF-8 转宽字符（与本项目其它地方一致的口径）
std::wstring Utf8ToWide(const std::string& s);

// 宽字符转 UTF-8
std::string WideToUtf8(const std::wstring& w);

// ---------------------------------------------------------------- 内嵌载荷

struct EmbeddedPayload {
    const void* data = nullptr;   // 指向模块资源，生命周期同模块，不需释放
    size_t size = 0;              // 字节数
    std::string id;               // 版本戳：<版本>+<载荷指纹>（由打包脚本生成）
    bool present = false;         // false = 精简单文件版（exe 里没有界面载荷）
};

// 读取自身 exe 的 UI_PAYLOAD / UI_PAYLOAD_ID 资源。
// 载荷不存在时 present=false，error 里是"这是精简版"的人话说明（不是失败路径）。
EmbeddedPayload LoadEmbeddedPayload(std::string& error);

// ---------------------------------------------------------------- 可单测的核心

struct ZipEntryInfo {
    std::string name;
    unsigned long long size = 0;
    bool stored = true;           // false = 压缩条目（诊断要如实报告）
};

// 只读中央目录（不解压）。用于诊断与"载荷是否全为存储型"的自检。
bool ListStoredZip(const void* zip, size_t size, std::vector<ZipEntryInfo>& out,
                   std::string& error);

// 解压"仅存储型条目"的 ZIP 到 dest_dir（必须已存在）。失败时不保证目录干净——
// 调用方负责用临时目录 + 改名保证原子性（见 EnsureUiPayloadFromBytes）。
bool ExtractStoredZip(const void* zip, size_t size,
                      const std::filesystem::path& dest_dir, std::string& error);

// ui_dir\.version 的内容（读不到返回空串，自动去掉行尾空白）
std::string ReadUiPayloadMarker(const std::filesystem::path& ui_dir);

// 版本戳一致 + 关键文件在 → 可以复用已释放的目录
bool UiPayloadUsable(const std::filesystem::path& ui_dir, const std::string& payload_id);

// 释放（或复用）UI 载荷到 base_dir\ui：
//   · 版本戳命中 → 直接复用，不读 zip；
//   · 否则解到 base_dir\ui.tmp-<pid>-<tick>（避免半成品被人当成品用），
//     成功写戳后再改名成 ui；旧目录先改名为 ui.old-<pid>-<tick> 再删（删不掉只记日志）。
// 目标被同名文件占住、改名失败等都会返回明确错误，并尽量保留原有可用目录。
bool EnsureUiPayloadFromBytes(const void* zip, size_t size, const std::string& payload_id,
                              const std::filesystem::path& base_dir,
                              std::string& ui_dir, std::string& error);

// 清理上次异常退出残留的 ui.tmp-*/ui.old-*（best-effort），返回清掉的个数。
// 当前正在用的 ui 目录与其它文件一律不动。
int CleanStaleUiDirs(const std::filesystem::path& base_dir);

// ---------------------------------------------------------------- 产品入口

// 释放内嵌载荷到 %LOCALAPPDATA%\CampusNet-Keeper\ui（版本戳命中则复用）。
// 跨进程用命名互斥体串行化，避免两个界面同时解压。返回目录（UTF-8）。
bool EnsureUiPayload(std::string& ui_dir, std::string& error);

}  // namespace cnk
