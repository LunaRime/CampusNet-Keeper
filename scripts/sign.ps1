<#
.SYNOPSIS
  给 CampusNet-Keeper 的可执行文件打数字签名，并复核签名结果。

.DESCRIPTION
  三种证书来源（按优先级）：
    1. -PfxPath / -PfxPassword   —— 真实 OV/EV 代码签名证书（CI 用 secrets 传进来）；也可用
                                    环境变量 CNK_SIGN_PFX_PATH / CNK_SIGN_PFX_PASSWORD。
    2. -Thumbprint               —— 直接用当前用户证书库里的代码签名证书。
    3. -CreateSelfSignedCert     —— 免费方案：创建/复用一个**自签名**证书（默认行为）。
       或 -Sign 时什么都不给      —— 同 3。

  ⚠ 自签名 ≠ 可信签名：只有"手工把它的公钥证书装进【受信任的发布者/根】"的机器才认它；
    SmartScreen 对自签名依旧会警告。要让陌生用户不看到警告，必须买 OV/EV 代码签名证书。
    本脚本会把自签名证书的**公钥**导出到 -ExportCer（默认 build\certs\），供本机信任用。

  签名（含时间戳）后用 Get-AuthenticodeSignature 复核；自签名证书的"证书链未受信任"是
  预期状态，只要 SignerCertificate 指纹匹配即视为签名成功。

.PARAMETER Files
  要签名的文件（可多个）。

.PARAMETER NoTimestamp
  跳过时间戳（离线环境用；签名仍有效，但证书过期后签名会失效）。

.EXAMPLE
  pwsh -File scripts/sign.ps1 -Files build\Release\CampusNet-Keeper.exe
  pwsh -File scripts/sign.ps1 -Files dist\x.exe -PfxPath codesign.pfx -PfxPassword $env:PFX_PW
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [string]$PfxPath = '',
    [string]$PfxPassword = '',
    [string]$Thumbprint = '',
    [switch]$CreateSelfSignedCert,
    [string]$SelfSignedSubject = 'CN=CampusNet-Keeper (self-signed), O=CampusNet-Keeper, C=CN',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$NoTimestamp,
    [string]$ExportCer = ''
)

$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }
function Ok($m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [注意] $m" -ForegroundColor Yellow }
function Die($m) { Write-Host "  [失败] $m" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 找 signtool
function Find-SignTool {
    $cmd = Get-Command signtool -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (Test-Path $kits) {
        $found = Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

$signtool = Find-SignTool
if (-not $signtool) { Die '找不到 signtool.exe：请安装 Windows SDK（签名工具）' }
Write-Host "签名工具：$signtool"

$targets = @()
foreach ($f in $Files) {
    $p = (Resolve-Path -ErrorAction SilentlyContinue $f)
    if (-not $p) { Die "文件不存在：$f" }
    $targets += $p.Path
}

# ---------------------------------------------------------------- 选证书
if (-not $PfxPath) { $PfxPath = $env:CNK_SIGN_PFX_PATH }
if (-not $PfxPassword) { $PfxPassword = $env:CNK_SIGN_PFX_PASSWORD }

$usePfx = $false
$cert = $null
if ($PfxPath) {
    if (-not (Test-Path $PfxPath)) { Die "PFX 不存在：$PfxPath" }
    if (-not $PfxPassword) { Warn 'PFX 没给密码（-PfxPassword / CNK_SIGN_PFX_PASSWORD）；若 PFX 有密码会失败' }
    $usePfx = $true
    Ok "使用 PFX 证书：$PfxPath"
} else {
    if (-not $Thumbprint) {
        # 没指定就找本机已有的同 subject 自签名证书；没有则创建
        $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -like '*CampusNet-Keeper*' } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
        if (-not $cert) {
            if (-not $CreateSelfSignedCert) {
                Warn '未指定证书来源，按自签名处理（-CreateSelfSignedCert 等价）'
            }
            Step '创建自签名代码签名证书（仅在信任它的机器上有效）'
            $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $SelfSignedSubject `
                -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256 -KeyLength 3072 `
                -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(3) `
                -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
            Ok "已创建：$($cert.Thumbprint)"
        }
    } else {
        $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
        if (-not $cert) { Die "证书库（CurrentUser\My）里找不到指纹为 $Thumbprint 的代码签名证书" }
    }
    $Thumbprint = $cert.Thumbprint
    Ok "使用证书：$($cert.Subject)（指纹 $Thumbprint，有效期至 $($cert.NotAfter.ToString('yyyy-MM-dd'))）"
    if (-not $ExportCer) { $ExportCer = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\certs' }
    if ($cert.Subject -like '*CampusNet-Keeper*') {
        New-Item -ItemType Directory -Force -Path $ExportCer | Out-Null
        $cerPath = Join-Path $ExportCer 'CampusNet-Keeper-codesign.cer'
        Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null
        Ok "公钥证书已导出：$cerPath（本机信任：Import-Certificate 到 CurrentUser\TrustedPublisher）"
    }
}

# ---------------------------------------------------------------- 签名
Step "签名 $($targets.Count) 个文件"
foreach ($file in $targets) {
    # 注意：变量不能叫 $args（PowerShell 自动变量），也别把参数数组塞进另一个数组再遍历
    #（`$list += $array` 在 PowerShell 里会被扁平化，结果是对 signtool 只传了 "sign" 一个词）。
    $signArgs = @('sign', '/fd', 'SHA256')
    if (-not $NoTimestamp) { $signArgs += @('/td', 'SHA256', '/tr', $TimestampUrl) }
    if ($usePfx) {
        $signArgs += @('/f', $PfxPath)
        if ($PfxPassword) { $signArgs += @('/p', $PfxPassword) }
    } else {
        $signArgs += @('/sha1', $Thumbprint)
    }
    $signArgs += $file

    & $signtool @signArgs
    $signed = ($LASTEXITCODE -eq 0)
    if (-not $signed -and -not $NoTimestamp) {
        Warn "带时间戳签名失败（可能是离线/时间戳服务器不可达）→ 重试不带时间戳"
        $noTs = @('sign', '/fd', 'SHA256')
        if ($usePfx) {
            $noTs += @('/f', $PfxPath)
            if ($PfxPassword) { $noTs += @('/p', $PfxPassword) }
        } else {
            $noTs += @('/sha1', $Thumbprint)
        }
        $noTs += $file
        & $signtool @noTs
        $signed = ($LASTEXITCODE -eq 0)
    }
    if (-not $signed) { Die "签名失败：$file（退出码 $LASTEXITCODE）" }
    Ok "已签名：$(Split-Path -Leaf $file)"
}

# ---------------------------------------------------------------- 复核
Step '复核签名'
$failed = 0
foreach ($file in $targets) {
    $sig = Get-AuthenticodeSignature -FilePath $file
    $signer = if ($sig.SignerCertificate) { $sig.SignerCertificate.Thumbprint } else { '' }
    $isOurCert = if ($usePfx) { $sig.Status -eq 'Valid' } else { $signer -eq $Thumbprint }
    $chainNote = if ($sig.Status -eq 'Valid') { '证书链可信' } else { "证书链状态=$($sig.Status)（自签名属预期）" }
    if ($sig.SignerCertificate -and $isOurCert) {
        Ok "$(Split-Path -Leaf $file)：已签名，$chainNote"
    } else {
        Write-Host "  [失败] $(Split-Path -Leaf $file)：$($sig.Status) / $($sig.StatusMessage)" -ForegroundColor Red
        $failed++
    }
}
if ($failed -gt 0) { Die "$failed 个文件签名复核未通过" }

if (-not $usePfx) {
    Warn '当前是**自签名**：只有把导出的 .cer 装进「受信任的发布者」的机器才认；'
    Warn '陌生用户下载仍会看到 SmartScreen 警告——要彻底解决需购买 OV/EV 代码签名证书。'
}
Write-Host "`n签名完成 ✓" -ForegroundColor Green
