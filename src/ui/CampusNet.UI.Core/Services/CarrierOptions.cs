using System.Text.Json.Nodes;

namespace CampusNet.UI.Services;

/// <summary>运营商下拉的一项。</summary>
/// <param name="Value">写回配置的值（对应 <c>isp.preset</c>）。</param>
/// <param name="Label">界面显示名。</param>
/// <param name="IsCustom">是否为「自定义…」项（选中时要展开文本框填 <c>isp.custom</c>）。</param>
public sealed record CarrierOption(string Value, string Label, bool IsCustom = false);

/// <summary>
/// 运营商下拉的合并规则（计划任务 24 的实现重点）：
/// <list type="number">
/// <item>优先用守护进程 <c>get_carrier_options</c> 拿到的**服务器下发**列表（各校不同）；</item>
/// <item>取不到（守护进程未运行 / 该部署不提供）时**回退内置预设**；</item>
/// <item>**始终保留「自定义…」项**且在末位；</item>
/// <item>当前配置值若不在列表里（服务器改版、或用户之前填过），也要补上，避免下拉显示空白。</item>
/// </list>
/// </summary>
public static class CarrierOptions
{
    /// <summary>「自定义…」的值（对应 C++ 的 <c>isp.preset == "custom"</c>）。</summary>
    public const string CustomValue = "custom";

    /// <summary>已知取值的显示名。服务器只给 id 不给 name 时用它兜底，别把 cmcc 这种原文丢给用户。</summary>
    // ⚠ 必须声明在 Builtin 之前：静态字段按文本顺序初始化，反了就是 NullReferenceException。
    private static readonly Dictionary<string, string> KnownLabels = new(StringComparer.OrdinalIgnoreCase)
    {
        ["campus"] = "校园网（默认）",
        ["cmcc"] = "中国移动",
        ["telecom"] = "中国电信",
        ["unicom"] = "中国联通",
    };

    /// <summary>内置回退预设（与 C++ <c>IspSuffix</c> 支持的取值一致）。</summary>
    public static IReadOnlyList<CarrierOption> Builtin { get; } = new[]
    {
        new CarrierOption("campus", KnownLabels["campus"]),
        new CarrierOption("cmcc", KnownLabels["cmcc"]),
        new CarrierOption("telecom", KnownLabels["telecom"]),
        new CarrierOption("unicom", KnownLabels["unicom"]),
    };

    /// <summary>
    /// 取显示名：服务器给的名字优先；但「空」或「只是把 id 原样回显」都不算名字
    /// （真机上出现过只下发 id、name 为空的部署 → 下拉里出现一条光秃秃的 cmcc）。
    /// </summary>
    private static string DisplayLabel(string key, string? serverName)
    {
        if (!string.IsNullOrWhiteSpace(serverName) &&
            !string.Equals(serverName, key, StringComparison.OrdinalIgnoreCase))
            return serverName!;
        return KnownLabels.TryGetValue(key, out var zh) ? zh : key;
    }

    public static CarrierOption Custom { get; } = new(CustomValue, "自定义…", true);

    /// <summary>
    /// 合并出最终绑定列表。<paramref name="fromServer"/> 为守护进程 <c>get_carrier_options</c>
    /// 的返回（计划任务 25 的形态是 <c>{"options":[{"id","name","suffix"}],"title","default_id"}</c>；
    /// 为了向前兼容也接受裸数组，以及 <c>{key,label}</c> 这种旧字段名）。
    /// <paramref name="current"/> 为当前配置里的 <c>isp.preset</c>（可为空）。
    /// </summary>
    public static List<CarrierOption> Merge(JsonNode? fromServer, string? current)
    {
        var list = new List<CarrierOption>();
        var array = fromServer as JsonArray ?? fromServer?["options"] as JsonArray;

        if (array is { Count: > 0 })
        {
            foreach (var item in array)
            {
                var key = item?["id"]?.GetValue<string>() ?? item?["key"]?.GetValue<string>();
                if (string.IsNullOrEmpty(key)) continue;
                var label = item?["name"]?.GetValue<string>() ?? item?["label"]?.GetValue<string>();
                list.Add(new CarrierOption(key, DisplayLabel(key, label)));
            }
        }

        if (list.Count == 0) list.AddRange(Builtin);

        // 当前值不在列表里也要能显示（否则下拉会空白，用户以为配置丢了）。
        // 显示名仍走映射表：这里曾经直接拿 id 当名字，于是下拉里冒出一条「cmcc」。
        if (!string.IsNullOrEmpty(current) && current != CustomValue &&
            !list.Exists(o => o.Value == current))
        {
            list.Add(new CarrierOption(current, DisplayLabel(current, null)));
        }

        list.Add(Custom);
        return list;
    }
}
