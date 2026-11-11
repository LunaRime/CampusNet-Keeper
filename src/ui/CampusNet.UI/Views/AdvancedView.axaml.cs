using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;

namespace CampusNet.UI.Views;

public partial class AdvancedView : UserControl
{
    public AdvancedView()
    {
        InitializeComponent();
        LayoutHelper.BindContentWidth(this);
    }

    // 口令显示切换只动视图层控件，不新增命令、不把明文写回 ViewModel 之外的地方
    private void ToggleSecret_Click(object? sender, RoutedEventArgs e)
    {
        var box = this.FindControl<TextBox>("SecretPassphraseBox");
        if (box is null) return;
        var revealing = box.PasswordChar == '\0';
        box.PasswordChar = revealing ? '●' : '\0';
        if (sender is Button b) b.Content = revealing ? "显示" : "隐藏";
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
