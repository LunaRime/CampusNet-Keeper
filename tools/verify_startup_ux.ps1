<#
.SYNOPSIS
  启动体验的真机验证：① 无参数启动（=双击 exe）后**自动打开设置界面**；
  ② 启动后**不留下可见终端窗口**（Win11 的 Windows Terminal 尤其要盯），关掉窗口也不会影响守护；
  ③ 开机自启形态（--autostart）**不**弹界面；④ 托盘图标确实用的是应用图标。

.DESCRIPTION
  ⚠ 需要 GUI 与 %APPDATA%/%LOCALAPPDATA% 写入，必须在非受限沙箱下运行。
  ⚠ 会占用守护进程单实例名额，脚本启动前要求没有正在运行的 CampusNet-Keeper。

  托盘图标的判定用**日志断言**（"托盘图标：应用图标 IDI_APPICON"）而不是截图：
  Win11 默认把新托盘图标收进溢出区，截屏既拍不到、还会把用户桌面拍进去。

.PARAMETER Exe
  被测可执行文件（默认 build\Release\CampusNet-Keeper.exe；建议先用 -SingleExe 构建）。

.EXAMPLE
  pwsh -File tools/verify_startup_ux.ps1 -Exe build-m1\Release\CampusNet-Keeper.exe
#>
[CmdletBinding()]
param(
    [string]$Exe = 'build\Release\CampusNet-Keeper.exe',
    [int]$TimeoutSec = 60
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

$logPath = Join-Path $env:APPDATA 'CampusNet-Keeper\logs\keeper.log'
function Read-LogText {
    if (-not (Test-Path $logPath)) { return '' }
    try { return (Get-Content -Raw -Path $logPath -ErrorAction Stop) } catch { return '' }
}
function Wait-LogMatch([string]$pattern, [string]$baseline, [int]$timeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $text = Read-LogText
        $tail = if ($null -ne $baseline -and $text.Length -ge $baseline.Length) {
            $text.Substring($baseline.Length)
        } else { $text }
        if ($tail -match $pattern) { return $tail }
        Start-Sleep -Milliseconds 250
    }
    return $null
}
function Get-UiWindows {
    return @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
             Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and
                            $_.MainWindowTitle -match '设置' })
}

# 让守护进程干净退出：给隐藏窗口发 WM_CLOSE（与托盘菜单「退出」同一条路径）
if (-not ('CnkUx.Win32' -as [type])) {
    Add-Type -Namespace CnkUx -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumProc cb, System.IntPtr p);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr p);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder sb, int max);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr hWnd, uint Msg, System.IntPtr wParam, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetProcessDPIAware();
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr hWnd);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowTextW(System.IntPtr hWnd, System.Text.StringBuilder sb, int max);
'@
}
# 可见的终端/控制台窗口（Win11 默认终端是 CASCADIA_HOSTING_WINDOW_CLASS）
function Get-VisibleTerminalWindows {
    $script:termWins = @()
    $cb = [CnkUx.Win32+EnumProc]{
        param($h, $p)
        $sb = [System.Text.StringBuilder]::new(256)
        [void][CnkUx.Win32]::GetClassNameW($h, $sb, 256)
        $cls = $sb.ToString()
        if (($cls -eq 'ConsoleWindowClass' -or $cls -eq 'CASCADIA_HOSTING_WINDOW_CLASS') -and
            [CnkUx.Win32]::IsWindowVisible($h)) {
            $tb = [System.Text.StringBuilder]::new(256)
            [void][CnkUx.Win32]::GetWindowTextW($h, $tb, 256)
            $script:termWins += "$cls|$($tb.ToString())"
        }
        return $true
    }
    [void][CnkUx.Win32]::EnumWindows($cb, [IntPtr]::Zero)
    return @($script:termWins)
}
function Stop-DaemonCleanly([int]$timeoutSec = 15) {
    $script:hidden = [IntPtr]::Zero
    $cb = [CnkUx.Win32+EnumProc]{
        param($h, $p)
        $sb = [System.Text.StringBuilder]::new(256)
        [void][CnkUx.Win32]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'CampusNetKeeperHidden') { $script:hidden = $h; return $false }
        return $true
    }
    [void][CnkUx.Win32]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:hidden -eq [IntPtr]::Zero) { return $false }
    [void][CnkUx.Win32]::PostMessageW($script:hidden, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
function Screenshot-TrayArea([string]$outPath) {
    Add-Type -AssemblyName System.Drawing, System.Windows.Forms -ErrorAction Stop
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $w = [Math]::Min(620, $b.Width)
    $h = [Math]::Min(80, $b.Height)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Width - $w, $b.Height - $h, 0, 0, $bmp.Size)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
    $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $outPath
}

if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe" }
$running = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $running | Select-Object Id, MainWindowTitle | Format-Table -AutoSize | Out-String | Write-Host
    throw '请先退出正在运行的 CampusNet-Keeper（本脚本要占用单实例名额）'
}

# ---------------------------------------------------------------- 1) 双击形态
Title '1) 无参数启动（= 双击 exe）：应自动打开设置界面，且不留下终端窗口'
$baseline = Read-LogText
$daemon = Start-Process -FilePath $Exe -PassThru   # 默认 ShellExecute，最接近双击
Info "启动 PID=$($daemon.Id)（无参数；GUI 子系统，本进程直接带界面）"

# GUI 子系统：没有被分配控制台，因此不存在"分离重启"这一步，也不会留下终端窗口
Check ($null -ne (Wait-LogMatch '启动后自动打开设置界面（双击意图）' $baseline 30)) '守护进程日志确认「启动后自动打开设置界面」'
# 托盘图标：应用图标加载成功时守护进程会记一行（Win11 会把新图标收进溢出区，截图不可靠）
Check ($null -ne (Wait-LogMatch '托盘图标：应用图标' $baseline 20)) '托盘图标使用应用图标（日志确认 IDI_APPICON 已加载）'

$ui = $null
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $ui = Get-UiWindows | Select-Object -First 1
    if ($ui) { break }
    Start-Sleep -Milliseconds 300
}
Check ($null -ne $ui) '设置界面窗口已自动出现'
if ($ui) { Info "界面 PID=$($ui.Id) 标题=『$($ui.MainWindowTitle)』" }

# ★ 用户报的 bug：启动后不能留下终端窗口（Win11 的 Windows Terminal 尤其要盯）
Start-Sleep -Seconds 2      # 父进程退出后终端标签应已关闭
$terms = Get-VisibleTerminalWindows
if ($terms.Count -gt 0) { Info ("当前可见终端窗口: " + (($terms | Group-Object | ForEach-Object { "$($_.Name)" }) -join ' ; ')) }
Check (-not (@($terms) | Where-Object { $_ -match 'CampusNet' })) '启动后没有留下可见终端窗口（含 Windows Terminal）'


# 关掉界面，守护应继续
if ($ui) { $null = $ui.CloseMainWindow(); $null = $ui.WaitForExit(15000) }
Start-Sleep -Seconds 1
Check (@(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue).Count -ge 1) '关窗后守护进程继续运行'

# ---------------------------------------------------------------- 2) 自启形态
Title '2) 开机自启形态（--autostart）：守护启动但**不**弹界面、也不留终端'
# 退出必须留痕：否则"守护进程怎么没的"永远查不出来
#（2026-10-27 真机上出现过一次无声消失，日志里什么都留不下）
$exitBase = Read-LogText
Check (Stop-DaemonCleanly) '守护进程已干净退出（为下一步腾出单实例）'
$exitTail = Wait-LogMatch '守护循环退出，来源=' $exitBase 10
Check ($null -ne $exitTail) '退出时在日志里留下了退出来历'
if ($exitTail) {
    Check ($exitTail -notmatch '未记录（退出路径未识别）') '退出来历不是「未记录」（退出路径被正确识别）'
}
$baseline = Read-LogText
$auto = Start-Process -FilePath $Exe -ArgumentList '--autostart' -PassThru
Info "启动 PID=$($auto.Id)（--autostart；GUI 子系统，无控制台）"
Check ($null -ne (Wait-LogMatch '进入托盘守护循环' $baseline 30)) '守护循环已启动'
$winAuto = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
             Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
Check ($winAuto.Count -eq 0) '自启形态没有可见窗口（不闪终端）'
$ui2 = $null
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline) {
    $ui2 = Get-UiWindows | Select-Object -First 1
    if ($ui2) { break }
    Start-Sleep -Milliseconds 300
}
Check ($null -eq $ui2) '没有自动弹出设置界面（开机自启不应打扰用户）'
$terms2 = Get-VisibleTerminalWindows
Check (-not (@($terms2) | Where-Object { $_ -match 'CampusNet' })) '自启形态也没有留下可见终端窗口'
# 只检查**本次**启动新增的日志：整段日志里还留着上一步"自动打开设置界面"的记录
$autoTail = Read-LogText
$autoTail = if ($autoTail.Length -ge $baseline.Length) { $autoTail.Substring($baseline.Length) } else { $autoTail }
Check (-not ($autoTail -match '自动打开设置界面')) '本次自启的日志里没有「自动打开设置界面」'
Check (-not ($autoTail -match '启动意图=双击')) '本次自启没有被误判为双击'

# ---------------------------------------------------------------- 收尾
Title '3) 收尾'
$exitBase2 = Read-LogText
Check (Stop-DaemonCleanly) '守护进程已干净退出'
Check ($null -ne (Wait-LogMatch '守护循环退出，来源=' $exitBase2 10)) '退出时在日志里留下了退出来历'
$left = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue)
Check ($left.Count -eq 0) '无残留进程'

Write-Host ''
if ($script:failed -eq 0) {
    Write-Host '启动体验验证：全部通过 ✓' -ForegroundColor Green
    exit 0
} else {
    Write-Host "启动体验验证：$($script:failed) 项失败 ✗" -ForegroundColor Red
    exit 1
}
