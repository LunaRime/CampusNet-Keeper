# UI 令牌层静态检查（**不需要 Avalonia 包**即可运行）。
#
# 为什么需要它：本机校园网 HTTPS 不通，NuGet 取不到 Avalonia，UI 工程暂时无法编译。
# 但设计令牌层能验证的部分不依赖编译器，于是把这些断言先固化成脚本：
#   1) 所有 .axaml / .csproj / .manifest 必须是格式良好的 XML
#   2) Theme.Light 与 Theme.Dark 的 x:Key 集合必须**完全一致**（明暗切换不该缺键）
#   3) 除 Theme.*.axaml 外，任何 XAML 不得出现裸色值（色值只能有一个来源）
#   4) XAML 里 {DynamicResource Cnk*} 引用的每个键都必须存在于两份主题字典
#
# 用法：pwsh -File tools/check_ui_tokens.ps1     （退出码 0 = 全过）
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ui = Join-Path $root 'src\ui'
$script:fail = 0
function Pass($m) { Write-Host "  [PASS] $m" }
function Fail($m) { Write-Host "  [FAIL] $m"; $script:fail++ }

Write-Host '1) XML 良构'
$xmlFiles = Get-ChildItem $ui -Recurse -File | Where-Object { $_.Extension -in '.axaml', '.csproj', '.manifest' }
foreach ($f in $xmlFiles) {
    try { [xml](Get-Content $f.FullName -Raw) | Out-Null; Pass $f.Name }
    catch { Fail "$($f.Name) 解析失败：$($_.Exception.Message)" }
}

Write-Host '2) 明暗主题键一一对应'
$xns = 'http://schemas.microsoft.com/winfx/2006/xaml'
function Get-Keys($path) {
    $doc = [xml](Get-Content $path -Raw)
    $keys = @()
    foreach ($n in $doc.DocumentElement.ChildNodes) {
        if ($n.NodeType -eq 'Element' -and $n.HasAttribute('Key', $xns)) {
            $keys += $n.GetAttribute('Key', $xns)
        }
    }
    return $keys
}
$light = Get-Keys (Join-Path $ui 'CampusNet.UI\Theme.Light.axaml')
$dark = Get-Keys (Join-Path $ui 'CampusNet.UI\Theme.Dark.axaml')
$onlyLight = $light | Where-Object { $_ -notin $dark }
$onlyDark = $dark | Where-Object { $_ -notin $light }
if ($onlyLight.Count -eq 0 -and $onlyDark.Count -eq 0) {
    Pass "两主题键一致（各 $($light.Count) 个）"
} else {
    if ($onlyLight) { Fail "仅亮色有：$($onlyLight -join ', ')" }
    if ($onlyDark) { Fail "仅暗色有：$($onlyDark -join ', ')" }
}

Write-Host '3) 色值只在主题文件里出现'
$hex = [regex]'#[0-9a-fA-F]{3,8}\b'
foreach ($f in $xmlFiles) {
    if ($f.Name -like 'Theme.*.axaml') { continue }
    $hits = $hex.Matches((Get-Content $f.FullName -Raw)) | ForEach-Object { $_.Value } | Select-Object -Unique
    if ($hits) { Fail "$($f.Name) 出现裸色值：$($hits -join ', ')" }
}
if ($script:fail -eq 0) { Pass '除主题文件外无裸色值' }

Write-Host '4) 引用的令牌键都存在'
$ref = [regex]'\{DynamicResource\s+(Cnk[A-Za-z0-9]+)\}'
# 键的来源有两处：两份主题字典（随明暗切换）与 App.axaml 的应用级资源（如字体栈）
$appKeys = @()
$appDoc = [xml](Get-Content (Join-Path $ui 'CampusNet.UI\App.axaml') -Raw)
foreach ($n in $appDoc.SelectNodes('//*[local-name()="ResourceDictionary"]/*')) {
    if ($n.LocalName -eq 'ThemeDictionaries') { continue }
    if ($n.HasAttribute('Key', $xns)) { $appKeys += $n.GetAttribute('Key', $xns) }
}
$allKeys = @($light) + @($dark) + @($appKeys) | Select-Object -Unique
$missing = @()
foreach ($f in $xmlFiles) {
    if ($f.Name -like 'Theme.*.axaml') { continue }
    foreach ($m in $ref.Matches((Get-Content $f.FullName -Raw))) {
        if ($m.Groups[1].Value -notin $allKeys) { $missing += "$($f.Name):$($m.Groups[1].Value)" }
    }
}
if ($missing.Count -eq 0) { Pass "所有 DynamicResource 引用都能解析（键池 $($allKeys.Count) 个）" } else { Fail "找不到的键：$($missing -join ', ')" }


Write-Host '5) 布局回归（长文案不撑破 / 按钮文字居中）'
# 5a：状态条与底部错误提示都必须是「可截断」的——2026-10-26 用户反馈长错误文案
#     把「立即重连」顶出了窗口。静态钉住：绑定这两个属性的文本必须有 TextTrimming。
#     v2 视图已拆成多个 UserControl，故扫描整个 UI 目录而不是只看 MainWindow.axaml。
$xamlDocs = @()
foreach ($f in $xmlFiles | Where-Object { $_.Extension -eq '.axaml' }) {
    $xamlDocs += ,@($f.Name, ([xml](Get-Content $f.FullName -Raw)))
}
foreach ($prop in @('StatusText', 'LastError')) {
    $found = @()
    foreach ($pair in $xamlDocs) {
        foreach ($node in $pair[1].SelectNodes("//*[@Text='{Binding $prop}']")) {
            $found += ,@($pair[0], $node.GetAttribute('TextTrimming'))
        }
    }
    if ($found.Count -eq 0) { Fail "找不到绑定 $prop 的元素"; continue }
    $bad = @($found | Where-Object { $_[1] -ne 'CharacterEllipsis' })
    if ($bad.Count -gt 0) {
        Fail "$prop 缺 TextTrimming=CharacterEllipsis（长文案会撑破布局、把按钮顶出窗口）：$($bad[0][0])"
    } else {
        Pass "$prop 已启用截断（长文案不再撑破布局）"
    }
}

# 5b：按钮文字居中必须在基础样式里显式声明。ContentControl 默认左对齐，
#     只覆盖 Padding 会让短标签贴着左边（用户反馈"文字不居中、没利用两边间隔"）。
#     v2 起控件样式集中在 Controls.axaml，故同样扫描整个 UI 目录。
$btnOk = $false
foreach ($pair in $xamlDocs) {
    $styles = @($pair[1].SelectNodes('//*[local-name()="Style"]') |
                Where-Object { $_.GetAttribute('Selector') -eq 'Button' })
    foreach ($st in $styles) {
        $setter = $st.SelectSingleNode('.//*[local-name()="Setter"][@Property="HorizontalContentAlignment"]')
        if ($setter -and $setter.GetAttribute('Value') -eq 'Center') { $btnOk = $true }
    }
}
if ($btnOk) { Pass '按钮文字水平居中（基础 Button 样式）' }
else { Fail '任意 axaml 中均未找到含 HorizontalContentAlignment=Center 的基础 Button 样式' }

Write-Host ''
if ($script:fail -eq 0) { Write-Host 'UI 静态检查：全部通过'; exit 0 }
Write-Host "UI 静态检查：$($script:fail) 项失败"; exit 1
