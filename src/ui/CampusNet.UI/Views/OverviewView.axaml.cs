using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.VisualTree;

namespace CampusNet.UI.Views;

public partial class OverviewView : UserControl
{
    public OverviewView() => InitializeComponent();

    // 跨分区导航只经 MainWindow 的纯视图层入口，不新增 ViewModel 状态
    private void GoLogs_Click(object? sender, RoutedEventArgs e)
        => this.FindAncestorOfType<MainWindow>()?.NavigateTo("logs");

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
