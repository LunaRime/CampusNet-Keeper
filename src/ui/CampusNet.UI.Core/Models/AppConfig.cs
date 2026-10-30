using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace CampusNet.UI.Models;

/// <summary>
/// C++ 侧 <c>src/native/store/Config.h</c> 的镜像，**字段名必须逐字对应**（snake_case）。
/// 
/// ⚠ **密码规则（硬约束，见计划任务 24）**：UI **从不**读写 <c>password_enc</c>——
/// 那是 DPAPI 密文，只由 C++ 侧生成与落盘。界面只需要知道「有没有密码」
/// （<see cref="Profile.HasPassword"/>，只读）；用户输入新密码时只写
/// <see cref="Profile.PasswordPlain"/>（非空才发），由 C++ 侧加密后保存。
/// UI 进程因此从头到尾不接触已存密码的明文。
/// 
/// ⚠ 序列化必须**单行**：这些 JSON 要经命名管道的「一行一条」协议传输。
/// </summary>
public sealed class AppConfig
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = false,           // 行协议要求单行
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    [JsonPropertyName("version")] public int Version { get; set; } = 1;
    [JsonPropertyName("active_profile")] public string ActiveProfile { get; set; } = "默认";
    [JsonPropertyName("settings")] public Settings Settings { get; set; } = new();
    [JsonPropertyName("profiles")] public List<Profile> Profiles { get; set; } = new();

    public string ToJson()
    {
        var node = JsonNode.Parse(JsonSerializer.Serialize(this, Options))!;

        // ⚠ 用户新输入的密码必须在这里补回：PasswordPlain 带 [JsonIgnore]（防止明文被
        // 任何属性序列化顺手带出去），而保存走的是本方法，不是 Profile.ToJson()。
        // System.Text.Json 不认 ShouldSerializeXxx 约定 → 少了这段，明文根本不进
        // set_config 报文，守护进程只能按档案名沿用旧密码（新机器上等于没有），
        // 症状就是「界面显示已保存、自连却报密码为空」。
        if (node["profiles"] is JsonArray arr)
            for (var i = 0; i < arr.Count && i < Profiles.Count; i++)
                if (Profiles[i].PasswordPlain.Length > 0 && arr[i] is JsonObject po)
                    po["password_plain"] = Profiles[i].PasswordPlain;

        return node.ToJsonString();
    }

    public static AppConfig FromJson(string json) =>
        JsonSerializer.Deserialize<AppConfig>(json, Options) ?? new AppConfig();

    /// <summary>命中的档案；不存在时取第一份；没有档案时返回 null（与 C++ ActiveProfile 同语义）。</summary>
    public Profile? ActiveProfileOrDefault() =>
        Profiles.FirstOrDefault(p => p.Name == ActiveProfile) ?? Profiles.FirstOrDefault();
}

/// <summary>镜像 C++ <c>Settings</c>。</summary>
public sealed class Settings
{
    [JsonPropertyName("probe_interval_seconds")] public int ProbeIntervalSeconds { get; set; } = 5;
    [JsonPropertyName("probe_timeout_seconds")] public int ProbeTimeoutSeconds { get; set; } = 6;
    [JsonPropertyName("probe_url")] public string ProbeUrl { get; set; } =
        "http://captive.apple.com/hotspot-detect.html";
    [JsonPropertyName("probe_expect")] public string ProbeExpect { get; set; } = "Success";
    [JsonPropertyName("retry_backoff_seconds")] public List<int> RetryBackoffSeconds { get; set; } =
        new() { 5, 10, 20, 30, 60 };
    [JsonPropertyName("autostart")] public bool Autostart { get; set; } = true;
    [JsonPropertyName("notify_on_reconnect")] public bool NotifyOnReconnect { get; set; } = true;
    [JsonPropertyName("log_level")] public string LogLevel { get; set; } = "info";
    [JsonPropertyName("log_max_kb")] public int LogMaxKb { get; set; } = 512;
    [JsonPropertyName("log_keep_files")] public int LogKeepFiles { get; set; } = 3;
}

/// <summary>镜像 C++ <c>IspConfig</c>（preset: campus|cmcc|telecom|unicom|custom）。</summary>
public sealed class IspConfig
{
    [JsonPropertyName("preset")] public string Preset { get; set; } = "campus";
    [JsonPropertyName("custom")] public string Custom { get; set; } = "";
}

/// <summary>镜像 C++ <c>CustomTemplate</c>。</summary>
public sealed class CustomTemplate
{
    [JsonPropertyName("method")] public string Method { get; set; } = "POST";
    [JsonPropertyName("url")] public string Url { get; set; } = "";
    [JsonPropertyName("body")] public string Body { get; set; } = "";
    [JsonPropertyName("success_regex")] public string SuccessRegex { get; set; } = "";
    [JsonPropertyName("headers")] public Dictionary<string, string> Headers { get; set; } = new();
}

/// <summary>镜像 C++ <c>Profile</c>。</summary>
public sealed class Profile
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = false };

    [JsonPropertyName("name")] public string Name { get; set; } = "默认";
    [JsonPropertyName("auth_url")] public string AuthUrl { get; set; } = "";
    [JsonPropertyName("username")] public string Username { get; set; } = "";
    [JsonPropertyName("vendor")] public string Vendor { get; set; } = "auto";
    [JsonPropertyName("isp")] public IspConfig Isp { get; set; } = new();
    [JsonPropertyName("custom_template")] public CustomTemplate CustomTemplate { get; set; } = new();

    /// <summary>只读：守护进程告诉我们「这份档案已有密码」。UI 不写、也不据此写盘。</summary>
    [JsonPropertyName("has_password")] public bool HasPassword { get; set; }

    /// <summary>
    /// 只读三态：<c>ok</c>=本机可解；<c>foreign</c>=密文存在但本机解不开（换机器/换 Windows 账号，
    /// 需要重新输入）；<c>none</c>=从来没有密码。界面据此给不同提示，别把 foreign 说成「没设置过」。
    /// </summary>
    [JsonPropertyName("password_state")] public string PasswordState { get; set; } = "none";

    /// <summary>
    /// 写侧：用户输入的新密码（明文）。**非空才会被序列化**——没改密码时绝不能覆盖
    /// 已存的 password_enc（C++ 侧据此判断要不要重新加密）。
    /// </summary>
    [JsonIgnore] public string PasswordPlain { get; set; } = "";

    // 注意：这里**不要**写 ShouldSerializePasswordPlain()。那是 Newtonsoft.Json 的约定，
    // System.Text.Json 完全不认，写了不生效还让人以为已经处理好了——
    // password_plain 由 AppConfig.ToJson() 统一补入。

    /// <summary>
    /// 单档案的报文形态（自检与调试用；**真正的保存走 <see cref="AppConfig.ToJson"/>**，
    /// 两者对 password_plain 的处理必须一致，否则又会退回「已保存但没密码」）。
    /// </summary>
    public string ToJson()
    {
        var node = JsonNode.Parse(JsonSerializer.Serialize(this, Options))!;
        if (PasswordPlain.Length > 0) node["password_plain"] = PasswordPlain;
        return node.ToJsonString();
    }

    public static Profile Create(string name, string authUrl, string username, string vendor,
                                 string ispPreset, string ispCustom) => new()
    {
        Name = name,
        AuthUrl = authUrl,
        Username = username,
        Vendor = vendor,
        Isp = new IspConfig { Preset = ispPreset, Custom = ispCustom },
    };
}
