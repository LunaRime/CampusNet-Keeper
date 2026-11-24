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

    /// <summary>
    /// 当前**生效**的档案名（守护进程按它自连）。与「正在编辑」是两件事（W1）：
    /// 切换列表选中项只改 <see cref="SelectedProfile"/>，只有「设为生效」才动这里，
    /// 保存时才作为 <c>active_profile</c> 提交。
    /// </summary>
    [ObservableProperty] private string _activeProfileName = "默认";

    /// <summary>
    /// 有未保存草稿。刷新状态 / 重连 / 导入密码都不许把草稿冲掉（W1）——
    /// 只有保存成功或用户显式重载才清。
    /// </summary>
    [ObservableProperty] private bool _isDirty;

    /// <summary>草稿未保存时不允许自动重载配置（避免"点一下刷新，改的东西没了"）。</summary>
    public bool CanReloadConfig => !IsDirty;

    /// <summary>
    /// 服务器下发的 Settings 基线。保存时只覆盖界面暴露的字段，其余（probe_expect /
    /// log_max_kb / log_keep_files）原样透传，不能拿默认值顶掉用户的自定义。
    /// </summary>
    private Settings? _serverSettings;

    /// <summary>ApplyConfig 期间抑制脏标记：程序性赋值不是用户编辑。</summary>
    private bool _loading;

    /// <summary>是否已从守护进程加载过配置（刷新状态时据此决定要不要补一次重载）。</summary>
    private bool _configLoaded;

    [ObservableProperty] private CarrierOption? _selectedCarrier;
    [ObservableProperty] private VendorOption? _selectedVendor;
    [ObservableProperty] private string _statusText = "正在连接守护进程…";
    [ObservableProperty] private string _state = "unknown";
    [ObservableProperty] private bool _daemonRunning;
    [ObservableProperty] private string _localIp = "—";
    /// <summary>网关 + 它的 ARP 状态。与「本机 IP」一起才能区分「网关不通」和「认证坏」。</summary>
    [ObservableProperty] private string _gatewayText = "—";
    [ObservableProperty] private string _lastSuccessText = "从未";
    [ObservableProperty] private bool _showAdvanced;
    [ObservableProperty] private string? _lastError;

    /// <summary>
    /// 用户操作的反馈（保存 / 测试 / 重连 / 导入导出）。与 <see cref="StatusText"/> 分开：
    /// 后者是**网络状态**（来自守护），混在一起会让人分不清"操作成功了"和"网络好了"（W3）。
    /// </summary>
    [ObservableProperty] private string _operationResult = "";

    /// <summary>守护进程连接状态（IPC 层面），与网络在线状态是两件事。</summary>
    [ObservableProperty] private string _daemonStatusText = "未连接";

    // ---- 四段链路（W3）：每段只认本轮证据，没证据就是「未知」 ----
    [ObservableProperty] private bool _localOk;
    [ObservableProperty] private bool _localFail;
    [ObservableProperty] private string _localStateText = "未知";
    [ObservableProperty] private bool _gatewayOk;
    [ObservableProperty] private bool _gatewayFail;
    [ObservableProperty] private string _gatewayStateText = "未知";
    [ObservableProperty] private bool _portalOk;
    [ObservableProperty] private bool _portalFail;
    [ObservableProperty] private string _portalStateText = "未知";
    [ObservableProperty] private bool _internetOk;
    [ObservableProperty] private bool _internetFail;
    [ObservableProperty] private string _internetStateText = "未知";

    /// <summary>日志面板：搜索词 / 级别筛选 / 是否跟随最新。</summary>
    [ObservableProperty] private string _logFilter = "";
    [ObservableProperty] private string _logLevelFilter = "全部";
    [ObservableProperty] private bool _logFollow = true;
    /// <summary>守护进程因字节预算只给了部分日志（界面据此提示，而不是假装就这么多）。</summary>
    [ObservableProperty] private bool _logTruncated;

    public IReadOnlyList<string> LogLevels { get; } = new[] { "全部", "INFO", "WARN", "ERROR", "DEBUG" };

    /// <summary>概览「最近事件」：最新几条非 DEBUG 的日志（不是整段日志的前几行）。</summary>
    public ObservableCollection<string> RecentEvents { get; } = new();

    /// <summary>过滤后的日志文本（搜索 + 级别）。原始 <see cref="LogText"/> 不动，随时可还原。</summary>
    public string FilteredLogText => BuildFilteredLog();

    /// <summary>日志面板内容（每 2 秒拉一次 <c>get_log_tail</c>）。</summary>
    [ObservableProperty] private string _logText = "";

    // 高级设置（镜像 C++ Settings）。数值型用 decimal 以便直接绑 NumericUpDown。
    // 默认值与 C++ 守护的 Settings 保持一致（2026-11-10 迁移 router 端实证）：
    // 30 秒间隔 + generate_204；204 仍须走 AC 复核 + 出网验证。
    [ObservableProperty] private decimal _probeIntervalSeconds = 30;
    [ObservableProperty] private decimal _probeTimeoutSeconds = 6;
    [ObservableProperty] private string _probeUrl = "http://connectivitycheck.platform.hicloud.com/generate_204";
    [ObservableProperty] private bool _autostart = true;
    [ObservableProperty] private bool _notifyOnReconnect = true;
    [ObservableProperty] private string _retryBackoffText = "5, 10, 20, 30, 60";
    [ObservableProperty] private string _logLevel = "info";

    /// <summary>密码迁移：导出/导入文件路径（用户可改，默认落在「文档」里）。</summary>
    [ObservableProperty] private string _secretPath = DefaultSecretPath();

    /// <summary>密码迁移口令：只在界面内存里停留，不落盘、不进日志。</summary>
    [ObservableProperty] private string _secretPassphrase = "";

    private static string DefaultSecretPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
        "campusnet-secret.json");

    public bool IsOnline => State == "online";
    public bool IsBusy => State == "authenticating";
    public bool IsFailed => State is "auth_failed" or "not_campus";

    /// <summary>日志目录（「打开日志目录」按钮用）。</summary>
    public static string LogDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "CampusNet-Keeper", "logs");

    public MainViewModel() : this(new IpcClient()) { }

    /// <param name="autoRefresh">
    /// false = 只做纯逻辑（自检用）：不连守护进程、不起日志轮询。
    /// </param>
    public MainViewModel(IpcClient ipc, bool autoRefresh = true)
    {
        _ipc = ipc;
        if (!autoRefresh) return;
        _ = InitializeAsync();
        _ = PollAsync();
    }

    /// <summary>启动：先刷状态，再拉一次配置（此时草稿必然是干净的）。</summary>
    private async Task InitializeAsync()
    {
        await RefreshStatusAsync();
        await ReloadConfigAsync();
    }

    partial void OnStateChanged(string value)
    {
        OnPropertyChanged(nameof(IsOnline));
        OnPropertyChanged(nameof(IsBusy));
        OnPropertyChanged(nameof(IsFailed));
    }

    partial void OnDaemonRunningChanged(bool value) =>
        DaemonStatusText = value ? "已连接" : "守护进程未运行";

    partial void OnLogTextChanged(string value)
    {
        RebuildRecentEvents();
        OnPropertyChanged(nameof(FilteredLogText));
    }

    partial void OnLogFilterChanged(string value) => OnPropertyChanged(nameof(FilteredLogText));
    partial void OnLogLevelFilterChanged(string value) => OnPropertyChanged(nameof(FilteredLogText));

    /// <summary>
    /// 「最近事件」= 最新的几条非 DEBUG 日志。旧实现直接把整段日志的前 6 行塞进概览，
    /// 看到的是**最旧**的调试输出，跟"最近"完全相反（W3）。
    /// </summary>
    private void RebuildRecentEvents()
    {
        RecentEvents.Clear();
        var lines = LogText.Split('\n');
        var picked = new List<string>();
        for (var i = lines.Length - 1; i >= 0 && picked.Count < 4; --i)
        {
            var line = lines[i].TrimEnd();
            if (line.Length == 0 || line.Contains("[DEBUG]", StringComparison.Ordinal)) continue;
            picked.Add(line);
        }
        foreach (var line in picked) RecentEvents.Add(line);   // 最新在前
    }

    /// <summary>日志过滤：级别 + 搜索词（大小写不敏感）。原始 LogText 不动。</summary>
    private string BuildFilteredLog()
    {
        var text = LogFilter?.Trim() ?? "";
        var level = LogLevelFilter;
        var useLevel = !string.IsNullOrEmpty(level) && level != "全部";
        var tag = "[" + level + "]";

        var sb = new System.Text.StringBuilder();
        foreach (var line in LogText.Split('\n'))
        {
            if (useLevel && !line.Contains(tag, StringComparison.Ordinal)) continue;
            if (text.Length > 0 && !line.Contains(text, StringComparison.OrdinalIgnoreCase)) continue;
            sb.Append(line).Append('\n');
        }
        return sb.ToString().TrimEnd('\n');
    }

    /// <summary>四段链路：ok / fail / unknown，只有 ok 才亮绿灯。internal 供自检直接断言。</summary>
    internal void ApplyLayers(JsonNode? layers)
    {
        static (bool Ok, bool Fail, string Text) Parse(JsonNode? n) =>
            (n?.GetValue<string>() ?? "unknown") switch
            {
                "ok" => (true, false, "已通过"),
                "fail" => (false, true, "失败"),
                _ => (false, false, "未知"),
            };

        var (ok, fail, text) = Parse(layers?["local"]);
        LocalOk = ok; LocalFail = fail; LocalStateText = text;
        (ok, fail, text) = Parse(layers?["gateway"]);
        GatewayOk = ok; GatewayFail = fail; GatewayStateText = text;
        (ok, fail, text) = Parse(layers?["portal"]);
        PortalOk = ok; PortalFail = fail; PortalStateText = text;
        (ok, fail, text) = Parse(layers?["internet"]);
        InternetOk = ok; InternetFail = fail; InternetStateText = text;
    }

    partial void OnSelectedProfileChanged(ProfileViewModel? value)
    {
        SetCarrierOptions(CarrierOptions.Merge(null, value?.IspPreset));
        SelectedVendor = Vendors.FirstOrDefault(v => v.Value == value?.Vendor) ?? Vendors[0];
    }

    partial void OnSelectedCarrierChanged(CarrierOption? value)
    {
        if (SelectedProfile is null || value is null) return;
        // 只是把下拉框同步到当前档案的值，不算用户编辑（否则切换档案就标脏）
        if (SelectedProfile.IspPreset == value.Value &&
            SelectedProfile.IsCustomCarrier == value.IsCustom) return;
        SelectedProfile.IspPreset = value.Value;
        SelectedProfile.IsCustomCarrier = value.IsCustom;
    }

    partial void OnSelectedVendorChanged(VendorOption? value)
    {
        if (SelectedProfile is null || value is null) return;
        if (SelectedProfile.Vendor == value.Value) return;   // 同上：同步不算编辑
        SelectedProfile.Vendor = value.Value;
    }

    // ---- 草稿脏标记（W1）----
    // 任何「用户能改的字段」变化都置脏；密码状态、显示开关这类只读/展示状态不算。
    partial void OnProbeIntervalSecondsChanged(decimal value) => MarkDirty();
    partial void OnProbeTimeoutSecondsChanged(decimal value) => MarkDirty();
    partial void OnProbeUrlChanged(string value) => MarkDirty();
    partial void OnAutostartChanged(bool value) => MarkDirty();
    partial void OnNotifyOnReconnectChanged(bool value) => MarkDirty();
    partial void OnRetryBackoffTextChanged(string value) => MarkDirty();
    partial void OnLogLevelChanged(string value) => MarkDirty();

    partial void OnIsDirtyChanged(bool value) => OnPropertyChanged(nameof(CanReloadConfig));

    partial void OnActiveProfileNameChanged(string value) => SyncActiveFlags();

    private void MarkDirty()
    {
        if (_loading) return;
        if (!IsDirty) IsDirty = true;
    }

    private void SyncActiveFlags()
    {
        foreach (var p in Profiles) p.IsActive = p.Name == ActiveProfileName;
    }

    private void OnProfileDraftChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        // 这四项不是"用户编辑"：密码状态由守护进程回传，ShowPassword 是显示开关，
        // IsActive 是本地派生的标记。
        if (e.PropertyName is nameof(ProfileViewModel.HasPassword)
            or nameof(ProfileViewModel.PasswordState)
            or nameof(ProfileViewModel.ShowPassword)
            or nameof(ProfileViewModel.IsActive))
            return;

        MarkDirty();
        if (e.PropertyName == nameof(ProfileViewModel.Name)) SyncActiveFlags();
    }

    private void AddTracked(ProfileViewModel vm)
    {
        vm.PropertyChanged += OnProfileDraftChanged;
        vm.IsActive = vm.Name == ActiveProfileName;
        Profiles.Add(vm);
    }

    [RelayCommand]
    private void TogglePassword()
    {
        // 只切换显示本回合输入的字符：**不去向守护进程索取已存密码**
        //（任务 24 的硬约束：UI 进程不接触已存密码的明文）
        if (SelectedProfile is not null) SelectedProfile.ShowPassword = !SelectedProfile.ShowPassword;
    }

    /// <summary>
    /// 每 2 秒一次的后台轮询：拉日志；守护不在时**主动重连**（W3 断线恢复）。
    ///
    /// ⚠ 全程在 UI 线程恢复（不要 ConfigureAwait(false)）：LogText / State 都是绑定源，
    /// 从线程池线程改会踩 Avalonia 的线程检查（旧实现就是这么写的，日志偶发不刷新）。
    /// 重连后只在「没有草稿」时补一次配置加载，绝不覆盖用户正在改的东西。
    /// </summary>
    private async Task PollAsync()
    {
        while (!_pollCts.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(2), _pollCts.Token);
                if (!DaemonRunning)
                {
                    // 守护可能刚重启：先问一句，恢复后刷状态 + 补配置（有草稿会自动跳过）
                    if (await _ipc.IsDaemonRunningAsync())
                    {
                        await RefreshStatusAsync();
                        await ReloadConfigAsync();
                    }
                    continue;
                }

                var resp = await _ipc.SendAsync("get_log_tail", null, _pollCts.Token);
                if (resp?["ok"]?.GetValue<bool>() == true)
                {
                    var data = resp["data"];
                    if (data?["lines"] is JsonArray lines)
                        LogText = string.Join('\n', lines.Select(l => l?.GetValue<string>() ?? ""));
                    LogTruncated = data?["truncated"]?.GetValue<bool>() == true;
                }
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (IpcException)
            {
                // 一次失败就标记断线：下一轮走恢复分支，别让界面永远停在「已连接」
                DaemonRunning = false;
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

    /// <summary>
    /// 「刷新」只刷状态：进程是否在跑、在线状态、本机 IP、网关/ARP、最近成功时间。
    /// **不碰配置** —— 旧实现顺带 ApplyConfig，把用户正在编辑的档案和设置全清掉（W1）。
    /// </summary>
    [RelayCommand]
    private async Task RefreshAsync() => await RefreshStatusAsync();

    private async Task RefreshStatusAsync()
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
                GatewayText = "—";
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
            // 四段链路各自的状态（W3）：没有 layers 字段时全部显示「未知」，绝不猜
            ApplyLayers(data?["layers"]);
            LocalIp = OrDash(data?["local_ip"]?.GetValue<string>());
            GatewayText = FormatGateway(data?["gateway"]?.GetValue<string>(),
                                        data?["gateway_arp"]?.GetValue<string>());
            LastSuccessText = FormatUnix(data?["last_success"]?.GetValue<long>() ?? 0);

            // 运营商下拉来自服务器；重建列表不会写档案字段（选中值按 IspPreset 复原），
            // 所以刷新它不算"改草稿"。
            await RefreshCarrierOptionsAsync(SelectedProfile?.IspPreset);

            // 守护进程晚于窗口启动时，配置区还是空的：补一次加载。
            // 用独立标志判断，不看 Profiles 是否为空 —— 用户可能已经点了"新增档案"。
            if (!_configLoaded) await ReloadConfigAsync();
        }
        catch (IpcException ex)
        {
            DaemonRunning = false;
            State = "offline";
            LastError = ex.Message;
        }
    }

    /// <summary>
    /// 从守护进程重新拉配置并覆盖界面。**有未保存草稿时不覆盖**（W1）：
    /// 这是「刷新状态」与「配置重载」分离的关键 —— 状态随便刷，配置只在干净时换。
    /// </summary>
    /// <param name="force">true = 用户显式要求丢弃草稿重载（当前只在启动时用）。</param>
    internal async Task ReloadConfigAsync(bool force = false)
    {
        if (!DaemonRunning) return;
        if (!force && IsDirty)
        {
            OperationResult = "有未保存的修改，已跳过配置重载";
            return;
        }

        try
        {
            var cfgResp = await _ipc.SendAsync("get_config");
            if (cfgResp?["ok"]?.GetValue<bool>() == true && cfgResp["data"] is JsonNode cfg)
                ApplyConfig(cfg);
        }
        catch (IpcException ex)
        {
            LastError = ex.Message;
        }
    }

    /// <summary>
    /// 只把「密码三态」从守护进程同步回界面（导入密码后用）。
    /// 与 <see cref="ReloadConfigAsync"/> 的区别：不重建档案、不动设置、不清草稿。
    /// </summary>
    private async Task SyncPasswordStateAsync()
    {
        try
        {
            var resp = await _ipc.SendAsync("get_config");
            if (resp?["ok"]?.GetValue<bool>() != true || resp["data"] is not JsonNode data) return;
            var model = AppConfig.FromJson(data.ToJsonString());

            _loading = true;
            try
            {
                foreach (var vm in Profiles)
                {
                    var p = model.Profiles.FirstOrDefault(x => x.Name == vm.Name);
                    if (p is null) continue;
                    vm.HasPassword = p.HasPassword;
                    vm.PasswordState = p.PasswordState;
                }
            }
            finally
            {
                _loading = false;
            }
        }
        catch (IpcException)
        {
            // 同步失败不影响"导入成功"本身：下次保存/重载会再对齐
        }
    }

    /// <summary>把服务器配置灌进界面。只允许来自 ReloadConfigAsync（草稿已确认可覆盖）。</summary>
    internal void ApplyConfig(JsonNode cfg)
    {
        var model = AppConfig.FromJson(cfg.ToJsonString());
        var keep = SelectedProfile?.Name;

        _loading = true;   // 程序性赋值不算用户编辑，否则刚同步完就显示"未保存"
        try
        {
            foreach (var old in Profiles) old.PropertyChanged -= OnProfileDraftChanged;
            Profiles.Clear();
            foreach (var p in model.Profiles) AddTracked(new ProfileViewModel(p));
            if (Profiles.Count == 0) AddTracked(new ProfileViewModel());

            ProbeIntervalSeconds = model.Settings.ProbeIntervalSeconds;
            ProbeTimeoutSeconds = model.Settings.ProbeTimeoutSeconds;
            ProbeUrl = model.Settings.ProbeUrl;
            Autostart = model.Settings.Autostart;
            NotifyOnReconnect = model.Settings.NotifyOnReconnect;
            RetryBackoffText = string.Join(", ", model.Settings.RetryBackoffSeconds);
            LogLevel = model.Settings.LogLevel;

            // 基线与生效档案都来自服务器：保存时以它为底，未暴露字段原样透传
            _serverSettings = model.Settings;
            ActiveProfileName = model.ActiveProfile;

            SelectedProfile = Profiles.FirstOrDefault(p => p.Name == keep)
                              ?? Profiles.FirstOrDefault(p => p.Name == model.ActiveProfile)
                              ?? Profiles.FirstOrDefault();
            IsDirty = false;
            _configLoaded = true;
        }
        finally
        {
            _loading = false;
        }
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
        AddTracked(vm);
        SelectedProfile = vm;
        MarkDirty();
    }

    [RelayCommand]
    private void RemoveProfile()
    {
        if (SelectedProfile is null || Profiles.Count <= 1) return;
        var removed = SelectedProfile;
        var idx = Profiles.IndexOf(removed);
        removed.PropertyChanged -= OnProfileDraftChanged;
        Profiles.Remove(removed);
        SelectedProfile = Profiles[Math.Max(0, idx - 1)];

        // 删掉的正好是生效档案：生效必须另指一份，否则保存后守护进程会回退到第一份
        if (ActiveProfileName == removed.Name)
        {
            ActiveProfileName = SelectedProfile?.Name ?? "默认";
            OperationResult = $"生效档案已改到「{ActiveProfileName}」（保存后应用）";
        }
        MarkDirty();
    }

    /// <summary>
    /// 把「正在编辑」的档案设为「当前生效」。只改本地状态，保存时才提交 <c>active_profile</c>。
    /// 这是 W1 的核心：切换列表选中项 ≠ 切换生效档案。
    /// </summary>
    [RelayCommand]
    private void SetActiveProfile()
    {
        if (SelectedProfile is null) return;
        if (ActiveProfileName == SelectedProfile.Name)
        {
            OperationResult = $"「{SelectedProfile.Name}」已经是生效档案";
            return;
        }
        ActiveProfileName = SelectedProfile.Name;
        MarkDirty();
        OperationResult = $"已把「{SelectedProfile.Name}」设为生效（保存后应用）";
    }

    /// <summary>
    /// 解析 set_config 的回包：把「配置已保存」和「开机自启没同步」分开说（W4）。
    /// 旧实现里守护只写一行日志、回包永远是空对象，界面只能笼统显示"已保存"。
    /// 纯逻辑，自检可直接断言。
    /// </summary>
    internal static (string Operation, string? Error) InterpretSaveResult(JsonNode? data, bool activeHasPassword)
    {
        if (data?["autostart_synced"]?.GetValue<bool>() == false)
        {
            var why = data?["autostart_error"]?.GetValue<string>() ?? "未知原因";
            return ("配置已保存，但开机自启同步失败",
                    $"开机自启没生效：{why}（配置本身已保存，可重开界面再试）");
        }
        if (!activeHasPassword)
            return ("已保存（生效档案没有密码）",
                    "生效档案还没有密码，自连会提示「密码为空」——请在密码框输入后保存");
        return ("已保存", null);
    }

    /// <summary>
    /// 把界面草稿打包成待提交的配置（纯逻辑，不碰 IPC —— 自检直接用它）。
    ///
    /// 两个不变量（W1）：
    /// ① <c>active_profile</c> 只取 <see cref="ActiveProfileName"/>，不看当前选中项 ——
    ///    否则"点开看一眼另一个档案"再保存，就把生效档案换掉了；
    /// ② Settings 以服务器基线为底，界面没暴露的字段（probe_expect / log_max_kb /
    ///    log_keep_files）原样透传 —— 旧代码把 probe_expect 写空，用户自定义的探测标识
    ///    在保存普通设置时被抹掉。
    /// </summary>
    internal AppConfig BuildConfigForSave()
    {
        var cfg = new AppConfig { ActiveProfile = ActiveProfileName };
        foreach (var vm in Profiles) cfg.Profiles.Add(vm.ToModel(includePassword: true));

        var probeUrl = ProbeUrl?.Trim();
        cfg.Settings = Settings.WithUiFields(
            baseline: _serverSettings ?? new Settings(),
            probeIntervalSeconds: (int)ProbeIntervalSeconds,
            probeTimeoutSeconds: (int)ProbeTimeoutSeconds,
            probeUrl: string.IsNullOrEmpty(probeUrl) ? new AppConfig().Settings.ProbeUrl : probeUrl!,
            autostart: Autostart,
            notifyOnReconnect: NotifyOnReconnect,
            logLevel: LogLevel,
            retryBackoffSeconds: ParseBackoff(RetryBackoffText));
        return cfg;
    }

    [RelayCommand]
    private async Task SaveAsync()
    {
        LastError = null;
        if (!DaemonRunning)
        {
            // 草稿原样保留：进程起来后还能接着保存
            LastError = "守护进程未运行，配置未保存";
            return;
        }

        var cfg = BuildConfigForSave();

        try
        {
            // 注意：config 必须是 **JSON 对象**（守护进程侧按对象解析）；不能塞字符串。
            var resp = await _ipc.SendAsync("set_config", new JsonObject
            {
                ["config"] = JsonNode.Parse(cfg.ToJson()),
            });

            if (resp?["ok"]?.GetValue<bool>() == true)
            {
                foreach (var vm in Profiles)
                {
                    // 带新密码的档案：守护进程已加密落盘，这里同步「已保存」状态
                    if (vm.PasswordInput.Length > 0) vm.HasPassword = true;
                    vm.PasswordInput = "";   // 已交给守护进程，界面不再留存明文
                }

                // 基线前移：下次保存以这次提交的值为底，不会退回更早的 probe_expect
                _serverSettings = cfg.Settings;
                IsDirty = false;

                // ⚠「已保存」≠「能登录」，也≠「自启已生效」：
                // 判缺密码看的是**生效**档案（自连用的是它），自启看守护进程的回包（W4）。
                var active = Profiles.FirstOrDefault(p => p.Name == ActiveProfileName)
                             ?? Profiles.FirstOrDefault();
                var (op, err) = InterpretSaveResult(resp["data"], active?.HasPassword == true);
                OperationResult = op;
                LastError = err;
            }
            else
            {
                // 草稿保留：IsDirty 不动、密码框不清
                LastError = (resp?["error"]?.GetValue<string>() ?? "保存失败") + "（草稿已保留）";
            }
        }
        catch (IpcException ex)
        {
            LastError = ex.Message + "（草稿已保留）";
        }
    }

    [RelayCommand]
    private async Task ExportSecretAsync()
    {
        LastError = null;
        if (SelectedProfile is null) { LastError = "请先选择一个配置档案"; return; }
        if (SecretPassphrase.Length < 6) { LastError = "口令至少 6 位（导出文件全靠它保护）"; return; }
        if (SecretPath.Trim().Length == 0) { LastError = "请填写导出文件路径"; return; }
        if (!DaemonRunning) { LastError = "守护进程未运行，无法导出"; return; }

        try
        {
            var resp = await _ipc.SendAsync("export_secret", new JsonObject
            {
                ["path"] = SecretPath.Trim(),
                ["passphrase"] = SecretPassphrase,
                ["profile"] = SelectedProfile.Name,
            });

            if (resp?["ok"]?.GetValue<bool>() == true)
            {
                OperationResult = "密码已导出";
                SecretPassphrase = "";   // 口令不留在界面里
                LastError = $"已导出到 {resp["data"]?["path"]?.GetValue<string>() ?? SecretPath}；口令请另走一个渠道告诉对方";
            }
            else
            {
                LastError = resp?["error"]?.GetValue<string>() ?? "导出失败";
            }
        }
        catch (IpcException ex)
        {
            LastError = ex.Message;
        }
    }

    [RelayCommand]
    private async Task ImportSecretAsync()
    {
        LastError = null;
        if (SecretPassphrase.Length < 6) { LastError = "请填写导出时设的口令"; return; }
        if (SecretPath.Trim().Length == 0) { LastError = "请填写导出文件路径"; return; }
        if (!DaemonRunning) { LastError = "守护进程未运行，无法导入"; return; }

        try
        {
            var resp = await _ipc.SendAsync("import_secret", new JsonObject
            {
                ["path"] = SecretPath.Trim(),
                ["passphrase"] = SecretPassphrase,
            });

            if (resp?["ok"]?.GetValue<bool>() == true)
            {
                SecretPassphrase = "";
                // 只同步「有没有密码」，不整体重载配置 —— 导入密码不该顺手丢草稿（W1）
                await SyncPasswordStateAsync();
                OperationResult = "密码已导入";
                var created = resp["data"]?["created_new_profile"]?.GetValue<bool>() == true;
                LastError = created
                    ? "导入成功，但新建了档案：请补上认证网址与运营商后保存"
                    : "导入成功，可以关闭窗口了";
            }
            else
            {
                LastError = resp?["error"]?.GetValue<string>() ?? "导入失败";
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
            OperationResult = resp?["ok"]?.GetValue<bool>() == true
                ? "已触发立即重连"
                : resp?["error"]?.GetValue<string>() ?? "重连请求失败";
        }
        catch (IpcException ex)
        {
            LastError = ex.Message;
        }
    }

    /// <summary>
    /// 用当前档案做一次「探测 → 识别 → 登录」并回报结果（现场排查用）。
    /// ⚠ 这是**用户点击触发**的联网动作：会真的尝试登录一次。
    /// </summary>
    [RelayCommand]
    private async Task TestProfileAsync()
    {
        if (SelectedProfile is null)
        {
            LastError = "请先选择一个配置档案";
            return;
        }
        if (!DaemonRunning)
        {
            LastError = "守护进程未运行，无法测试";
            return;
        }

        var name = SelectedProfile.Name;
        OperationResult = $"正在测试「{name}」…（会真的尝试登录一次）";
        LastError = null;
        try
        {
            var resp = await _ipc.SendAsync("test_profile", new JsonObject { ["profile"] = name });
            if (resp?["ok"]?.GetValue<bool>() != true)
            {
                LastError = resp?["error"]?.GetValue<string>() ?? "测试失败";
                OperationResult = "测试未完成";
                return;
            }

            var d = resp["data"];
            var ok = d?["success"]?.GetValue<bool>() == true;
            var detail = d?["message"]?.GetValue<string>() ?? "";
            OperationResult = ok
                ? $"测试成功（{d?["strategy"]?.GetValue<string>()}）"
                : $"测试未成功（{d?["strategy"]?.GetValue<string>()}）：{detail}";
            if (!ok) LastError = detail;   // 失败细节留在底部，便于照抄排查
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

    /// <summary>空串/空值统一显示为「—」，否则界面上会出现「本机 IP：」后面什么都没有。</summary>
    private static string OrDash(string? s) => string.IsNullOrEmpty(s) ? "—" : s;

    /// <summary>
    /// 网关 + ARP 状态。只有 IP 看不出「网关根本不通」：
    /// ARP 未解析意味着二层断（2026-11-01 真实故障形态），此时重试登录必然无效。
    /// </summary>
    private static string FormatGateway(string? gateway, string? arpState)
    {
        if (string.IsNullOrEmpty(gateway)) return "—";
        var arp = arpState switch
        {
            "resolved" => "ARP 已解析",
            "unresolved" => "ARP 未解析，二层可能断",
            _ => "ARP 未知",
        };
        return $"{gateway}（{arp}）";
    }
}
