<#
.SYNOPSIS
  CampusNet-Keeper 一键构建脚本：配置 → 构建 → 测试 → **安装到 build\Release** → 校验产物。

.DESCRIPTION
  为什么要有"安装 + 校验"这两步（2026-10-17 的真实教训）：
  曾出现"用户报告检测不到守护进程/无法保存配置"，根因是我只把新构建留在开发树
  （build-m1\Release），忘了同步到用户实际运行的 build\Release —— 运行的是旧二进制。
  所以本脚本把"同步安装 + 校验新产物含预期标记"固化下来，不靠人记得。

  任务 26 起多了 `-SingleExe`：把 Avalonia 界面发布产物打成**存储型 ZIP** 嵌进 exe，
  得到一个"单 exe 双形态"（守护 + `--ui` 界面）的完整版；不带该开关时构建精简版，
  并会**主动删掉**残留的载荷资源，避免精简版误嵌上一轮的旧载荷。

.PARAMETER Config
  构建配置（Release/Debug），默认 Release。

.PARAMETER SingleExe
  构建完整版单 exe：发布 UI → 生成载荷资源 → 编进 exe。目标机器需装 .NET 10 Desktop
  Runtime（界面用；守护与 CLI 不需要）。

.PARAMETER SelfContainedPayload
  配合 -SingleExe：把 .NET 运行时一起打进载荷，做到**零依赖**（exe 会大很多，
  且首次需要联网 restore 运行时包）。

.PARAMETER SkipTests
  跳过 C++ 测试（仅用于快速验证能否编译）。

.PARAMETER SkipUi
  跳过 Avalonia UI 构建（UI 需要 NuGet 包；首次需联网 restore）。

.PARAMETER Package
  额外产出发布压缩包 build\CampusNet-Keeper-win-x64.zip。

.EXAMPLE
  pwsh -File build.ps1
  pwsh -File build.ps1 -SingleExe
  pwsh -File build.ps1 -SingleExe -SelfContainedPayload -Package
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [switch]$SingleExe,
    [switch]$SelfContainedPayload,
    [switch]$SkipTests,
    [switch]$SkipUi,
    [switch]$Package,
    # ---- 数字签名（可选）----
    # -Sign 后：给了 PfxPath 就用真实证书，给了 CertThumbprint 就用证书库里的，
    # 什么都不给则用/创建自签名证书（免费，但只有信任它的机器认）。
    [switch]$Sign,
    [string]$CertThumbprint = '',
    [string]$PfxPath = '',
    [string]$PfxPassword = '',
    [string]$TimestampUrl = '',
    [switch]$NoTimestamp
)

$ErrorActionPreference = 'Stop'
# 原生命令的非零退出码由各处显式判断（不让 PowerShell 7 的新开关把非零当异常抛出）
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# 统一编码口径：本脚本会拿正则去匹配 C++ exe 打印的中文（--ui-info 的「全部为存储型」
# 「载荷校验: 通过」）。控制台默认是 cp936 时，PowerShell 会把 exe 输出的 UTF-8 字节
# 按 GBK 解码，个别汉字变成 U+FFFD，正则静默失配 → 误报「载荷含压缩条目」。
# 2026-10-26 实测踩到；CI 上曾因同类原因让审计脚本崩过，所以这里显式钉死。
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }

$root = $PSScriptRoot
# 主程序是 GUI 子系统（开机不闪窗、启动项显示自身），PowerShell 直接调用它拿不到输出、
# 也不等它结束 → 所有对本 exe 的"跑一下看结果"都必须走这个统一的重定向捕获。
. (Join-Path $root 'scripts\invoke_cli.ps1')
$devBuild = Join-Path $root 'build-m1'      # 开发/沙箱友好的构建树（见 .gitignore）
$outDir = Join-Path $root 'build\Release'   # **用户实际运行**的目录：必须每次同步
$payloadDir = Join-Path $root 'build\generated'
$script:failed = 0
$script:stepNo = 0
$script:stepTotal = 7 + $(if ($Package) { 1 } else { 0 }) + $(if ($Sign) { 1 } else { 0 })
if ($SelfContainedPayload -and -not $SingleExe) {
    Write-Host '[警告] -SelfContainedPayload 只在 -SingleExe 下有意义；本次会被忽略（精简版不嵌载荷）' -ForegroundColor Yellow
}

function Step($name) {
    $script:stepNo++
    Write-Host "`n=== $script:stepNo/$script:stepTotal $name ===" -ForegroundColor Cyan
}
function Ok($m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [警告] $m" -ForegroundColor Yellow }
function Die($m) { Write-Host "  [失败] $m" -ForegroundColor Red; exit 1 }

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
        'D:\Visual Studio 2026\Microsoft Visual Studio\2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )) { if (Test-Path $p) { return $p } }
    throw 'cmake 未找到：请安装 Visual Studio 2022（含 C++ 桌面开发工作负载），或把 cmake 加入 PATH'
}

function Invoke-Checked($file, $arguments, $what) {
    & $file @arguments
    if ($LASTEXITCODE -ne 0) { Die "$what 失败（退出码 $LASTEXITCODE）" }
}

# dotnet 需要可写的 CLI_HOME；包缓存与中间目录都放在工作区内（沙箱/CI 友好）
function Use-LocalDotnetEnv {
    $env:DOTNET_CLI_HOME = Join-Path $root 'build-ui\dotnet-home'
    $env:NUGET_PACKAGES = Join-Path $root 'build-ui\nuget'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
}

$cmake = Find-CMake
Write-Host "CampusNet-Keeper 构建（配置=$Config$(if ($SingleExe) { '，完整版单 exe' }))" -ForegroundColor White
Write-Host "  仓库: $root"
Write-Host "  cmake: $cmake"

# ------------------------------------------------- 1) UI 载荷（仅完整版需要，必须在 CMake 配置之前）
if ($SingleExe) {
    Step '发布 Avalonia 界面并生成内嵌载荷'
    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        Die '未安装 dotnet SDK：-SingleExe 需要先发布界面（可先用不带开关的精简构建）'
    }
    if ($SkipUi) { Warn '已指定 -SkipUi，但 -SingleExe 必须发布界面（忽略 -SkipUi）' }
    Use-LocalDotnetEnv
    $publish = Join-Path $root 'build-ui\publish'
    Remove-Item $publish -Recurse -Force -ErrorAction SilentlyContinue
    $selfContained = if ($SelfContainedPayload) { 'true' } else { 'false' }
    Invoke-Checked 'dotnet' @(
        'publish', (Join-Path $root 'src\ui\CampusNet.UI\CampusNet.UI.csproj'),
        '-c', $Config, '-r', 'win-x64', '--self-contained', $selfContained, '-o', $publish
    ) 'UI 发布'
    Invoke-Checked 'pwsh' @(
        '-NoProfile', '-File', (Join-Path $root 'scripts\embed_ui.ps1'),
        '-UiPublishDir', $publish, '-OutDir', $payloadDir
    ) 'UI 载荷打包'
} else {
    Step '精简单文件版：清掉可能残留的 UI 载荷资源'
    $removed = 0
    foreach ($f in @('ui_payload.rc', 'ui_payload.zip', 'ui_payload_id.txt')) {
        $p = Join-Path $payloadDir $f
        if (Test-Path $p) { Remove-Item $p -Force; $removed++ ; Warn "已删除残留的 $f（避免精简版误嵌旧载荷）" }
    }
    if ($removed -eq 0) { Ok '没有残留载荷资源' }
}

# ---------------------------------------------------------------- 2) 配置
Step '配置 CMake'
# 每次都配置：载荷资源的"出现/消失"必须立刻生效（否则会构建出与预期不符的形态）
# CNK_REQUIRE_REAL_PAYLOAD：完整版必须带上真实载荷，缺了就让测试**失败**而不是静默跳过
$requirePayload = if ($SingleExe) { 'ON' } else { 'OFF' }
Invoke-Checked $cmake @(
    '-S', $root, '-B', $devBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCNK_REQUIRE_REAL_PAYLOAD=$requirePayload"
) 'CMake 配置'

# ---------------------------------------------------------------- 3) 构建
Step '构建 C++（cnk_core / 主程序 / 测试 / 探针）'
Invoke-Checked $cmake @('--build', $devBuild, '--config', $Config) 'C++ 构建'

# ---------------------------------------------------------------- 3b) 数字签名（可选）
# 在**安装/打包之前**签开发树里的主程序：第 6 步的"安装后哈希一致"校验因此仍然成立，
# 用户拿到的就是签名后的文件。
if ($Sign) {
    Step '数字签名'
    $exeToSign = Join-Path $devBuild "$Config\CampusNet-Keeper.exe"
    if (-not (Test-Path $exeToSign)) { Die "待签名文件不存在：$exeToSign" }
    $signArgs = @('-NoProfile', '-File', (Join-Path $root 'scripts\sign.ps1'),
                  '-Files', $exeToSign)
    if ($CertThumbprint) { $signArgs += @('-Thumbprint', $CertThumbprint) }
    if ($PfxPath) { $signArgs += @('-PfxPath', $PfxPath) }
    if ($PfxPassword) { $signArgs += @('-PfxPassword', $PfxPassword) }
    if ($TimestampUrl) { $signArgs += @('-TimestampUrl', $TimestampUrl) }
    if ($NoTimestamp) { $signArgs += '-NoTimestamp' }
    & pwsh @signArgs
    if ($LASTEXITCODE -ne 0) { Die '数字签名失败' }
}

# ---------------------------------------------------------------- 4) 测试
Step '运行 C++ 测试'
$testExe = Join-Path $devBuild "$Config\cnk_tests.exe"
if ($SkipTests) {
    Warn '按要求跳过测试'
} elseif (-not (Test-Path $testExe)) {
    Die "测试程序不存在：$testExe"
} else {
    $out = & $testExe 2>&1
    $out | Select-String -Pattern 'test cases:|assertions:|Status:' | ForEach-Object { '  ' + $_.Line.Trim() }
    # 完整性标签用例依赖"能写完整性标签"的权限，普通用户/沙箱下会失败——单独识别并说明，
    # 但**其他任何失败都算构建失败**（不能习惯性放过）。
    $fails = $out | Select-String -Pattern ': ERROR:|: FATAL ERROR:'
    $other = $fails | Where-Object { $_.Line -notmatch 'integrity_test\.cpp' }
    if ($other) {
        $other | Select-Object -First 8 | ForEach-Object { '  ' + $_.Line.Trim() }
        Die "测试存在非环境性失败（$($other.Count) 条）"
    }
    if ($fails) { Warn "只有 integrity_test 失败（需要写完整性标签的权限），视为环境性：$($fails.Count) 条" }
    Ok '测试通过'
}

# ---------------------------------------------------------------- 5) UI（可选）
Step '构建 Avalonia 设置界面'
$uiExe = Join-Path $root 'build-ui\bin\CampusNet.UI\Release\net10.0-windows\CampusNet.UI.exe'
if ($SingleExe) {
    Ok '已在上面的发布步骤构建（跳过重复构建）'
} elseif ($SkipUi) {
    Warn '按要求跳过 UI 构建'
} elseif (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Warn '未安装 dotnet SDK，跳过 UI（守护进程与 CLI 不受影响）'
} else {
    Use-LocalDotnetEnv
    & dotnet build (Join-Path $root 'src\ui\CampusNet.UI') -c Release
    if ($LASTEXITCODE -ne 0) { Warn 'UI 构建失败（首次需要联网 restore Avalonia；守护进程与 CLI 不受影响）' }
    else { Ok 'UI 构建通过' }
}

# ---------------------------------------------------------------- 6) 安装（关键步骤）
Step "安装到 $outDir（用户实际运行的目录）"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$artifacts = @('CampusNet-Keeper.exe', 'cnk_tests.exe', 'cnk_ipc_probe.exe')
foreach ($a in $artifacts) {
    $src = Join-Path $devBuild "$Config\$a"
    if (Test-Path $src) { Copy-Item $src (Join-Path $outDir $a) -Force }
}
Get-ChildItem $outDir -Filter '*.exe' | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host

# ---------------------------------------------------------------- 7) 校验（防的正是"跑了旧二进制"）
Step '校验产物（防止用户运行到旧二进制）'
$exe = Join-Path $outDir 'CampusNet-Keeper.exe'
if (-not (Test-Path $exe)) { Die "主程序未安装：$exe" }

# 用"新增功能的日志标记"当指纹：它能证明装上去的确实是本次构建
$markers = @('IPC 已就绪')
foreach ($m in $markers) {
    $n = (Select-String -Path $exe -Pattern $m -Encoding utf8 -ErrorAction SilentlyContinue |
          Measure-Object).Count
    if ($n -lt 1) { Die "安装在 $outDir 的主程序不含标记『$m』——装到的不是本次构建的产物" }
    Ok "主程序含标记『$m』"
}

$srcExe = Join-Path $devBuild "$Config\CampusNet-Keeper.exe"
$h1 = (Get-FileHash $srcExe -Algorithm SHA256).Hash
$h2 = (Get-FileHash $exe -Algorithm SHA256).Hash
if ($h1 -ne $h2) { Die '安装后的主程序与刚构建的哈希不一致（复制被干扰？）' }
Ok "哈希一致：$($h2.Substring(0, 16))…"

# 任务 26：形态自检——真的去读 exe 资源 / 机器上的 .NET，而不是只看文件大小
$cli = Invoke-CnkCli -Exe $exe -CliArgs @('--ui-info')
$uiInfo = $cli.Lines
$uiInfo | ForEach-Object { '  ' + $_ }
$uiRc = $cli.ExitCode
if ($SingleExe) {
    if ($uiRc -ne 0) {
        Die "--ui-info 退出码 $uiRc（完整版应为 0：载荷结构/CRC 可解析且找到 .NET 运行时）。若只是本机没装 .NET，请先装 .NET 10 运行时"
    }
    if (-not ($uiInfo -match '全部为存储型')) { Die '载荷含压缩条目：运行时会被明确拒绝（用 scripts/embed_ui.ps1 重新打包）' }
    if (-not ($uiInfo -match '载荷校验: 通过')) { Die '载荷干跑校验未通过（见上面 --ui-info 输出）' }
    Ok '完整版自检通过：内嵌载荷（存储型 + 干跑校验）与 hostfxr 均就绪'
} else {
    if ($uiRc -ne 3) { Die "精简版 --ui-info 应返回 3（无内嵌载荷），实际 $uiRc" }
    Ok '精简版自检通过：确认未嵌入界面载荷'
}

# ---------------------------------------------------------------- 打包（可选）
if ($Package) {
    Step '额外：打包发布压缩包'
    $zip = Join-Path $root 'build\CampusNet-Keeper-win-x64.zip'
    Remove-Item $zip -ErrorAction SilentlyContinue
    $stage = Join-Path $root 'build\stage'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item (Join-Path $outDir 'CampusNet-Keeper.exe') $stage
    foreach ($doc in @('README.md', 'LICENSE', 'DESIGN.md', 'docs')) {
        $p = Join-Path $root $doc
        if (Test-Path $p) { Copy-Item $p $stage -Recurse -Force }
    }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Ok "已打包：$zip（$([math]::Round((Get-Item $zip).Length / 1KB)) KB）"
}

Write-Host "`n构建完成 ✓" -ForegroundColor Green
Write-Host "  守护进程: $outDir\CampusNet-Keeper.exe$(if ($SingleExe) { '（完整版：含内嵌设置界面）' })"
if (-not $SingleExe -and (Test-Path $uiExe)) { Write-Host "  设置界面: $uiExe" }
if ($SingleExe) { Write-Host "  设置界面: 由主程序自身 --ui 提供（无需额外文件）" }
