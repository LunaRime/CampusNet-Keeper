using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace CampusNet.UI.Views;

public partial class LogsView : UserControl
{
    public LogsView()
    {
        InitializeComponent();
        // 本页根是 Grid（无 PageScroll），帮助方法会安静返回；留着统一入口防以后改回滚动容器
        LayoutHelper.BindContentWidth(this);
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
