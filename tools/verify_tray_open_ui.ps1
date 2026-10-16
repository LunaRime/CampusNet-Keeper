<#
.SYNOPSIS
  任务 26：验证托盘「打开设置界面」真的会拉起 --ui 子进程（而不是旧版的占位气泡）。

.DESCRIPTION
  用 `--simulate=drcom` 起守护进程：所有地址都被指向本机模拟服务器（只改内存配置、
  不写盘），因此**不会碰真实校园网**。然后以"再启一个自身实例"的方式触发
  SingleInstance::NotifyExistingInstance() → 守护进程隐藏窗口收到 kMsgShowUiRequest
  → 与点击托盘菜单「设置…」完全相同的那条回调 → LaunchUiProcess() → 新进程 --ui。

  ⚠ 非受限沙箱下运行（GUI + %LOCALAPPDATA% 写入）。
  ⚠ 会临时占用本机守护进程的单实例名额，因此脚本先确认没有正在运行的守护进程。

.EXAMPLE
  pwsh -File tools/verify_tray_open_ui.ps1 -Exe build\Release\CampusNet-Keeper.exe
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
function Log-Tail([string]$baseline) {
    $text = Read-LogText
    if ($null -ne $baseline -and $text.Length -ge $baseline.Length) { return $text.Substring($baseline.Length) }
    return $text
}
# 日志是异步写的（守护进程在自己的消息循环里处理请求）→ 必须轮询，不能读一次就断言
function Wait-LogMatch([string]$pattern, [string]$baseline, [int]$timeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $tail = Log-Tail $baseline
        if ($tail -match $pattern) { return $tail }
        Start-Sleep -Milliseconds 200
    }
    return $null
}
function Start-Ui([string]$arguments) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true      # 与守护进程 LaunchUiProcess 的 CREATE_NO_WINDOW 一致
    return [System.Diagnostics.Process]::Start($psi)
}
function Wait-UiWindow([int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $with = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
                  Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
        if ($with.Count -ge 1) { return $with[0] }
        Start-Sleep -Milliseconds 300
    }
    return $null
}
function Close-Ui($proc, [int]$timeoutSec = 20) {
    if (-not $proc -or $proc.HasExited) { return $true }
    $null = $proc.CloseMainWindow()
    return $proc.WaitForExit($timeoutSec * 1000)
}

if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe" }
$running = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $running | Select-Object Id, ProcessName, MainWindowTitle | Format-Table -AutoSize | Out-String | Write-Host
    throw '请先退出正在运行的 CampusNet-Keeper，再跑本验证（避免与真实守护抢单实例）'
}

$daemon = $null
$uiFromTray = $null
$uiDirect = $null

try {
    # ------------------------------------------------------------ 1) 起模拟守护
    Title '1) 启动模拟守护进程（--simulate=drcom，不碰真实网络）'
    $daemon = Start-Ui '--simulate=drcom'
    Info "守护 PID=$($daemon.Id)"
    $baseline = Read-LogText
    Check ($null -ne (Wait-LogMatch '进入托盘守护循环' $baseline $TimeoutSec)) '守护循环已进入（托盘图标已就绪）'
    Check ($null -ne (Wait-LogMatch '设置界面 IPC 已就绪' $baseline $TimeoutSec)) 'IPC 已就绪'

    # ------------------------------------------------------------ 2) 触发"打开设置界面"
    Title '2) 触发打开设置界面（第二实例通知 = 托盘菜单「设置…」的同一条回调）'
    $baseline = Read-LogText
    $trigger = Start-Ui ''
    $trigger.WaitForExit(15000) | Out-Null
    Check ($null -ne (Wait-LogMatch '已启动设置界面进程（--ui）' $baseline 20)) '守护进程日志确认已启动 --ui 子进程'

    $uiFromTray = Wait-UiWindow $TimeoutSec
    Check ($null -ne $uiFromTray) '设置界面窗口已出现（新进程）'
    if ($uiFromTray) {
        Info "界面 PID=$($uiFromTray.Id) 标题=『$($uiFromTray.MainWindowTitle)』"
        Check ($uiFromTray.MainWindowTitle -match '设置') '窗口标题是设置界面'
    }
    Check ($null -ne (Wait-LogMatch '即将进入托管界面入口' $baseline 20)) '界面侧日志确认已进入托管入口'

    # ------------------------------------------------------------ 3) 已在运行时不该再开一个
    Title '3) 界面已在运行时：托盘请求只提示、不再开第二个窗口'
    $baseline = Read-LogText
    $trigger2 = Start-Ui ''
    $trigger2.WaitForExit(15000) | Out-Null
    Check ($null -ne (Wait-LogMatch '设置界面已在运行，忽略本次打开请求' $baseline 20)) '守护日志确认「已在运行，忽略请求」'
    Start-Sleep -Seconds 1
    $windows = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
                 Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
    Check ($windows.Count -eq 1) "仍然只有一个界面窗口（实际 $($windows.Count)）"

    # ------------------------------------------------------------ 4) 关窗 → 界面退出，守护继续
    Title '4) 关闭界面窗口：界面进程退出，守护进程继续跑（spec S5）'
    if ($uiFromTray) { Check (Close-Ui $uiFromTray) '关窗后界面进程退出' }
    Start-Sleep -Seconds 1
    Check (-not $daemon.HasExited) '守护进程仍在运行'

    # 不只看"进程还在"，还要它能应答：关窗后 IPC 仍应正常（真的还在工作）
    $sid = ([System.Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
    $pingOk = $false
    try {
        $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
            '.', "CampusNetKeeper.$sid", [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(3000)
        $writer = [System.IO.StreamWriter]::new($pipe, [System.Text.UTF8Encoding]::new($false))
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true
        $writer.WriteLine('{"id":1,"cmd":"ping"}')
        $reader = [System.IO.StreamReader]::new($pipe, [System.Text.UTF8Encoding]::new($false))
        $task = $reader.ReadLineAsync()
        if ($task.Wait(5000)) {
            $line = $task.Result
            Info "ping 应答：$line"
            $pingOk = ($line -match '"ok"\s*:\s*true')
        } else {
            Bad 'IPC ping 应答超时（5 秒）'
        }
        $pipe.Dispose()
    } catch {
        Bad "IPC ping 失败：$($_.Exception.Message)"
    }
    Check $pingOk '关窗后 IPC 仍能应答 ping（守护真的还在工作）'
} finally {
    # ------------------------------------------------------------ 收尾
    Title '5) 收尾'
    foreach ($p in @($uiFromTray, $uiDirect)) {
        if ($p -and -not $p.HasExited) { $null = $p.CloseMainWindow(); if (-not $p.WaitForExit(10000)) { $p.Kill() } }
    }
    if ($daemon -and -not $daemon.HasExited) {
        # 给守护进程的隐藏窗口发 WM_CLOSE（TrayIcon::WndProc 的 WM_DESTROY → PostQuitMessage）。
        # 注意：这里必须用 EnumWindows 找窗口——实测 PowerShell 里 FindWindowW 对
        # 该隐藏窗口恒返回 0，而 C++ 侧（产品自己的 NotifyExistingInstance）用 FindWindowW
        # 是能找到的，故与本功能无关，只是别在脚本里踩这个坑。
        try {
            Add-Type -Namespace CnkVerify -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumProc cb, System.IntPtr p);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr p);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder sb, int max);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr hWnd, uint Msg, System.IntPtr wParam, System.IntPtr lParam);
'@ -ErrorAction Stop
            $script:hidden = [IntPtr]::Zero
            $cb = [CnkVerify.Win32+EnumProc]{
                param($h, $p)
                $sb = [System.Text.StringBuilder]::new(256)
                [void][CnkVerify.Win32]::GetClassNameW($h, $sb, 256)
                if ($sb.ToString() -eq 'CampusNetKeeperHidden') { $script:hidden = $h; return $false }
                return $true
            }
            [void][CnkVerify.Win32]::EnumWindows($cb, [IntPtr]::Zero)
            if ($script:hidden -ne [IntPtr]::Zero) {
                [void][CnkVerify.Win32]::PostMessageW($script:hidden, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
                if ($daemon.WaitForExit(10000)) { Ok '守护进程已干净退出（与托盘菜单「退出」同一条路径）' }
                else { Bad '守护进程未在 10 秒内退出' }
            } else {
                Bad '找不到守护进程的隐藏窗口，无法干净退出'
            }
        } catch {
            Bad "发送退出消息失败：$($_.Exception.Message)"
        }
        if (-not $daemon.HasExited) { $daemon.Kill(); Info '已强制结束守护进程（测试用实例）' }
    }
    Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue | ForEach-Object {
        Info "兜底结束残留进程 PID=$($_.Id)"
        try { $_.Kill() } catch { }
    }
}

Write-Host ''
if ($script:failed -eq 0) {
    Write-Host '托盘打开设置界面：全部通过 ✓' -ForegroundColor Green
    exit 0
} else {
    Write-Host "托盘打开设置界面：$($script:failed) 项失败 ✗" -ForegroundColor Red
    exit 1
}
