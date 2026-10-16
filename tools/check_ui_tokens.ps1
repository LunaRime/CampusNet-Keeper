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

Write-Host ''
if ($script:fail -eq 0) { Write-Host 'UI 令牌层检查：全部通过'; exit 0 }
Write-Host "UI 令牌层检查：$($script:fail) 项失败"; exit 1
