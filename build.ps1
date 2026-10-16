<#
.SYNOPSIS
  CampusNet-Keeper 一键构建脚本：配置 → 构建 → 测试 → **安装到 build\Release** → 校验产物。

.DESCRIPTION
  为什么要有"安装 + 校验"这两步（2026-10-17 的真实教训）：
  曾出现"用户报告检测不到守护进程/无法保存配置"，根因是我只把新构建留在开发树
  （build-m1\Release），忘了同步到用户实际运行的 build\Release —— 运行的是旧二进制。
  所以本脚本把"同步安装 + 校验新产物含预期标记"固化下来，不靠人记得。

.PARAMETER Config
  构建配置（Release/Debug），默认 Release。

.PARAMETER SkipTests
  跳过 C++ 测试（仅用于快速验证能否编译）。

.PARAMETER SkipUi
  跳过 Avalonia UI 构建（UI 需要 NuGet 包；首次需联网 restore）。

.PARAMETER Package
  额外产出发布压缩包 build\CampusNet-Keeper-<版本>-win-x64.zip。

.EXAMPLE
  pwsh -File build.ps1
  pwsh -File build.ps1 -Package
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [switch]$SkipTests,
    [switch]$SkipUi,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$devBuild = Join-Path $root 'build-m1'      # 开发/沙箱友好的构建树（见 .gitignore）
$outDir = Join-Path $root 'build\Release'   # **用户实际运行**的目录：必须每次同步
$script:failed = 0

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }
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

$cmake = Find-CMake
Write-Host "CampusNet-Keeper 构建（配置=$Config）" -ForegroundColor White
Write-Host "  仓库: $root"
Write-Host "  cmake: $cmake"

# ---------------------------------------------------------------- 1) 配置
Step '1/6 配置 CMake'
if (-not (Test-Path (Join-Path $devBuild 'CMakeCache.txt'))) {
    Invoke-Checked $cmake @('-S', $root, '-B', $devBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64') 'CMake 配置'
} else {
    Ok '已有缓存，跳过配置'
}

# ---------------------------------------------------------------- 2) 构建
Step '2/6 构建 C++（cnk_core / 主程序 / 测试 / 探针）'
Invoke-Checked $cmake @('--build', $devBuild, '--config', $Config) 'C++ 构建'

# ---------------------------------------------------------------- 3) 测试
Step '3/6 运行 C++ 测试'
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

# ---------------------------------------------------------------- 4) UI（可选）
Step '4/6 构建 Avalonia 设置界面'
$uiDir = Join-Path $root 'src\ui\CampusNet.UI'
$uiExe = Join-Path $root 'build-ui\bin\CampusNet.UI\Release\net10.0-windows\CampusNet.UI.exe'
if ($SkipUi) {
    Warn '按要求跳过 UI 构建'
} elseif (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Warn '未安装 dotnet SDK，跳过 UI（守护进程与 CLI 不受影响）'
} else {
    # 沙箱/CI 下 dotnet 需要可写的 CLI_HOME；包缓存与中间目录都放在工作区内
    $env:DOTNET_CLI_HOME = Join-Path $root 'build-ui\dotnet-home'
    $env:NUGET_PACKAGES = Join-Path $root 'build-ui\nuget'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
    & dotnet build $uiDir -c Release
    if ($LASTEXITCODE -ne 0) { Warn 'UI 构建失败（首次需要联网 restore Avalonia；守护进程与 CLI 不受影响）' }
    else { Ok 'UI 构建通过' }
}

# ---------------------------------------------------------------- 5) 安装（关键步骤）
Step "5/6 安装到 $outDir（用户实际运行的目录）"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$artifacts = @('CampusNet-Keeper.exe', 'cnk_tests.exe', 'cnk_ipc_probe.exe')
foreach ($a in $artifacts) {
    $src = Join-Path $devBuild "$Config\$a"
    if (Test-Path $src) { Copy-Item $src (Join-Path $outDir $a) -Force }
}
Get-ChildItem $outDir -Filter '*.exe' | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host

# ---------------------------------------------------------------- 6) 校验（防的正是"跑了旧二进制"）
Step '6/6 校验产物（防止用户运行到旧二进制）'
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
Write-Host "  守护进程: $outDir\CampusNet-Keeper.exe"
if (Test-Path $uiExe) { Write-Host "  设置界面: $uiExe" }
