using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Platform;
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
        // 键与取值全部取自 DESIGN.md v2（现代桌面原生）；暗色独立调校，不是数学反转
        ("CnkBg", "#fff3f5f9", "#ff0f1218"),
        ("CnkTitleBar", "#fffbfcfe", "#ff151a22"),
        ("CnkSurface", "#ffffffff", "#ff1a1f28"),
        ("CnkSurfaceStrong", "#ffeff3f9", "#ff252c39"),
        ("CnkSurfaceSubtle", "#fff7f9fc", "#ff202632"),
        ("CnkConsole", "#fff8fafc", "#ff141922"),
        ("CnkSidebar", "#fff9fafc", "#ff161b23"),
        ("CnkText", "#ff16202e", "#ffedf2f9"),
        ("CnkTextSecondary", "#ff46566b", "#ffb2bfd0"),
        ("CnkTextMuted", "#ff7b8b9f", "#ff8593a6"),
        ("CnkTextFaint", "#ffa8b4c3", "#ff5c6a7c"),
        ("CnkBrand", "#ff2f6bff", "#ff5b8cff"),
        ("CnkBrandHover", "#ff1f56e0", "#ff7aa2ff"),
        ("CnkBrandActive", "#ff1846c2", "#ff93b4ff"),
        ("CnkBrandSoft", "#ffe8efff", "#ff1e2c4a"),
        ("CnkOnBrand", "#ffffffff", "#ff0f1218"),
        ("CnkBorder", "#ffe2e8f1", "#ff2a3240"),
        ("CnkBorderStrong", "#ffcbd5e3", "#ff3b4657"),
        ("CnkInputBorder", "#ffcbd5e3", "#ff3b4657"),
        ("CnkRing", "#612f6bff", "#785b8cff"),
        ("CnkSelection", "#1a2f6bff", "#245b8cff"),
        ("CnkSuccess", "#ff16a34a", "#ff3ecf7a"),
        ("CnkSuccessSoft", "#ffe7f6ec", "#ff14301f"),
        ("CnkInfo", "#ff2563eb", "#ff6f9bff"),
        ("CnkInfoSoft", "#ffe9f0ff", "#ff1b2a4a"),
        ("CnkWarning", "#ffd97706", "#fff0a942"),
        ("CnkWarningSoft", "#fffef2df", "#ff34250e"),
        ("CnkDanger", "#ffdc2626", "#ffff6b6b"),
        ("CnkDangerHover", "#ffb91c1c", "#ffff8f8f"),
        ("CnkDangerSoft", "#fffdeaea", "#ff3a1a1c"),
        ("CnkOffline", "#ff64748b", "#ff94a3b8"),
        ("CnkOfflineSoft", "#ffeef1f5", "#ff242c38"),
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

        // 圆角（DESIGN.md v2 §2：控件 8 / 卡片 12 / 大面 16）
        CheckValue(app, "CnkRadiusSm", ThemeVariant.Light, 8, ref failed);
        CheckValue(app, "CnkRadiusMd", ThemeVariant.Light, 12, ref failed);
        CheckValue(app, "CnkRadiusLg", ThemeVariant.Light, 16, ref failed);

        // 字号（DESIGN.md v2 §3 的桌面字阶）
        CheckValue(app, "CnkFontSizeTitle", ThemeVariant.Light, 20, ref failed);
        CheckValue(app, "CnkFontSizeLabel", ThemeVariant.Light, 12, ref failed);
        CheckValue(app, "CnkFontSizeCaption", ThemeVariant.Light, 11.5, ref failed);

        // 字体栈：Windows 原生栈 + 中文回退（平台必需）
        CheckFont(app, "CnkFontSans", "Microsoft YaHei UI", ref failed);
        CheckFont(app, "CnkFontMono", "Consolas", ref failed);

        // 窗口图标：ApplicationIcon 只作用于 exe，Window.Icon 走的是 avares 资源。
        // 少了 csproj 的 AvaloniaResource，标题栏左上角就退回系统默认图标。
        CheckWindowIcon(ref failed);
        CheckWindowConstruction(ref failed);

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

    /// <summary>
    /// 真正构造一次主窗口并走一遍分区导航。编译通过不等于窗口能起来：
    /// v0.1.7 就是 XAML 运行时加载下 x:Name 字段为 null，在 DataContext 赋值时
    /// 抛 NullReferenceException，完整版 exe 点「设置」毫无反应。
    /// 这里不 Show 窗口，只验证构造 + 控件解析 + 导航，适合无人值守/CI。
    /// </summary>
    private static void CheckWindowConstruction(ref int failed)
    {
        try
        {
            var window = new Views.MainWindow();
            string[] required =
            {
                "Crumb",
                "NavOverview", "NavProfiles", "NavAdvanced", "NavLogs",
                "PageOverview", "PageProfiles", "PageAdvanced", "PageLogs",
            };
            foreach (var name in required)
            {
                if (window.FindControl<Control>(name) is null)
                {
                    Console.WriteLine($"  [FAIL] 主窗口控件树缺 {name}（x:Name 未解析）");
                    failed++;
                }
            }
            // 子 UserControl 有各自的 NameScope，要在子视图上查，不能从窗口直接找
            var profiles = window.FindControl<Control>("PageProfiles");
            if (profiles is null ||
                profiles.FindControl<Control>("PwdAlert") is null ||
                profiles.FindControl<Control>("PwdAlertText") is null)
            {
                Console.WriteLine("  [FAIL] 配置档案视图的密码三态控件缺失");
                failed++;
            }
            window.NavigateTo("logs");
            window.NavigateTo("overview");

            // v0.1.9 布局回归：每个可滚动分区都必须有 PageScroll / PageContent，
            // 由 LayoutHelper 把内容宽度钉到视口；漏掉就会让长文本把右侧内容顶出窗口。
            foreach (var name in new[] { "PageOverview", "PageProfiles", "PageAdvanced" })
            {
                var page = window.FindControl<Control>(name);
                if (page is null ||
                    page.FindControl<ScrollViewer>("PageScroll") is null ||
                    page.FindControl<Control>("PageContent") is null)
                {
                    Console.WriteLine($"  [FAIL] {name} 未按视口宽度约束内容（缺 PageScroll/PageContent）");
                    failed++;
                }
            }

            // v0.1.8 布局回归：日志页只能有一条垂直滚动条，长日志行必须换行。
            // 之前内层 ScrollViewer 固定 300px + 不换行，外层又套一层 page 滚动，
            // 结果长行把星号列撑开、右侧按钮和卡片被切出窗口。
            var logsView = window.FindControl<Control>("PageLogs");
            var logScroll = logsView?.FindControl<ScrollViewer>("LogScroll");
            if (logScroll is not null)
            {
                // 用字符串比较，避免 Avalonia 版本间 ScrollBarVisibility 命名空间差异
                if (!string.Equals(logScroll.HorizontalScrollBarVisibility.ToString(),
                                   "Disabled", StringComparison.Ordinal))
                {
                    Console.WriteLine("  [FAIL] 日志控制台仍允许横向滚动条");
                    failed++;
                }
                var logText = logsView?.FindControl<SelectableTextBlock>("LogTextBlock");
                if (logText is not null && logText.TextWrapping != TextWrapping.Wrap)
                {
                    Console.WriteLine("  [FAIL] 日志长行未换行，会撑破卡片");
                    failed++;
                }
            }
            Console.WriteLine("  [PASS] 主窗口可构造，四分区导航与日志滚动布局无回归");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  [FAIL] 主窗口构造/导航失败：{ex.GetType().Name}: {ex.Message}");
            failed++;
        }
    }

    /// <summary>
    /// 窗口图标：<c>ApplicationIcon</c> 只影响 exe 在资源管理器里的样子，
    /// 标题栏左上角走的是 <c>Window.Icon</c>，需要 ico 以 AvaloniaResource 编进程序集。
    /// 这里验「资源在 + 能解码成 WindowIcon」两件事——少了 csproj 那条就退回系统默认图标。
    /// </summary>
    private static void CheckWindowIcon(ref int failed)
    {
        var uri = new Uri("avares://CampusNet.UI/Assets/app.ico");
        try
        {
            if (!AssetLoader.Exists(uri))
            {
                Console.WriteLine($"  [FAIL] 缺窗口图标资源 {uri}（检查 csproj 的 AvaloniaResource）");
                failed++;
                return;
            }
            using var stream = AssetLoader.Open(uri);
            // 解码一次：文件损坏/不是 ico 时这里会抛，而不是等窗口画出来才发现
            _ = new WindowIcon(stream);
            Console.WriteLine("  [PASS] 窗口图标资源可加载（avares Assets/app.ico）");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  [FAIL] 窗口图标加载失败：{ex.Message}");
            failed++;
        }
    }
}
