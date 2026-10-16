using Avalonia;

namespace CampusNet.UI;

internal static class Program
{
    // 初始化不要在这里做：Avalonia 需要先完成平台初始化再跑业务逻辑
    //（见 App.OnFrameworkInitializationCompleted）。也刻意不调 WithInterFont()——
    // Inter 不在 DESIGN.md 的字体栈里，字体由 App.axaml 的 CnkFontSans/Mono 决定。
    [STAThread]
    public static void Main(string[] args) =>
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();
}
