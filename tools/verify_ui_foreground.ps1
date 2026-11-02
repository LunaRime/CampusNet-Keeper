<#
.SYNOPSIS
  验证「第二次 --ui 会把已打开的设置界面提到前台」（而不是默默退出）。

.DESCRIPTION
  2026-11-02 之前的行为：已有界面在跑时，第二次 --ui 只写一行日志就退出（
  `已有设置界面在运行，本次不再打开新窗口`）。用户点托盘「设置…」看到的是
  "什么都没发生"——窗口被别的程序压在后面。

  现在的行为：按**进程映像路径**枚举顶层窗口（不按标题，标题是 UI 文案，改一个字就失效），
  找到后 ShowWindow(SW_RESTORE) + SetForegroundWindow。

  怎么判定真的生效（不是只看"没报错"）：
    ① 先开一个干扰窗口（记事本）并让它拿到前台；
    ② 再跑一次 --ui；
    ③ 断言前台窗口的标题 == 设置界面标题，且日志出现「切换到前台：成功」。

  ⚠ 会短暂弹出窗口并抢焦点；⚠ 会临时占用单实例名额，故先清掉已有实例。

.EXAMPLE
  pwsh -File tools/verify_ui_foreground.ps1
  pwsh -File tools/verify_ui_foreground.ps1 -Exe build\Release\CampusNet-Keeper.exe -TimeoutSec 60
#>
[CmdletBinding()]
param(
    [string]$Exe = 'build\Release\CampusNet-Keeper.exe',
    [int]$TimeoutSec = 45
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
if (-not (Test-Path $Exe)) { Write-Host "[失败] 找不到 $Exe" -ForegroundColor Red; exit 1 }

$script:failed = 0
function Ok($m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Bad($m) { Write-Host "  [失败] $m" -ForegroundColor Red; $script:failed++ }
function Title($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

# Win32：按进程名找顶层窗口（可见、有标题、无 owner），以及当前前台窗口
if (-not ('CnkWin' -as [type])) {
Add-Type @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public class CnkWin {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static string Title(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
  public static IntPtr FindByProcess(string procName) {
    var pids = new HashSet<uint>();
    foreach (var p in Process.GetProcessesByName(procName)) pids.Add((uint)p.Id);
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (!pids.Contains(pid)) return true;
      if (!IsWindowVisible(h)) return true;
      if (GetWindow(h, 4) != IntPtr.Zero) return true;   // GW_OWNER
      if (GetWindowTextLength(h) == 0) return true;
      found = h; return false;
    }, IntPtr.Zero);
    return found;
  }
}
"@
}

$logPath = Join-Path $env:APPDATA 'CampusNet-Keeper\logs\keeper.log'
function Log-Len { if (Test-Path $logPath) { return (Get-Content -Raw $logPath).Length } else { return 0 } }
function Log-Tail([int]$baseline) {
    if (-not (Test-Path $logPath)) { return '' }
    $all = Get-Content -Raw $logPath
    if ($all.Length -ge $baseline) { return $all.Substring($baseline) }
    return $all
}

Get-Process CampusNet-Keeper -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$baseline = Log-Len

Title '1. 启动设置界面'
Start-Process $Exe -ArgumentList '--ui'
$hwnd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $hwnd = [CnkWin]::FindByProcess('CampusNet-Keeper')
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 400
}
if ($hwnd -eq [IntPtr]::Zero) { Bad "窗口在 $TimeoutSec 秒内没出现"; exit 1 }
$uiTitle = [CnkWin]::Title($hwnd)
Ok "窗口已出现：[$uiTitle]"

Title '2. 让别的窗口占住前台'
$notepad = Start-Process notepad.exe -PassThru
Start-Sleep -Seconds 2
$before = [CnkWin]::Title([CnkWin]::GetForegroundWindow())
Ok "干扰窗口拿到前台：[$before]"

Title '3. 再启动一次 --ui（应把设置界面提到前台）'
Start-Process $Exe -ArgumentList '--ui' -Wait
Start-Sleep -Seconds 2
$after = [CnkWin]::Title([CnkWin]::GetForegroundWindow())
$tail = Log-Tail $baseline

if ($after -eq $uiTitle) { Ok "前台已切回设置界面：[$after]" }
else { Bad "前台仍是：[$after]（期望 [$uiTitle]）" }
if ($tail -match '切换到前台：成功') { Ok '日志：切换到前台：成功' }
else { Bad '日志里没有「切换到前台：成功」' }
# 单实例：本次基线之后只应有**一个**进程走到托管入口（第一次的那个）；
# 第二次应当只做"提前台"就退出。数错一次就会把正常当异常（本脚本第一版就这么错过）。
$entries = @($tail -split "`r?`n" | Where-Object { $_ -match '即将进入托管界面入口' }).Count
if ($entries -eq 1) { Ok '没有重复创建第二个界面进程（托管入口只进了一次）' }
else { Bad "托管入口进入了 $entries 次，期望 1 次（单实例可能失效）" }

# 收尾：关掉干扰窗口与界面
if ($notepad -and -not $notepad.HasExited) { Stop-Process -Id $notepad.Id -Force -ErrorAction SilentlyContinue }
Get-Process CampusNet-Keeper -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host ''
if ($script:failed -eq 0) {
    Write-Host '全部通过：第二次 --ui 会把已有窗口提到前台' -ForegroundColor Green
    exit 0
}
Write-Host "$($script:failed) 项失败" -ForegroundColor Red
exit 1
