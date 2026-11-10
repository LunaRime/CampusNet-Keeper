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
    ///
    /// ⚠ 本工程的 XAML 是 AvaloniaXamlLoader.Load(this) 运行时加载，x:Name **不会**
    /// 自动给同名字段赋值（字段声明存在但为 null）。一律用 FindControl 取控件，
    /// 并容忍取不到——绝不让「窗口构造时空引用」把整个界面打挂（v0.1.7 事故）。
    /// </summary>
    public void NavigateTo(string tag)
    {
        var crumb = this.FindControl<TextBlock>("Crumb");
        if (crumb is not null)
        {
            crumb.Text = (tag switch
            {
                "profiles" => "配置档案",
                "advanced" => "高级设置",
                "logs" => "运行日志",
                _ => "概览",
            }) + " · 设置";
        }

        SetPage("NavOverview", "PageOverview", tag == "overview");
        SetPage("NavProfiles", "PageProfiles", tag == "profiles");
        SetPage("NavAdvanced", "PageAdvanced", tag == "advanced");
        SetPage("NavLogs", "PageLogs", tag == "logs");
    }

    private void SetPage(string navName, string viewName, bool active)
    {
        var view = this.FindControl<Control>(viewName);
        if (view is not null) view.IsVisible = active;

        var nav = this.FindControl<Button>(navName);
        if (nav is null) return;
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
