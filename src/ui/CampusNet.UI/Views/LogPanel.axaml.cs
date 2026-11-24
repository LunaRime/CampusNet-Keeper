using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using CampusNet.UI.ViewModels;

namespace CampusNet.UI.Views;

public partial class LogPanel : UserControl
{
    public LogPanel()
    {
        InitializeComponent();

        var block = this.FindControl<SelectableTextBlock>("LogTextBlock");
        var scroll = this.FindControl<ScrollViewer>("LogScroll");
        if (block is not null && scroll is not null)
        {
            block.PropertyChanged += (_, e) =>
            {
                if (e.Property != TextBlock.TextProperty) return;
                // 「跟随最新」关掉时不要抢滚动位置：用户正在往回翻（W3）。
                // 旧实现无条件 ScrollToEnd()，往回翻两行就被拽回底部。
                if (DataContext is MainViewModel { LogFollow: false }) return;
                scroll.ScrollToEnd();
            };
        }
    }

    /// <summary>复制当前筛选结果（不是被过滤后的全量）。</summary>
    private async void Copy_Click(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainViewModel vm) return;
        var top = TopLevel.GetTopLevel(this);
        if (top?.Clipboard is null) return;
        await top.Clipboard.SetTextAsync(vm.FilteredLogText);
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
