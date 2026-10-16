using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Text.Json.Nodes;
using CampusNet.UI.Models;
using CampusNet.UI.Services;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace CampusNet.UI.ViewModels;

/// <summary>认证方式下拉项（对应 C++ 的 <c>vendor</c> 字段）。</summary>
public sealed record VendorOption(string Value, string Label);

/// <summary>设置界面的主视图模型（计划任务 24/25）。</summary>
public partial class MainViewModel : ObservableObject
{
    private readonly IpcClient _ipc;
    private readonly CancellationTokenSource _pollCts = new();

    public ObservableCollection<ProfileViewModel> Profiles { get; } = new();
    public ObservableCollection<CarrierOption> AllCarrierOptions { get; } = new();

    /// <summary>认证方式（与 C++ <c>vendor</c> 取值一致）。</summary>
    public IReadOnlyList<VendorOption> Vendors { get; } = new List<VendorOption>
    {
        new("auto", "自动识别（推荐）"),
        new("drcom", "Dr.COM 哆点"),
        new("eportal", "锐捷 eportal"),
        new("srun", "深澜 srun"),
        new("generic", "通用表单（兜底）"),
        new("custom", "自定义模板"),
    };

    [ObservableProperty] private ProfileViewModel? _selectedProfile;
    [ObservableProperty] private CarrierOption? _selectedCarrier;
    [ObservableProperty] private VendorOption? _selectedVendor;
    [ObservableProperty] private string _statusText = "正在连接守护进程…";
    [ObservableProperty] private string _state = "unknown";
    [ObservableProperty] private bool _daemonRunning;
    [ObservableProperty] private string _localIp = "—";
    [ObservableProperty] private string _lastSuccessText = "从未";
    [ObservableProperty] private bool _showAdvanced;
    [ObservableProperty] private string? _lastError;

    /// <summary>日志面板内容（每 2 秒拉一次 <c>get_log_tail</c>）。</summary>
    [ObservableProperty] private string _logText = "";

    // 高级设置（镜像 C++ Settings）。数值型用 decimal 以便直接绑 NumericUpDown。
    [ObservableProperty] private decimal _probeIntervalSeconds = 5;
    [ObservableProperty] private decimal _probeTimeoutSeconds = 6;
    [ObservableProperty] private string _probeUrl = "http://captive.apple.com/hotspot-detect.html";
    [ObservableProperty] private bool _autostart = true;
    [ObservableProperty] private bool _notifyOnReconnect = true;
    [ObservableProperty] private string _retryBackoffText = "5, 10, 20, 30, 60";
    [ObservableProperty] private string _logLevel = "info";

    public bool IsOnline => State == "online";
    public bool IsBusy => State == "authenticating";
    public bool IsFailed => State is "auth_failed" or "not_campus";

    /// <summary>日志目录（「打开日志目录」按钮用）。</summary>
    public static string LogDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "CampusNet-Keeper", "logs");

    public MainViewModel() : this(new IpcClient()) { }

    public MainViewModel(IpcClient ipc)
    {
        _ipc = ipc;
        _ = RefreshAsync();
        _ = PollLogAsync();
    }

    partial void OnStateChanged(string value)
    {
        OnPropertyChanged(nameof(IsOnline));
        OnPropertyChanged(nameof(IsBusy));
        OnPropertyChanged(nameof(IsFailed));
    }

    partial void OnSelectedProfileChanged(ProfileViewModel? value)
    {
        SetCarrierOptions(CarrierOptions.Merge(null, value?.IspPreset));
        SelectedVendor = Vendors.FirstOrDefault(v => v.Value == value?.Vendor) ?? Vendors[0];
    }

    partial void OnSelectedCarrierChanged(CarrierOption? value)
    {
        if (SelectedProfile is null || value is null) return;
        SelectedProfile.IspPreset = value.Value;
        SelectedProfile.IsCustomCarrier = value.IsCustom;
    }

    partial void OnSelectedVendorChanged(VendorOption? value)
    {
        if (SelectedProfile is null || value is null) return;
        SelectedProfile.Vendor = value.Value;
    }

    [RelayCommand]
    private void TogglePassword()
    {
        // 只切换显示本回合输入的字符：**不去向守护进程索取已存密码**
        //（任务 24 的硬约束：UI 进程不接触已存密码的明文）
        if (SelectedProfile is not null) SelectedProfile.ShowPassword = !SelectedProfile.ShowPassword;
    }

    /// <summary>日志轮询：每 2 秒拉一次尾部日志。守护进程不在时静默跳过。</summary>
    private async Task PollLogAsync()
    {
        while (!_pollCts.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(2), _pollCts.Token).ConfigureAwait(false);
                if (!DaemonRunning) continue;

                var resp = await _ipc.SendAsync("get_log_tail", null, _pollCts.Token)
                                    .ConfigureAwait(false);
                var lines = resp?["data"]?["lines"] as JsonArray;
                if (lines is null) continue;

                LogText = string.Join('\n', lines.Select(l => l?.GetValue<string>() ?? ""));
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (IpcException)
            {
                // 守护进程刚退出等情况：下一轮再说，不在界面刷错误
            }
        }
    }

    [RelayCommand]
    private void OpenLogFolder()
    {
        try
        {
            Directory.CreateDirectory(LogDirectory);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{LogDirectory}\"")
            {
                UseShellExecute = true,
            });
        }
        catch (Exception ex)
        {
            LastError = "打开日志目录失败：" + ex.Message;
        }
    }

    [RelayCommand]
    private async Task RefreshAsync()
    {
        LastError = null;
        try
        {
            DaemonRunning = await _ipc.IsDaemonRunningAsync();
            if (!DaemonRunning)
            {
                State = "offline";
                StatusText = "守护进程未运行";
                LocalIp = "—";
                return;
            }

            var status = await _ipc.SendAsync("get_status");
            if (status?["ok"]?.GetValue<bool>() != true)
            {
                State = "unknown";
                StatusText = status?["error"]?.GetValue<string>() ?? "守护进程无响应";
                return;
            }

            var data = status["data"];
            State = data?["state"]?.GetValue<string>() ?? "unknown";
            StatusText = data?["message"]?.GetValue<string>() ?? StateText(State);
            LocalIp = data?["local_ip"]?.GetValue<string>() ?? "—";
            LastSuccessText = FormatUnix(data?["last_success"]?.GetValue<long>() ?? 0);

            var cfgResp = await _ipc.SendAsync("get_config");
            if (cfgResp?["ok"]?.GetValue<bool>() == true && cfgResp["data"] is JsonNode cfg)
                ApplyConfig(cfg);

            await RefreshCarrierOptionsAsync(SelectedProfile?.IspPreset);
        }
        catch (IpcException ex)
        {
            DaemonRunning = false;
            State = "offline";
            LastError = ex.Message;
        }
    }

    private void ApplyConfig(JsonNode cfg)
    {
        var model = AppConfig.FromJson(cfg.ToJsonString());
        var keep = SelectedProfile?.Name;

        Profiles.Clear();
        foreach (var p in model.Profiles) Profiles.Add(new ProfileViewModel(p));
        if (Profiles.Count == 0) Profiles.Add(new ProfileViewModel());

        ProbeIntervalSeconds = model.Settings.ProbeIntervalSeconds;
        ProbeTimeoutSeconds = model.Settings.ProbeTimeoutSeconds;
        ProbeUrl = model.Settings.ProbeUrl;
        Autostart = model.Settings.Autostart;
        NotifyOnReconnect = model.Settings.NotifyOnReconnect;
        RetryBackoffText = string.Join(", ", model.Settings.RetryBackoffSeconds);
        LogLevel = model.Settings.LogLevel;

        SelectedProfile = Profiles.FirstOrDefault(p => p.Name == keep)
                          ?? Profiles.FirstOrDefault(p => p.Name == model.ActiveProfile)
                          ?? Profiles.FirstOrDefault();
    }

    private async Task RefreshCarrierOptionsAsync(string? current)
    {
        JsonNode? fromServer = null;
        if (DaemonRunning)
        {
            try
            {
                var resp = await _ipc.SendAsync("get_carrier_options");
                // 形态：{"options":[{"id","name","suffix"}], ...}；Merge 也吃裸数组
                if (resp?["ok"]?.GetValue<bool>() == true) fromServer = resp["data"];
            }
            catch (IpcException)
            {
                // 取不到就用内置预设：运营商下拉绝不能因为守护进程的问题而空掉
            }
        }
        SetCarrierOptions(CarrierOptions.Merge(fromServer, current));
    }

    private void SetCarrierOptions(List<CarrierOption> options)
    {
        var keep = SelectedProfile?.IspPreset;
        AllCarrierOptions.Clear();
        foreach (var o in options) AllCarrierOptions.Add(o);
        SelectedCarrier = AllCarrierOptions.FirstOrDefault(o => o.Value == keep)
                          ?? AllCarrierOptions.FirstOrDefault(o => !o.IsCustom);
    }

    [RelayCommand]
    private void AddProfile()
    {
        var vm = new ProfileViewModel { Name = $"档案 {Profiles.Count + 1}" };
        Profiles.Add(vm);
        SelectedProfile = vm;
    }

    [RelayCommand]
    private void RemoveProfile()
    {
        if (SelectedProfile is null || Profiles.Count <= 1) return;
        var idx = Profiles.IndexOf(SelectedProfile);
        Profiles.Remove(SelectedProfile);
        SelectedProfile = Profiles[Math.Max(0, idx - 1)];
    }

    [RelayCommand]
    private async Task SaveAsync()
    {
        LastError = null;
        var cfg = new AppConfig { ActiveProfile = SelectedProfile?.Name ?? "默认" };
        foreach (var vm in Profiles) cfg.Profiles.Add(vm.ToModel(includePassword: true));

        cfg.Settings.ProbeIntervalSeconds = (int)ProbeIntervalSeconds;
        cfg.Settings.ProbeTimeoutSeconds = (int)ProbeTimeoutSeconds;
        cfg.Settings.ProbeUrl = ProbeUrl;
        cfg.Settings.Autostart = Autostart;
        cfg.Settings.NotifyOnReconnect = NotifyOnReconnect;
        cfg.Settings.LogLevel = LogLevel;
        cfg.Settings.RetryBackoffSeconds = ParseBackoff(RetryBackoffText);

        if (!DaemonRunning)
        {
            LastError = "守护进程未运行，配置未保存";
            return;
        }

        try
        {
            // 注意：config 必须是 **JSON 对象**（守护进程侧按对象解析）；不能塞字符串。
            var resp = await _ipc.SendAsync("set_config", new JsonObject
            {
                ["config"] = JsonNode.Parse(cfg.ToJson()),
            });

            if (resp?["ok"]?.GetValue<bool>() == true)
            {
                StatusText = "已保存";
                LastError = null;
                foreach (var vm in Profiles) vm.PasswordInput = "";   // 已交给守护进程，界面不再留存
            }
            else
            {
                LastError = resp?["error"]?.GetValue<string>() ?? "保存失败";
            }
        }
        catch (IpcException ex)
        {
            LastError = ex.Message;
        }
    }

    [RelayCommand]
    private async Task ReconnectNowAsync()
    {
        try
        {
            var resp = await _ipc.SendAsync("reconnect_now");
            StatusText = resp?["ok"]?.GetValue<bool>() == true
                ? "已触发立即重连"
                : resp?["error"]?.GetValue<string>() ?? "重连请求失败";
        }
        catch (IpcException ex)
        {
            LastError = ex.Message;
        }
    }

    private static List<int> ParseBackoff(string text)
    {
        var list = new List<int>();
        foreach (var part in text.Split(',', StringSplitOptions.RemoveEmptyEntries))
            if (int.TryParse(part.Trim(), out var v) && v > 0) list.Add(v);
        return list.Count > 0 ? list : new List<int> { 5, 10, 20, 30, 60 };
    }

    private static string StateText(string state) => state switch
    {
        "online" => "在线",
        "authenticating" => "正在认证…",
        "auth_failed" => "认证失败",
        "not_campus" => "不是校园网",
        "offline" => "离线",
        _ => "未知",
    };

    private static string FormatUnix(long seconds)
    {
        if (seconds <= 0) return "从未";
        return DateTimeOffset.FromUnixTimeSeconds(seconds).ToLocalTime()
            .ToString("yyyy-MM-dd HH:mm:ss");
    }
}
