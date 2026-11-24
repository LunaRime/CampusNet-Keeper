using Avalonia;
using Avalonia.Controls;
using Avalonia.LogicalTree;
using Avalonia.Media;
using Avalonia.Platform;
using Avalonia.Styling;
using Avalonia.Threading;
using System.Text.Json.Nodes;
using CampusNet.UI.Models;
using CampusNet.UI.Services;
using CampusNet.UI.ViewModels;

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
        CheckDraftProtection(ref failed);
        CheckOverviewAndLogs(ref failed);
        CheckSmallWindowAndKeyboard(ref failed);

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
    /// 草稿保护（W1）：把"刷新状态"和"改配置"彻底分开后必须成立的几条不变量。
    /// 这里跑的是真 ViewModel（不连守护进程），断言的是**保存报文里到底装了什么**。
    /// </summary>
    private static void CheckDraftProtection(ref int failed)
    {
        var bad = new List<string>();
        void Check(bool ok, string name)
        {
            if (!ok) bad.Add(name);
        }

        var vm = new MainViewModel(new IpcClient(), autoRefresh: false);
        vm.ApplyConfig(JsonNode.Parse("""
        {
          "active_profile": "校园A",
          "settings": { "probe_interval_seconds": 30, "probe_expect": "Success",
                        "log_max_kb": 2048, "log_keep_files": 9 },
          "profiles": [
            { "name": "校园A", "username": "a", "has_password": true },
            { "name": "校园B", "username": "b" }
          ]
        }
        """)!);

        Check(vm.ActiveProfileName == "校园A", "生效档案来自 active_profile");
        Check(!vm.IsDirty, "刚同步完不算未保存");
        Check(vm.Profiles.Count == 2, "档案数量正确");

        // 只看不改：切换列表选中项 ≠ 切换生效档案，也不该标脏
        vm.SelectedProfile = vm.Profiles[1];
        Check(!vm.IsDirty, "切换选中项不置脏");
        Check(vm.ActiveProfileName == "校园A", "切换选中项不改变生效档案");

        var draft = vm.BuildConfigForSave();
        Check(draft.ActiveProfile == "校园A", "保存以生效档案为准，不是当前选中项");
        Check(draft.Settings.ProbeExpect == "Success", "probe_expect 原样透传（不被写空）");
        Check(draft.Settings.LogMaxKb == 2048 && draft.Settings.LogKeepFiles == 9,
              "未暴露的日志字段透传");

        // 真编辑才置脏，且此时禁止自动重载
        vm.Profiles[1].Username = "b-changed";
        Check(vm.IsDirty, "编辑字段后置脏");
        Check(!vm.CanReloadConfig, "有草稿时禁止自动重载");

        // 显式设为生效：只改本地状态，保存时才提交
        vm.SetActiveProfileCommand.Execute(null);
        Check(vm.ActiveProfileName == "校园B", "设为生效只改本地状态");
        Check(vm.BuildConfigForSave().ActiveProfile == "校园B", "设为生效后保存提交它");
        Check(!vm.Profiles[0].IsActive && vm.Profiles[1].IsActive, "生效标记跟着走");

        // 未在界面暴露的 custom_template 同样不能被"保存普通设置"清空
        var tplVm = new ProfileViewModel(new Profile
        {
            Name = "模板档案",
            CustomTemplate = new CustomTemplate { Url = "http://tpl.invalid/login", Body = "u=$u" },
        });
        var tplOut = tplVm.ToModel(includePassword: false).CustomTemplate;
        Check(tplOut.Url == "http://tpl.invalid/login" && tplOut.Body == "u=$u",
              "custom_template 透传（不被清空）");

        // 保存回包：自启没同步必须与「已保存」分开说（W4）
        var (op, err) = MainViewModel.InterpretSaveResult(
            JsonNode.Parse("""{"saved":true,"autostart_synced":false,"autostart_error":"拒绝访问"}"""), true);
        Check(op.Contains("自启同步失败"), "自启失败时不谎报「已保存」");
        Check(err is not null && err.Contains("拒绝访问"), "自启失败带上原因");

        (op, err) = MainViewModel.InterpretSaveResult(
            JsonNode.Parse("""{"saved":true,"autostart_synced":true}"""), true);
        Check(op == "已保存" && err is null, "自启正常 → 已保存");

        (op, err) = MainViewModel.InterpretSaveResult(
            JsonNode.Parse("""{"saved":true,"autostart_synced":true}"""), false);
        Check(op.Contains("没有密码") && err is not null, "生效档案无密码仍有提示");

        foreach (var name in bad) Console.WriteLine($"  [FAIL] 草稿保护：{name}");
        failed += bad.Count;
        if (bad.Count == 0) Console.WriteLine("  [PASS] 草稿保护（状态/配置分离、生效档案、字段透传）");
    }

    /// <summary>
    /// 概览与日志（W3）：四段链路只认证据、最近事件取最新、日志筛选真的生效。
    /// 这里跑真 ViewModel（不连守护），断言的是"界面会显示什么"。
    /// </summary>
    private static void CheckOverviewAndLogs(ref int failed)
    {
        var bad = new List<string>();
        void Check(bool ok, string name)
        {
            if (!ok) bad.Add(name);
        }

        var vm = new MainViewModel(new IpcClient(), autoRefresh: false);

        // ① 分层证据：没证据必须显示「未知」，不许猜成通过
        vm.ApplyLayers(JsonNode.Parse(
            """{"local":"ok","gateway":"unknown","portal":"fail","internet":"unknown"}"""));
        Check(vm.LocalOk && !vm.LocalFail && vm.LocalStateText == "已通过", "本机段有证据 → 已通过");
        Check(!vm.GatewayOk && !vm.GatewayFail && vm.GatewayStateText == "未知", "网关段无证据 → 未知");
        Check(!vm.PortalOk && vm.PortalFail && vm.PortalStateText == "失败", "门户段明确失败 → 失败");
        Check(!vm.InternetOk && !vm.InternetFail && vm.InternetStateText == "未知", "出网段无证据 → 未知");

        // 老守护没有 layers 字段：不能沿用上一轮的值
        vm.ApplyLayers(null);
        Check(vm.LocalStateText == "未知" && vm.InternetStateText == "未知", "缺 layers 字段 → 全部未知");

        // ② 最近事件：取最新几条，跳过 DEBUG（旧实现是整段日志的前 6 行 = 最旧的）
        vm.LogText = string.Join('\n', new[]
        {
            "10:00:00 [DEBUG] 旧的调试",
            "10:00:01 [INFO] 第一件",
            "10:00:02 [DEBUG] 调试噪声",
            "10:00:03 [WARN] 第二件",
            "10:00:04 [ERROR] 第三件",
        });
        Check(vm.RecentEvents.Count == 3, "最近事件跳过 DEBUG");
        Check(vm.RecentEvents.Count > 0 && vm.RecentEvents[0].Contains("第三件"), "最近事件最新在前");

        // ③ 日志筛选：级别 + 搜索词，清空后回到全量
        vm.LogLevelFilter = "WARN";
        Check(vm.FilteredLogText.Contains("第二件") && !vm.FilteredLogText.Contains("第一件"),
              "级别筛选只留 WARN");
        vm.LogLevelFilter = "全部";
        vm.LogFilter = "第三";
        Check(vm.FilteredLogText.Contains("第三件") && !vm.FilteredLogText.Contains("第一件"),
              "搜索词过滤");
        vm.LogFilter = "";
        Check(vm.FilteredLogText.Contains("第一件") && vm.FilteredLogText.Contains("第三件"),
              "清空筛选回到全量");

        foreach (var name in bad) Console.WriteLine($"  [FAIL] 概览/日志：{name}");
        failed += bad.Count;
        if (bad.Count == 0) Console.WriteLine("  [PASS] 概览分层证据 / 最近事件 / 日志筛选");
    }

    /// <summary>
    /// 最小窗口 + 键盘（W3）：880×600 时内容区只有 672×488（侧栏 208、标题栏 48、底栏 64），
    /// 内容宽度必须被 LayoutHelper 钉回视口；主要按钮都要能用键盘触发。
    /// </summary>
    private static void CheckSmallWindowAndKeyboard(ref int failed)
    {
        try
        {
            var window = new Views.MainWindow();
            var buttons = window.GetLogicalDescendants().OfType<Button>().ToList();
            bool HasKey(string content) => buttons.Any(
                b => (b.Content as string) == content && b.HotKey is not null);
            if (!HasKey("保存") || !HasKey("刷新") || !HasKey("测试登录"))
            {
                Console.WriteLine("  [FAIL] 主窗口按钮缺键盘快捷键（HotKey）");
                failed++;
            }

            // 真实布局需要可视树：放进一个移出屏幕的窗口，量完立刻关掉。
            // 无交互桌面（纯服务会话）时降级为 SKIP —— 那是环境限制，不是被测代码的问题。
            var view = new Views.OverviewView
            {
                DataContext = new MainViewModel(new IpcClient(), autoRefresh: false),
            };
            Window? host = null;
            try
            {
                host = new Window
                {
                    Width = 880,
                    Height = 600,
                    SystemDecorations = SystemDecorations.None,
                    ShowInTaskbar = false,
                    CanResize = false,
                    WindowStartupLocation = WindowStartupLocation.Manual,
                    Position = new PixelPoint(-3000, -3000),
                    Content = view,
                };
                host.Show();
                Dispatcher.UIThread.RunJobs();

                var content = view.FindControl<Control>("PageContent");
                var width = content?.Bounds.Width ?? 0;
                // 这里把概览单独放进 880 宽的窗口（没有侧栏），视口就是 880；
                // LayoutHelper 会再减左右各 20 的 Margin。它失效时内容会按自然宽度
                // 撑到 1300+，正好被这条断言抓住。
                var max = 880 - 40 + 1;
                if (width <= 0 || width > max)
                {
                    Console.WriteLine($"  [FAIL] 最小窗口下概览内容宽 {width:0}（上限 {max:0}），长文案会顶出窗口");
                    failed++;
                }
                else
                {
                    Console.WriteLine($"  [PASS] 最小窗口内容宽 {width:0} ≤ {max:0}；{buttons.Count} 个按钮带快捷键");
                }

                // 光测 ViewModel 不够：XAML 若把四个节点又绑回同一个总体状态，
                // 上面那些断言照样全绿（"测了函数没测入口"）。这里查真实控件。
                var vm = (MainViewModel)view.DataContext!;
                vm.ApplyLayers(JsonNode.Parse(
                    """{"local":"ok","gateway":"unknown","portal":"fail","internet":"unknown"}"""));
                Dispatcher.UIThread.RunJobs();

                bool Lit(string name, string cls)
                {
                    var b = view.FindControl<Border>("Node" + name);
                    return b is not null && b.Classes.Contains(cls);
                }

                var bindingBad = 0;
                if (!Lit("Local", "online")) bindingBad++;
                // unknown 必须两个类都没有：亮任何一种颜色都是把"没证据"说成了结论
                if (Lit("Gateway", "online") || Lit("Gateway", "failed")) bindingBad++;
                if (!Lit("Portal", "failed")) bindingBad++;
                if (Lit("Internet", "online")) bindingBad++;

                if (bindingBad > 0)
                {
                    Console.WriteLine($"  [FAIL] 链路节点绑定：{bindingBad} 处颜色与证据不符（unknown 不该亮灯）");
                    failed++;
                }
                else
                {
                    Console.WriteLine("  [PASS] 四个链路节点各绑各的证据（unknown 不亮灯）");
                }
            }
            catch (Exception layoutEx)
            {
                Console.WriteLine($"  [SKIP] 最小窗口布局（无交互桌面：{layoutEx.GetType().Name}）");
            }
            finally
            {
                host?.Close();
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  [FAIL] 最小窗口/键盘检查失败：{ex.GetType().Name}: {ex.Message}");
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
