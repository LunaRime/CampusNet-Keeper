<#
.SYNOPSIS
  开机自启端到端验证：
    ① 配置 autostart=true → 启动守护后注册表 Run 键应出现，且值是
       `"<exe>" --autostart`（Windows 登录时会执行它）；
    ② 按 `--autostart` 启动（模拟下次登录）：守护起来、**不弹设置界面**、不留下终端；
    ③ 配置 autostart=false → 启动守护后 Run 键应被删除；
    ④ 内部标志 `--no-autostart`（供其它测试脚本用）不得改动注册表。

.DESCRIPTION
  ⚠ 需要注册表写入、进程与 GUI 权限，必须在非受限沙箱下运行。
  ⚠ 会占用守护进程单实例名额，脚本启动前要求没有正在运行的 CampusNet-Keeper。
  ⚠ 用**临时配置**（--config）运行守护，不会读写用户的真实配置；脚本结束会把 Run 键值
     恢复成运行前的状态。

.PARAMETER Exe
  被测可执行文件（默认 build\Release\CampusNet-Keeper.exe）。

.EXAMPLE
  pwsh -File tools/verify_autostart.ps1
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

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$valueName = 'CampusNetKeeper'
$logPath = Join-Path $env:APPDATA 'CampusNet-Keeper\logs\keeper.log'
$tmpDir = Join-Path $env:TEMP ("cnk-autostart-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))

function Get-AutostartValue {
    $prop = Get-ItemProperty -Path $runKey -Name $valueName -ErrorAction SilentlyContinue
    if (-not $prop -or -not $prop.PSObject.Properties[$valueName]) { return $null }
    $v = $prop.$valueName
    # 空值视为"没有"（历史上出现过空 REG_SZ 残留，会让"是否写入"判断误报）
    if ([string]::IsNullOrEmpty($v)) { return $null }
    return [string]$v
}
# 注意：参数**不要**标 [string]——PowerShell 会把 $null 转成空串，
# 于是"删除"变成"写入一个空值"（本脚本自己踩过这个坑）。
function Set-AutostartValue($value) {
    if ([string]::IsNullOrEmpty($value)) {
        Remove-ItemProperty -Path $runKey -Name $valueName -ErrorAction SilentlyContinue
    } else {
        New-ItemProperty -Path $runKey -Name $valueName -Value $value -PropertyType String -Force | Out-Null
    }
}
function Read-LogText {
    if (-not (Test-Path $logPath)) { return '' }
    try { return (Get-Content -Raw -Path $logPath -ErrorAction Stop) } catch { return '' }
}
function Log-Delta([int]$baselineLen) {
    $t = Read-LogText
    if ($t.Length -ge $baselineLen) { return $t.Substring($baselineLen) }
    return $t
}
function Wait-LogMatch([string]$pattern, [int]$baselineLen, [int]$timeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Log-Delta $baselineLen) -match $pattern) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
function New-TestConfig([string]$path, [bool]$autostart) {
    $cfg = [ordered]@{
        version        = 1
        active_profile = ''
        profiles       = @()
        settings       = [ordered]@{
            autostart               = $autostart
            notify_on_reconnect     = $false
            log_level               = 'info'
            log_max_kb              = 512
            log_keep_files          = 3
            probe_url               = 'http://127.0.0.1:9/'
            probe_expect            = 'x'
            probe_interval_seconds  = 60
            probe_timeout_seconds   = 1
            retry_backoff_seconds   = @(60)
        }
    }
    $cfg | ConvertTo-Json -Depth 6 | Set-Content -Path $path -Encoding utf8
}
function Start-Daemon([string]$configPath, [switch]$NoAutostart) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.Arguments = "--config `"$configPath`" --detached"
    if ($NoAutostart) { $psi.Arguments += ' --no-autostart' }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    return [System.Diagnostics.Process]::Start($psi)
}
function Stop-Daemons {
    Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}
function Get-ProcessCount { return @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue).Count }
function Get-VisibleTerminalWindows {
    # Win11 默认终端是 Windows Terminal（CASCADIA_HOSTING_WINDOW_CLASS）
    if (-not ('CnkAuto.Win32' -as [type])) {
        Add-Type -Namespace CnkAuto -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumProc cb, System.IntPtr p);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr p);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder sb, int max);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowTextW(System.IntPtr h, System.Text.StringBuilder sb, int max);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr h);
'@
    }
    $script:wins = @()
    $cb = [CnkAuto.Win32+EnumProc]{
        param($h, $p)
        $sb = [System.Text.StringBuilder]::new(256)
        [void][CnkAuto.Win32]::GetClassNameW($h, $sb, 256)
        $cls = $sb.ToString()
        if (($cls -eq 'ConsoleWindowClass' -or $cls -eq 'CASCADIA_HOSTING_WINDOW_CLASS') -and
            [CnkAuto.Win32]::IsWindowVisible($h)) {
            $tb = [System.Text.StringBuilder]::new(256)
            [void][CnkAuto.Win32]::GetWindowTextW($h, $tb, 256)
            $script:wins += "$cls|$($tb.ToString())"
        }
        return $true
    }
    [void][CnkAuto.Win32]::EnumWindows($cb, [IntPtr]::Zero)
    return @($script:wins)
}
function Send-Ipc([string]$json, [int]$timeoutMs = 8000) {
    $sid = ([System.Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', "CampusNetKeeper.$sid", [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(3000)
        $writer = [System.IO.StreamWriter]::new($pipe, [System.Text.UTF8Encoding]::new($false))
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true
        $writer.WriteLine($json)
        $reader = [System.IO.StreamReader]::new($pipe, [System.Text.UTF8Encoding]::new($false))
        $task = $reader.ReadLineAsync()
        if ($task.Wait($timeoutMs)) { return $task.Result }
        return $null
    } finally {
        $pipe.Dispose()
    }
}

# ---------------------------------------------------------------- 前置检查
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe" }
$running = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw '请先退出正在运行的 CampusNet-Keeper（本脚本要占用单实例名额）'
}
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$cfgOn = Join-Path $tmpDir 'on.json'
$cfgOff = Join-Path $tmpDir 'off.json'
New-TestConfig $cfgOn $true
New-TestConfig $cfgOff $false

# set_config（设置界面保存配置）会写**默认**配置路径，不认 --config。
# 为不破坏用户真实配置，测试前备份、结束时恢复。
$realConfig = Join-Path $env:APPDATA 'CampusNet-Keeper\config.json'
$configBackup = Join-Path $tmpDir 'real-config.json.bak'
$hasConfigBackup = $false
if (Test-Path $realConfig) {
    Copy-Item $realConfig $configBackup -Force
    $hasConfigBackup = $true
}

$original = Get-AutostartValue
Info "运行前 Run 键值：" + $(if ($null -eq $original) { '（不存在）' } else { $original })

try {
    # ---------------------------------------------------------------- 1) 开启
    Title '1) 配置 autostart=true → 启动守护应写入 Run 键'
    Set-AutostartValue $null   # 先清空，确保验证的是"守护进程写进去的"
    $base = (Read-LogText).Length
    $d1 = Start-Daemon $cfgOn
    Info "守护 PID=$($d1.Id)"
    Check (Wait-LogMatch '进入托盘守护循环' $base $TimeoutSec) '守护已就绪'
    Start-Sleep -Milliseconds 500   # 注册表同步与托盘就绪同批发生，留一点余量

    $expected = "`"$Exe`" --autostart"
    $value = Get-AutostartValue
    Info "Run 键值：" + $(if ($null -eq $value) { '（不存在）' } else { $value })
    Check ($value -eq $expected) "登录自启项已写入，值为 `"<exe>`" --autostart"
    if ($value -ne $expected) { Info "期望值：$expected" }
    Check (-not $null -eq (Wait-LogMatch '开机自启' $base 5)) '守护日志记录了开机自启同步结果'
    Stop-Daemons

    # ---------------------------------------------------------------- 2) 模拟下次登录
    Title '2) 按 --autostart 启动（模拟登录自启）：守护起来、不弹界面、不留终端'
    $base = (Read-LogText).Length
    # Registry 里的命令行没有 --config；这里额外带临时配置，避免碰用户的真实账号/网络
    $boot = Start-Process -FilePath $Exe -ArgumentList '--autostart','--config',"`"$cfgOn`"" -PassThru
    Info "启动 PID=$($boot.Id)（该进程会分离重启后退出）"
    Check (Wait-LogMatch '已分离到无控制台进程' $base $TimeoutSec) '自启启动时分离到无控制台进程'
    Check (Wait-LogMatch '进入托盘守护循环' $base $TimeoutSec) '守护守护循环已进入'
    $ui = @(Get-Process -Name 'CampusNet-Keeper' -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and $_.MainWindowTitle -match '设置' })
    Start-Sleep -Seconds 2
    $delta = Log-Delta $base
    Check ($ui.Count -eq 0) '没有自动弹出设置界面'
    Check (-not ($delta -match '自动打开设置界面')) '日志里没有「自动打开设置界面」'
    $terms = Get-VisibleTerminalWindows | Where-Object { $_ -match 'CampusNet' }
    Check (-not $terms) '没有留下可见终端窗口'
    Stop-Daemons

    # ---------------------------------------------------------------- 3) 关闭
    Title '3) 配置 autostart=false → 启动守护应删除 Run 键'
    # 先确保注册表里有条目，再让守护进程按配置删掉它
    Set-AutostartValue $expected
    $base = (Read-LogText).Length
    $d3 = Start-Daemon $cfgOff
    Info "守护 PID=$($d3.Id)"
    Check (Wait-LogMatch '进入托盘守护循环' $base $TimeoutSec) '守护已就绪'
    Start-Sleep -Milliseconds 500
    Check ($null -eq (Get-AutostartValue)) 'Run 键已按配置删除'
    Stop-Daemons

    # ---------------------------------------------------------------- 4) --no-autostart
    Title '4) --no-autostart（测试/脚本专用）不得改动注册表'
    Set-AutostartValue $null
    Start-Sleep -Milliseconds 300
    $base = (Read-LogText).Length
    $d4 = Start-Daemon $cfgOn -NoAutostart
    Check (Wait-LogMatch '进入托盘守护循环' $base $TimeoutSec) '守护已就绪'
    Start-Sleep -Milliseconds 800
    $val4 = Get-AutostartValue
    Check ($null -eq $val4) '带 --no-autostart 时没有写入 Run 键'
    if ($null -ne $val4) {
        Info "意外写入的值：$val4"
        Info '本次启动日志（用于定位）：'
        (Log-Delta $base) -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 8 |
            ForEach-Object { Info ("  " + $_.Trim()) }
    }
    $delta4 = Log-Delta $base
    Check (-not ($delta4 -match '开机自启')) '带 --no-autostart 时日志里没有同步记录'
    Stop-Daemons

    # ---------------------------------------------------------------- 5) 设置界面保存
    Title '5) 设置界面保存配置（set_config）应立即同步 Run 键'
    Set-AutostartValue $null
    $base = (Read-LogText).Length
    # 启动阶段用 --no-autostart 隔离，确保 Run 键的变化只可能来自 set_config
    $d5 = Start-Daemon $cfgOn -NoAutostart
    Check (Wait-LogMatch '进入托盘守护循环' $base $TimeoutSec) '守护已就绪'

    $getResp = Send-Ipc '{"id":10,"cmd":"get_config","args":{}}'
    Check ($null -ne $getResp -and $getResp -match '"ok"\s*:\s*true') 'get_config 正常应答'
    $uiCfg = $null
    if ($getResp) {
        $respObj = $getResp | ConvertFrom-Json
        $uiCfg = $respObj.data
        if ($uiCfg -is [string]) { $uiCfg = $uiCfg | ConvertFrom-Json }
    }
    Check ($null -ne $uiCfg) '拿到界面用的配置结构'

    if ($uiCfg) {
        $uiCfg.settings.autostart = $true
        $setOn = @{ id = 11; cmd = 'set_config'; args = @{ config = $uiCfg } } |
            ConvertTo-Json -Depth 16 -Compress
        $respOn = Send-Ipc $setOn
        Check ($null -ne $respOn -and $respOn -match '"ok"\s*:\s*true') 'set_config（开启自启）成功'
        Start-Sleep -Milliseconds 500
        Check ((Get-AutostartValue) -eq $expected) '保存后 Run 键立即写入（无需重启守护）'

        $uiCfg.settings.autostart = $false
        $setOff = @{ id = 12; cmd = 'set_config'; args = @{ config = $uiCfg } } |
            ConvertTo-Json -Depth 16 -Compress
        $respOff = Send-Ipc $setOff
        Check ($null -ne $respOff -and $respOff -match '"ok"\s*:\s*true') 'set_config（关闭自启）成功'
        Start-Sleep -Milliseconds 500
        Check ($null -eq (Get-AutostartValue)) '保存后 Run 键立即删除（无需重启守护）'
    }
    Stop-Daemons
}
finally {
    Stop-Daemons
    # 恢复真实配置（set_config 写的是默认配置路径）与运行前的注册表状态
    if ($hasConfigBackup -and (Test-Path $configBackup)) {
        Copy-Item $configBackup $realConfig -Force
        Info '用户真实配置已恢复'
    }
    # 恢复运行前的注册表状态
    Set-AutostartValue $original
    Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    $final = Get-AutostartValue
    Info ("Run 键已恢复为：" + $(if ($null -eq $final) { '（不存在）' } else { $final }))
}

Write-Host ''
if ($script:failed -eq 0) {
    Write-Host '开机自启验证：全部通过 ✓' -ForegroundColor Green
    exit 0
} else {
    Write-Host "开机自启验证：$($script:failed) 项失败 ✗" -ForegroundColor Red
    exit 1
}
