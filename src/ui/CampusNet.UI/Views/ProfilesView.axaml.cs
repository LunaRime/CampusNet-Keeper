using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using CampusNet.UI.ViewModels;

namespace CampusNet.UI.Views;

public partial class ProfilesView : UserControl
{
    private MainViewModel? _vm;
    private ProfileViewModel? _profile;

    public ProfilesView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    // 密码三态只由视图层呈现：不新增 ViewModel 字段，也不向守护进程索取明文。
    // HasPassword / PasswordState 变化（保存成功、重新拉配置）都要即时反映到提示条。
    private void OnDataContextChanged(object? sender, System.EventArgs e)
    {
        if (_vm is not null) _vm.PropertyChanged -= OnPropertyChanged;
        _vm = DataContext as MainViewModel;
        if (_vm is not null) _vm.PropertyChanged += OnPropertyChanged;
        AttachProfile();
        UpdateAlert();
    }

    private void AttachProfile()
    {
        if (_profile is not null) _profile.PropertyChanged -= OnPropertyChanged;
        _profile = _vm?.SelectedProfile;
        if (_profile is not null) _profile.PropertyChanged += OnPropertyChanged;
    }

    private void OnPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.SelectedProfile)) AttachProfile();
        UpdateAlert();
    }

    private void UpdateAlert()
    {
        PwdAlert.Classes.Remove("warn");
        PwdAlert.Classes.Remove("bad");

        var p = _profile;
        if (p is null)
        {
            PwdAlertText.Text = "请先选择一个配置档案。";
            return;
        }
        if (p.HasPassword)
        {
            PwdAlertText.Text = "已保存密码：守护进程持有 DPAPI 密文，界面只显示状态；留空即不修改。";
        }
        else if (p.PasswordState == "foreign")
        {
            PwdAlert.Classes.Add("warn");
            PwdAlertText.Text = "密码来自其他电脑 / 账号：本机解不开，界面会原样保留密文、不会删除；请重新输入密码后保存。";
        }
        else
        {
            PwdAlert.Classes.Add("bad");
            PwdAlertText.Text = "当前档案还没有密码：保存后自连会提示「密码为空」，请在密码框输入后再保存。";
        }
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
