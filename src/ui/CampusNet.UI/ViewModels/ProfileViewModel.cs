using CommunityToolkit.Mvvm.ComponentModel;
using CampusNet.UI.Models;

namespace CampusNet.UI.ViewModels;

/// <summary>一份账号档案的可编辑视图（对应 C++ <c>Profile</c>）。</summary>
public partial class ProfileViewModel : ObservableObject
{
    [ObservableProperty] private string _name = "默认";
    [ObservableProperty] private string _authUrl = "";
    [ObservableProperty] private string _username = "";
    [ObservableProperty] private string _vendor = "auto";
    [ObservableProperty] private string _ispPreset = "campus";
    [ObservableProperty] private string _ispCustom = "";

    /// <summary>
    /// 是否已有密码（由守护进程告知，**只读**）。界面据此显示「已保存密码」提示，
    /// 并不去读那份密文——UI 进程不接触已存密码。
    /// </summary>
    [ObservableProperty] private bool _hasPassword;

    /// <summary>用户新输入的密码（明文）。留空表示不改密码，保存时不会覆盖已存密文。</summary>
    [ObservableProperty] private string _passwordInput = "";

    [ObservableProperty] private bool _showPassword;

    /// <summary>是否选中了「自定义…」，选中时展开自定义运营商文本框。</summary>
    [ObservableProperty] private bool _isCustomCarrier;

    public ProfileViewModel() { }

    public ProfileViewModel(Profile p)
    {
        Name = p.Name;
        AuthUrl = p.AuthUrl;
        Username = p.Username;
        Vendor = p.Vendor;
        IspPreset = p.Isp.Preset;
        IspCustom = p.Isp.Custom;
        HasPassword = p.HasPassword;
        IsCustomCarrier = p.Isp.Preset == Services.CarrierOptions.CustomValue;
    }

    /// <summary>导出为模型。<paramref name="includePassword"/> 为 false 时不带新密码（用户没改）。</summary>
    public Profile ToModel(bool includePassword)
    {
        var p = Profile.Create(Name, AuthUrl, Username, Vendor, IspPreset, IspCustom);
        p.HasPassword = HasPassword;
        if (includePassword && PasswordInput.Length > 0) p.PasswordPlain = PasswordInput;
        return p;
    }

    /// <summary>界面上的密码提示文案。</summary>
    public string PasswordHint => HasPassword ? "已保存密码（留空表示不修改）" : "尚未设置密码";
}
