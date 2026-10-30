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

    /// <summary>
    /// 只读三态（守护进程给的）：ok / foreign / none。foreign = 密文存在但本机解不开，
    /// 通常是配置从别的电脑或别的 Windows 账号带过来的。
    /// </summary>
    [ObservableProperty] private string _passwordState = "none";

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
        PasswordState = p.PasswordState;
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
    public string PasswordHint =>
        HasPassword ? "已保存密码（留空表示不修改）"
        : PasswordState == "foreign" ? "密码来自其他电脑/账号，需在本机重新输入"
        : "尚未设置密码";

    // HasPassword / PasswordState 变了（保存成功、或从守护进程重新拉到配置）提示语必须跟着变，
    // 否则保存后水印仍是「尚未设置密码」，看起来就像密码没存上。
    partial void OnHasPasswordChanged(bool value) => OnPropertyChanged(nameof(PasswordHint));
    partial void OnPasswordStateChanged(string value) => OnPropertyChanged(nameof(PasswordHint));
}
