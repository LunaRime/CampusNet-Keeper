<#
.SYNOPSIS
   把当前会话的代理显式传给 git，并带有限重试；端口每轮变化也无需手工修改。

.DESCRIPTION
   本机 ~/.gitconfig 与本仓库 .git/config 曾写死 http.proxy=127.0.0.1:7897，
   而每轮会话注入的 HTTPS_PROXY 端口都不同。两处写死配置已清除，git 会跟随环境变量；
   注意：代理软件没开启时同样会报 502 / Empty reply / HTTP2 framing / TLS EOF，
   那不是配置问题、必须先把代理开起来。代理刚启动尚未就绪的短暂失败由下面最多 4 次
   重试兜住。git push 幂等，重试安全。

.EXAMPLE
   pwsh -NoProfile -File scripts/git-proxy.ps1 push origin master:main
   pwsh -NoProfile -File scripts/git-proxy.ps1 push origin v0.2.0
   pwsh -NoProfile -File scripts/git-proxy.ps1 ls-remote origin HEAD
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GitArgs
)

$ErrorActionPreference = 'Stop'

$proxy = $env:HTTPS_PROXY
if ([string]::IsNullOrWhiteSpace($proxy)) { $proxy = $env:HTTP_PROXY }
if ([string]::IsNullOrWhiteSpace($proxy)) {
    Write-Error 'git-proxy: 未找到 HTTPS_PROXY / HTTP_PROXY 环境变量；请在代理会话里执行。'
    exit 2
}
if ($GitArgs.Count -eq 0) {
    Write-Error 'git-proxy: 请给出 git 子命令，例如 push origin master:main'
    exit 2
}

$all = @('-c', "http.proxy=$proxy", '-c', "https.proxy=$proxy") + $GitArgs
for ($i = 1; $i -le 4; $i++) {
    & git @all
    if ($LASTEXITCODE -eq 0) { exit 0 }
    if ($i -lt 4) {
        Write-Host "git-proxy: 第 $i 次失败（代理可能在切换/抖动），1 秒后重试" -ForegroundColor Yellow
        Start-Sleep -Milliseconds 1000
    }
}
Write-Error "git-proxy: 连续 4 次失败，请确认代理会话是否正常（当前 $proxy）"
exit 1
