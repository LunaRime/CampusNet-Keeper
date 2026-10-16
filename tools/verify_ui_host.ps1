<#
.SYNOPSIS
  任务 26 的真机验证：单 exe 双形态 —— 载荷释放 → hostfxr 加载 .NET → 界面窗口出现
  → 关窗后进程真的退出 → 二次运行命中版本戳（不重复解压）。

.DESCRIPTION
  ⚠ 必须在**非受限沙箱**下运行：Avalonia 的 Win32 初始化与 %LOCALAPPDATA% 写入都会被
  受限沙箱拦（见 src/ui/README.md 的环境注意事项）。

  为什么用 .NET 的 ProcessStartInfo 而不是 Start-Process：
  `CreateNoWindow = $true` 对应 CreateProcess 的 CREATE_NO_WINDOW，与守护进程
  LaunchUiProcess() 的真实启动方式一致（控制台子系统程序不加这个标志会闪黑窗）。

.PARAMETER Exe
  要验证的可执行文件（默认 build\Release\CampusNet-Keeper.exe）。

.PARAMETER WindowTimeoutSec
  等窗口出现的上限秒数（首次要解压 22MB + 加载 CLR，给宽一点）。

.PARAMETER SkipReset
  不删除已释放的 %LOCALAPPDATA%\CampusNet-Keeper\ui（默认删除以验证"首次释放"）。

.EXAMPLE
  pwsh -File tools/verify_ui_host.ps1
#>
[CmdletBinding()]
param(
    [string]$Exe = 'build\Release\CampusNet-Keeper.exe',
    [int]$WindowTimeoutSec = 60,
    [switch]$SkipReset
)

$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }

$script:failed = 0
function Ok($m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Bad($m) { Write-Host "  [失败] $m" -ForegroundColor Red; $script:failed++ }
function Info($m) { Write-Host "  $m" -ForegroundColor DarkGray }
function Title($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Check($cond, $m) { if ($cond) { Ok $m } else { Bad $m } }

$uiDir = Join-Path $env:LOCALAPPDATA 'CampusNet-Keeper\ui'
$logPath = Join-Path $env:APPDATA 'CampusNet-Keeper\logs\keeper.log'

if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe（先跑 build.ps1 -SingleExe）" }
Write-Host "验证对象：$Exe（$([math]::Round((Get-Item $Exe).Length / 1MB, 1)) MB）" -ForegroundColor White

function Read-LogText {
    if (-not (Test-Path $logPath)) { return '' }
    try { return (Get-Content -Raw -Path $logPath -ErrorAction Stop) } catch { return '' }
}

function Start-UiProcess {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.Arguments = '--ui'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true          # 与守护进程 LaunchUiProcess 的 CREATE_NO_WINDOW 一致
    return [System.Diagnostics.Process]::Start($psi)
}

function Wait-Window($proc, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) { return $null }
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { return $proc }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Stop-UiProcess($proc, [int]$timeoutSec = 20) {
    if ($proc.HasExited) { return $true }
    $null = $proc.CloseMainWindow()
    if ($proc.WaitForExit($timeoutSec * 1000)) { return $true }
    return $false
}

# 带"设置界面"窗口的进程。刻意按标题筛：只数 CampusNet-Keeper 进程的话，
# 万一有守护进程带控制台窗口在跑，会被误算成"第二个界面"（复核 P2-4 的噪声提示）
function Get-UiWindows {
    return @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
             Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and
                            $_.MainWindowTitle -match '设置' })
}

# ---------------------------------------------------------------- 1) 前置自检
Title '1) --ui-info：载荷与 hostfxr 就绪'
$infoOut = & $Exe --ui-info 2>&1
$infoOut | ForEach-Object { Info $_ }
$infoRc = $LASTEXITCODE
Check ($infoRc -eq 0) "--ui-info 退出码 0（实际 $infoRc）"
$payloadId = $null
foreach ($line in $infoOut) {
    $m = [regex]::Match([string]$line, '版本戳:\s*(\S+)')
    if ($m.Success) { $payloadId = $m.Groups[1].Value }
}
Check ($null -ne $payloadId) "解析到版本戳：$payloadId"

# ---------------------------------------------------------------- 2) 首次释放
Title '2) 首次运行：应解压载荷并弹出界面'
if (-not $SkipReset) {
    # 删除前先确认解析出的绝对路径就是预期目录（绝不删计算出来的陌生路径）
    $resolvedUi = [System.IO.Path]::GetFullPath($uiDir)
    $expected = [System.IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'CampusNet-Keeper\ui'))
    if ($resolvedUi -ne $expected) { throw "拒绝删除：$resolvedUi ≠ $expected" }
    if (Test-Path $resolvedUi) { Remove-Item $resolvedUi -Recurse -Force; Info "已删除旧目录：$resolvedUi" }
    else { Info "目录本来就不存在：$resolvedUi" }
}

$logBefore = Read-LogText
$p1 = Start-UiProcess
Info "已启动 --ui 进程，PID=$($p1.Id)"
$w1 = Wait-Window $p1 $WindowTimeoutSec
if ($null -eq $w1) {
    Bad "等不到界面窗口（超时 ${WindowTimeoutSec}s，进程 HasExited=$($p1.HasExited)）"
    if (-not $p1.HasExited) { $p1.Kill() }
} else {
    Ok "界面窗口已出现：标题『$($p1.MainWindowTitle)』"
}

Check (Test-Path (Join-Path $uiDir 'CampusNet.UI.dll')) '载荷已释放到 %LOCALAPPDATA%\CampusNet-Keeper\ui'
$marker = Join-Path $uiDir '.version'
if (Test-Path $marker) {
    $onDisk = (Get-Content -Raw $marker).Trim()
    Check ($onDisk -eq $payloadId) "版本戳文件与载荷一致（$onDisk）"
} else {
    Bad '缺少 .version 版本戳文件'
}

$logAfter = Read-LogText
$newLog = if ($logAfter.Length -ge $logBefore.Length) { $logAfter.Substring($logBefore.Length) } else { $logAfter }
Check ($newLog -match 'UI 载荷已就绪|解压 UI 载荷到') '日志出现「解压载荷」记录'
Check ($newLog -match '已加载 hostfxr') '日志出现「已加载 hostfxr」记录'
Check ($newLog -match '即将进入托管界面入口') '日志出现「进入托管入口」记录'

# ---------------------------------------------------------------- 3) 关窗退出
Title '3) 关闭窗口后进程必须真的退出（不能留僵尸）'
$exited = Stop-UiProcess $p1 20
Check $exited '关窗后进程已退出'
if (-not $exited) { $p1.Kill() }

# ---------------------------------------------------------------- 4) 二次运行：版本戳命中
Title '4) 二次运行：应复用已释放的载荷（不重复解压）'
$markerTimeBefore = (Get-Item $marker).LastWriteTimeUtc
$logBefore2 = Read-LogText
$p2 = Start-UiProcess
$w2 = Wait-Window $p2 $WindowTimeoutSec
if ($null -eq $w2) {
    Bad "二次运行等不到窗口（超时 ${WindowTimeoutSec}s）"
    if (-not $p2.HasExited) { $p2.Kill() }
} else {
    Ok "界面窗口已出现：标题『$($p2.MainWindowTitle)』"
}
$logAfter2 = Read-LogText
$newLog2 = if ($logAfter2.Length -ge $logBefore2.Length) { $logAfter2.Substring($logBefore2.Length) } else { $logAfter2 }
Check ($newLog2 -match '复用已解压的 UI 载荷') '日志出现「复用已解压的载荷」记录'
Check ($newLog2 -notmatch '解压 UI 载荷到') '日志没有重复解压'
$markerTimeAfter = (Get-Item $marker).LastWriteTimeUtc
Check ($markerTimeAfter -eq $markerTimeBefore) '版本戳文件未被改写'
$exited2 = Stop-UiProcess $p2 20
Check $exited2 '关窗后进程已退出'

# ---------------------------------------------------------------- 5) 单实例
Title '5) 界面已在运行时，再开一个应"正常退出"而不是崩溃'
$p3 = Start-UiProcess
$null = Wait-Window $p3 $WindowTimeoutSec
$logBefore3 = Read-LogText
$p4 = Start-UiProcess
$second = $p4.WaitForExit(15000)
Check $second '第二个 --ui 进程在 15 秒内退出'
if ($second) {
    # 只断言"退出了"是不够的：缺 .NET、载荷坏等早期失败也会退出（审阅 P2-4）
    Check ($p4.ExitCode -eq 0) "第二个 --ui 退出码为 0（实际 $($p4.ExitCode)）"
    $tail3 = if ((Read-LogText).Length -ge $logBefore3.Length) {
        (Read-LogText).Substring($logBefore3.Length)
    } else { Read-LogText }
    Check ($tail3 -match '已有设置界面在运行') '日志确认是"已有界面在运行"而不是早期失败'
} else {
    $p4.Kill()
}
$null = Stop-UiProcess $p3 20

# ---------------------------------------------------------------- 6) 真并发
Title '6) 真并发：同时启动两个 --ui，最终只应有一个窗口'
$pa = Start-UiProcess
$pb = Start-UiProcess
$deadline = (Get-Date).AddSeconds($WindowTimeoutSec)
$winner = $null
while ((Get-Date) -lt $deadline) {
    $with = Get-UiWindows
    if ($with.Count -ge 1) { $winner = $with[0]; break }
    Start-Sleep -Milliseconds 300
}
Check ($null -ne $winner) '并发启动后出现了界面窗口'
Start-Sleep -Seconds 3        # 给输的一方时间退出
$wins = Get-UiWindows
Check ($wins.Count -eq 1) "只有一个界面窗口（实际 $($wins.Count)）"
$loser = @($pa, $pb) | Where-Object { $_.HasExited } | Select-Object -First 1
Check ($null -ne $loser) '其中一个进程已退出'
if ($loser) { Check ($loser.ExitCode -eq 0) "退出方退出码为 0（实际 $($loser.ExitCode)）" }
foreach ($p in @($pa, $pb)) {
    if (-not $p.HasExited) { $null = Stop-UiProcess $p 20 }
}

Write-Host ''
if ($script:failed -eq 0) {
    Write-Host '任务 26 真机验证：全部通过 ✓' -ForegroundColor Green
    exit 0
} else {
    Write-Host "任务 26 真机验证：$($script:failed) 项失败 ✗" -ForegroundColor Red
    exit 1
}
