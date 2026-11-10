using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Styling;
using CampusNet.UI.Services;
using CampusNet.UI.ViewModels;

namespace CampusNet.UI.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        // 视图模型只依赖 IpcClient（可测核心），界面保持薄
        DataContext = new MainViewModel(new IpcClient());
        NavigateTo("overview");
    }

    /// <summary>
    /// 分区导航：纯视图层状态。不新增 ViewModel 字段，不改 IPC 契约。
    /// 子视图（例如概览的「查看全部日志」）也走这里。
    /// </summary>
    public void NavigateTo(string tag)
    {
        Crumb.Text = (tag switch
        {
            "profiles" => "配置档案",
            "advanced" => "高级设置",
            "logs" => "运行日志",
            _ => "概览",
        }) + " · 设置";

        SetPage(NavOverview, PageOverview, tag == "overview");
        SetPage(NavProfiles, PageProfiles, tag == "profiles");
        SetPage(NavAdvanced, PageAdvanced, tag == "advanced");
        SetPage(NavLogs, PageLogs, tag == "logs");
    }

    private static void SetPage(Button nav, Control view, bool active)
    {
        view.IsVisible = active;
        if (active)
        {
            if (!nav.Classes.Contains("active")) nav.Classes.Add("active");
        }
        else
        {
            nav.Classes.Remove("active");
        }
    }

    private void Nav_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is Button b && b.Tag is string tag) NavigateTo(tag);
    }

    private void Theme_Click(object? sender, RoutedEventArgs e)
    {
        var app = Application.Current;
        if (app is null) return;
        app.RequestedThemeVariant = app.RequestedThemeVariant == ThemeVariant.Dark
            ? ThemeVariant.Light
            : ThemeVariant.Dark;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
