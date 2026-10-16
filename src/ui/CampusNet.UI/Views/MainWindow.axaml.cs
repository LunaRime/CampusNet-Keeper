using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using CampusNet.UI.Services;

namespace CampusNet.UI.Views;

public partial class MainWindow : Window
{
    private readonly IpcClient _client = new();

    public MainWindow()
    {
        InitializeComponent();

        var refresh = this.FindControl<Button>("RefreshButton");
        if (refresh is not null) refresh.Click += async (_, _) => await RefreshStatusAsync();

        var close = this.FindControl<Button>("CloseButton");
        if (close is not null) close.Click += (_, _) => Close();

        // 窗口一出来就问一次守护进程：外壳 ↔ IPC 的链路因此可见（不必等任务 24）
        Opened += async (_, _) => await RefreshStatusAsync();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private async Task RefreshStatusAsync()
    {
        var text = this.FindControl<TextBlock>("StatusText");
        if (text is null) return;

        text.Text = "正在连接守护进程…";
        var running = await _client.IsDaemonRunningAsync();
        text.Text = running
            ? $"已连接守护进程（管道 {_client.PipeName}）"
            : "未连接守护进程：请先启动 CampusNet-Keeper.exe（守护模式）";
    }
}
