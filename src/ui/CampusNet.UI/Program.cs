using System.Runtime.InteropServices;
using Avalonia;

namespace CampusNet.UI;

public static class Program
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

    /// <summary>
    /// 任务 26：供原生宿主（CampusNet-Keeper.exe --ui → hostfxr → load_assembly_and_get_function_pointer）
    /// 直接调用的入口。签名必须是 component_entry_point_fn：int (void* arg, int32 arg_size)，
    /// 且方法要标 [UnmanagedCallersOnly]（原生侧对应 UNMANAGEDCALLERSONLY_METHOD）。
    /// </summary>
    [UnmanagedCallersOnly]
    public static int NativeEntry(IntPtr args, int sizeBytes)
    {
        var exitCode = 0;
        // Avalonia 的 Win32 平台要求 STA，而原生调用线程的 COM 单元状态不确定，
        // 所以另起一个 STA 线程跑界面，并在这里阻塞到窗口关闭。
        var uiThread = new Thread(() =>
        {
            try
            {
                BuildAvaloniaApp().StartWithClassicDesktopLifetime(Array.Empty<string>());
            }
            catch (Exception ex)
            {
                exitCode = 2;
                TryLogFatal(ex);
            }
        });
        uiThread.SetApartmentState(ApartmentState.STA);
        uiThread.IsBackground = false;
        uiThread.Start();
        uiThread.Join();

        // 窗口关掉后进程必须真的退出：Avalonia 可能留下消息循环线程把进程挂住
        //（--self-test 就实测过），而这条路径上原生侧已无别的事要做。
        Environment.Exit(exitCode);
        return exitCode;
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();

    // 托管入口抛异常时没有控制台可看（原生子进程是 CREATE_NO_WINDOW 启动的），
    // 所以落到界面载荷目录旁边的日志文件里，便于排障。
    private static void TryLogFatal(Exception ex)
    {
        try
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "CampusNet-Keeper");
            Directory.CreateDirectory(dir);
            File.AppendAllText(
                Path.Combine(dir, "ui-host-error.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} 界面启动失败：{ex}{Environment.NewLine}");
        }
        catch
        {
            // 记日志本身失败也不能再抛（否则异常会跨托管边界，进程直接崩）
        }
    }
}
