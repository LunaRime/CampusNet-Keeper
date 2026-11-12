<#
.SYNOPSIS
   游戏/直播卡顿现场取证：每秒记录到固定公网 IP 的 ICMP 往返，用来区分两种情况。

   背景：2026-11-11 真机日志显示，AC 复核超时集中在 17-23 点（游戏/直播时段），
   而双设备（游戏 PC + 手机直播）同时卡 2~4 秒。直播是已建长连接、不依赖新 DNS 查询，
   两台设备一起卡更指向 WAN 侧转发面瞬时黑洞，而非单设备 DNS 抖动。
   纯 IP 的 ping 不经过 DNS：卡顿时刻它也超时 → 转发面黑洞；它正常 → 问题在 DNS/应用层。

.EXAMPLE
   pwsh -NoProfile -File tools/capture-blip.ps1 -Minutes 30
   pwsh -NoProfile -File tools/capture-blip.ps1 -Minutes 60 -Target 119.29.29.29 -Out D:\blip.csv
#>
[CmdletBinding()]
param(
    [int]$Minutes = 30,
    [string]$Target = '223.5.5.5',
    [string]$Out = ''
)

if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $env:TEMP ("cnk-blip-{0}.csv" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

"timestamp,ping_ms,ping_ok" | Set-Content -Path $Out -Encoding utf8
$pinger = New-Object System.Net.NetworkInformation.Ping
$end = (Get-Date).AddMinutes($Minutes)

Write-Host "采样 $Minutes 分钟，目标 $Target → $Out" -ForegroundColor Cyan
Write-Host "打游戏/看直播时保持本窗口运行；卡顿时记下时间，事后对照 CSV。Ctrl+C 可提前结束。" -ForegroundColor Cyan

while ((Get-Date) -lt $end) {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    try {
        $r = $pinger.Send($Target, 1000)
        $ok = if ($r.Status -eq [System.Net.NetworkInformation.IPStatus]::Success) { 1 } else { 0 }
        $ms = if ($ok -eq 1) { $r.RoundtripTime } else { -1 }
    }
    catch {
        $ok = 0
        $ms = -1
    }
    "$ts,$ms,$ok" | Add-Content -Path $Out -Encoding utf8
    if ($ok -eq 0) { Write-Host "$ts  ICMP 超时" -ForegroundColor Red }
    Start-Sleep -Milliseconds 1000
}

Write-Host "完成：$Out" -ForegroundColor Green
