#pragma once
#include <string>
#include <vector>

namespace cnk {

/// 导出窗口。
struct DiagRange {
    int minutes = 15;        // 默认最近 15 分钟
    bool around = false;     // true = 以 around_ts 为中心，前后各 2 分钟
    std::string around_ts;   // "YYYY-MM-DD HH:MM:SS"（本地时间）
};

struct DiagResult {
    bool ok = false;
    std::string path;                     // 落盘路径
    int lines = 0;
    bool truncated = false;               // 触及行数/字节上限
    std::string error;
    std::vector<std::string> sources;     // 实际读到的文件（仅文件名）
    std::vector<std::string> missing;     // 窗口涉及但没有文件的日期
};

/// 导出一段脱敏诊断日志。
///
/// 硬约定（D1）：
///   · **只采 CNK 自己的日志**（keeper-YYYY-MM-DD.log 及其 .N 轮转），支持跨午夜；
///   · 先脱敏再落盘：账号/凭据/门户查询参数 → ***，IP → a.b.x.x，MAC / 12 位数字 → ***；
///   · 不读配置文件、密码库，也不含原始门户回包（日志里本来就只有判定与原因）；
///   · 来源缺失、输出截断都写进文件头，不假装完整。
DiagResult ExportDiagnostics(const std::string& log_dir, const std::string& out_dir,
                             const std::string& build_info, const DiagRange& range);

/// 单行脱敏（导出用）。公开出来是为了能单独测试规则，不用碰文件系统。
std::string RedactForExport(const std::string& line);

}  // namespace cnk
