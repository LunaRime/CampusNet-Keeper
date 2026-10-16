using Avalonia.Controls;
using Avalonia.Markup.Xaml;
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
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
