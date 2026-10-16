using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using CampusNet.UI.ViewModels;

namespace CampusNet.UI.Views;

public partial class LogPanel : UserControl
{
    public LogPanel()
    {
        InitializeComponent();

        // 新日志到达时自动滚到底部（日志面板的默认期望）
        var block = this.FindControl<SelectableTextBlock>("LogTextBlock");
        var scroll = this.FindControl<ScrollViewer>("LogScroll");
        if (block is not null && scroll is not null)
        {
            block.PropertyChanged += (_, e) =>
            {
                if (e.Property == TextBlock.TextProperty) scroll.ScrollToEnd();
            };
        }
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
