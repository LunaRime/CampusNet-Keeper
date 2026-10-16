using Avalonia;

namespace CampusNet.UI;

internal static class Program
{
    // 初始化不要在这里做：Avalonia 需要先完成平台初始化再跑业务逻辑
    //（见 App.OnFrameworkInitializationCompleted）。也刻意不调 WithInterFont()——
    // Inter 不在 DESIGN.md 的字体栈里，字体由 App.axaml 的 CnkFontSans/Mono 决定。
    [STAThread]
    public static int Main(string[] args)
    {
        // 不显示窗口的自检（设计令牌层）：可无人值守运行，退出码 0 = 全过。
        // ⚠ 必须 Environment.Exit：Avalonia 初始化 Win32 平台会留下消息循环线程，
        // Main 返回后进程仍不退出（实测会一直挂着）。
        if (Array.Exists(args, a => a == "--self-test"))
        {
            var code = SelfTest.Run();
            Environment.Exit(code);
            return code;
        }

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();
}
