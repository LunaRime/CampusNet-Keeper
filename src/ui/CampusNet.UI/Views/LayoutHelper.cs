using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;

namespace CampusNet.UI.Views;

/// <summary>
/// 把分区内容宽度钉到 ScrollViewer 视口，防止长文本 / 长水印把内容撑出窗口右侧。
///
/// 背景：Avalonia 在 HorizontalScrollBarVisibility=Disabled 时不会把 Content 压回视口宽度，
/// 子项按自然宽度测量后整块被 Arrange 到超宽位置，右边直接被裁（v0.1.8 实机截图：
/// 「显示」按钮只剩半边、alert 文字被切、右侧滚动条被推出窗口）。
/// Padding 由 ScrollViewer 移到内容 Margin，避免二次留白；40 = 左右各 20 的 Margin。
/// </summary>
internal static class LayoutHelper
{
    public static void BindContentWidth(Control root)
    {
        var scroll = root.FindControl<ScrollViewer>("PageScroll");
        var content = root.FindControl<Control>("PageContent");
        if (scroll is null || content is null) return;

        void Update()
        {
            var width = scroll.Viewport.Width;
            if (width <= 0) width = scroll.Bounds.Width;
            if (width <= 0) return;
            content.Width = Math.Max(0, width - 40);
        }

        // SizeChanged 时 Viewport 可能尚未刷新，延后一帧再取一次
        scroll.SizeChanged += (_, _) => Dispatcher.UIThread.Post(Update);
        Dispatcher.UIThread.Post(Update);
    }
}
