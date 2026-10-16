using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Styling;

namespace CampusNet.UI;

/// <summary>
/// 不显示窗口的自检：验证**设计令牌层**在明/暗两种主题下都能解析，且取值与 DESIGN.md 一致。
/// 
/// 为什么需要它：占位窗口"看着对"不算验证。这里把 docs/design/avalonia-mapping.md §1–§3
/// 的每个键与色值都钉住——主题字典漏键、色值抄错、<c>ThemeVariant</c> 没接上、
/// 明暗字典键不对应，任何一条都会立刻失败，且可在 CI 里无人值守运行。
/// 
/// 用法：<c>CampusNet.UI.exe --self-test</c>（退出码 0 = 全过）
/// </summary>
internal static class SelfTest
{
    private static readonly (string Key, string Light, string Dark)[] ExpectedBrushes =
    {
        // 键与取值全部取自 DESIGN.md（shadcn/ui）；亮暗为数学反转
        ("CnkBg", "#ffffffff", "#ff0a0a0a"),
        ("CnkText", "#ff0a0a0a", "#fffafafa"),
        ("CnkTextSecondary", "#ff3f3f46", "#ffd4d4d8"),
        ("CnkTextMuted", "#ff71717a", "#ffa1a1aa"),
        ("CnkTextFaint", "#ffa1a1aa", "#ff71717a"),
        ("CnkBrand", "#ff0a0a0a", "#fffafafa"),
        ("CnkBrandHover", "#ff1a1a1a", "#ffe4e4e7"),
        ("CnkBrandActive", "#ff27272a", "#ffd4d4d8"),
        ("CnkOnBrand", "#fffafafa", "#ff0a0a0a"),
        ("CnkSurface", "#fffafafa", "#ff171717"),
        ("CnkSurfaceStrong", "#fff4f4f5", "#ff27272a"),
        ("CnkSurfaceElevated", "#ffffffff", "#ff18181b"),
        ("CnkBorder", "#ffe4e4e7", "#ff27272a"),
        ("CnkBorderSoft", "#fff4f4f5", "#ff18181b"),
        ("CnkBorderStrong", "#ffd4d4d8", "#ff3f3f46"),
        ("CnkInputBorder", "#ffe4e4e7", "#ff27272a"),
        ("CnkRing", "#ff0a0a0a", "#fffafafa"),
        ("CnkDanger", "#ffef4444", "#ff7f1d1d"),
        ("CnkDangerHover", "#ffdc2626", "#ff991b1b"),
        ("CnkSuccess", "#ff10b981", "#ff10b981"),
        ("CnkWarning", "#fff59e0b", "#fff59e0b"),
        ("CnkInfo", "#ff3b82f6", "#ff3b82f6"),
        ("CnkSelection", "#1a0a0a0a", "#1ffafafa"),
    };

    public static int Run()
    {
        // WinExe 没有自带控制台：显式设成 UTF-8，否则中文在 GBK 控制台上是乱码
        try { Console.OutputEncoding = System.Text.Encoding.UTF8; } catch { /* 无控制台时忽略 */ }

        Console.WriteLine("UI 自检：设计令牌层");

        var app = AppBuilder.Configure<App>().UsePlatformDetect().SetupWithoutStarting()
                      .Instance ?? throw new InvalidOperationException("Avalonia 初始化失败");
        var failed = 0;

        foreach (var (key, light, dark) in ExpectedBrushes)
        {
            CheckBrush(app, key, ThemeVariant.Light, light, ref failed);
            CheckBrush(app, key, ThemeVariant.Dark, dark, ref failed);
        }

        // 圆角（单一 radius 基准派生）
        CheckValue(app, "CnkRadiusSm", ThemeVariant.Light, 6, ref failed);
        CheckValue(app, "CnkRadiusMd", ThemeVariant.Light, 8, ref failed);
        CheckValue(app, "CnkRadiusLg", ThemeVariant.Light, 12, ref failed);

        // 字号（DESIGN.md 的 type scale）
        CheckValue(app, "CnkFontSizeTitle", ThemeVariant.Light, 24, ref failed);
        CheckValue(app, "CnkFontSizeLabel", ThemeVariant.Light, 14, ref failed);
        CheckValue(app, "CnkFontSizeCaption", ThemeVariant.Light, 12, ref failed);

        // 字体栈：必须是 DESIGN.md 的栈 + 中文回退
        CheckFont(app, "CnkFontSans", "Microsoft YaHei UI", ref failed);
        CheckFont(app, "CnkFontMono", "Consolas", ref failed);

        Console.WriteLine(failed == 0
            ? $"  [PASS] {ExpectedBrushes.Length} 个颜色键 × 明暗 + 圆角/字号/字体全部符合 DESIGN.md"
            : $"  [FAIL] 共 {failed} 项不符合");

        // 结果同时落盘：WinExe 的退出码在 PowerShell 里不可靠（& 与 Start-Process 都可能拿不到），
        // 脚本/CI 直接读这个文件更稳。
        var verdict = failed == 0 ? "PASS" : $"FAIL {failed}";
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "ui-selftest.txt");
            File.WriteAllText(path, verdict + Environment.NewLine);
            Console.WriteLine($"  结果文件: {path}");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  （结果文件写入失败：{ex.Message}）");
        }

        Console.WriteLine(failed == 0 ? "主题自检：全部通过" : $"主题自检：{failed} 项失败");
        return failed == 0 ? 0 : 1;
    }

    private static void CheckBrush(Application app, string key, ThemeVariant variant,
                                   string expect, ref int failed)
    {
        if (!app.TryFindResource(key, variant, out var value))
        {
            Console.WriteLine($"  [FAIL] {variant} 缺键 {key}");
            failed++;
            return;
        }
        if (value is not ISolidColorBrush brush)
        {
            Console.WriteLine($"  [FAIL] {variant} {key} 不是画刷：{value?.GetType().Name ?? "(null)"}");
            failed++;
            return;
        }
        // 比 Color 而不是比字符串：Color.ToString() 对已知色会返回名字（#ffffff → "White"）
        var wanted = Color.Parse(expect);
        if (brush.Color != wanted)
        {
            Console.WriteLine($"  [FAIL] {variant} {key} = {brush.Color}，期望 {wanted}");
            failed++;
        }
    }

    private static void CheckValue(Application app, string key, ThemeVariant variant,
                                   double expect, ref int failed)
    {
        if (!app.TryFindResource(key, variant, out var value))
        {
            Console.WriteLine($"  [FAIL] {variant} 缺键 {key}");
            failed++;
            return;
        }
        var actual = value switch
        {
            CornerRadius r => r.TopLeft,
            double d => d,
            _ => double.NaN,
        };
        if (Math.Abs(actual - expect) > 0.001)
        {
            Console.WriteLine($"  [FAIL] {variant} {key} = {actual}，期望 {expect}");
            failed++;
        }
    }

    private static void CheckFont(Application app, string key, string mustContain, ref int failed)
    {
        if (!app.TryFindResource(key, ThemeVariant.Light, out var value) ||
            value is not FontFamily family)
        {
            Console.WriteLine($"  [FAIL] 缺字体键 {key}");
            failed++;
            return;
        }
        // 字体栈里必须含有中文回退族，否则界面上的中文会落到不确定的回退上
        if (family.ToString().IndexOf(mustContain, StringComparison.OrdinalIgnoreCase) < 0)
        {
            Console.WriteLine($"  [FAIL] {key} = {family}，其中不含 {mustContain}（中文回退缺失）");
            failed++;
        }
    }
}
