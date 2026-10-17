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
# signtool 并不"装在 VS 的 IDE 目录里"：它随 **Windows SDK** 分发，而 VS 2022 的
# 「使用 C++ 的桌面开发」工作负载默认就会装 Windows SDK →
#   C:\Program Files (x86)\Windows Kits\10\bin\<版本>\x64\signtool.exe
# 另外 VS/.NET SDK 还会带一个 **ClickOnce SignTool**（老版本 32 位，同样支持 /fd /tr /td）：
#   C:\Program Files (x86)\Microsoft SDKs\ClickOnce\SignTool\signtool.exe
# 查找顺序：PATH → Windows SDK（取最高版本，x64）→ ClickOnce SignTool；
# 实在都没有就退回 PowerShell 自带的 Set-AuthenticodeSignature（零依赖，但不支持时间戳）。
function Find-SignTool {
    $cmd = Get-Command signtool -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @()
    # 1) Windows SDK（新版，x64）
    $sdkRoots = @()
    if ($env:WindowsSdkDir) { $sdkRoots += $env:WindowsSdkDir.TrimEnd('\') }
    if (${env:ProgramFiles(x86)}) { $sdkRoots += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10') }
    if ($env:ProgramFiles) { $sdkRoots += (Join-Path $env:ProgramFiles 'Windows Kits\10') }
    foreach ($root in $sdkRoots) {
        if (-not (Test-Path $root)) { continue }
        # 根可能是 ...\Windows Kits\10，也可能已经是 ...\bin
        $binRoot = Join-Path $root 'bin'
        if (-not (Test-Path $binRoot)) { $binRoot = $root }
        $candidates += Get-ChildItem $binRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path (Join-Path $_.FullName 'x64') 'signtool.exe' } |
            Where-Object { Test-Path $_ }
    }
    # 2) VS / .NET SDK 附带的 ClickOnce SignTool（老版本但够用）
    foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($pf) { $candidates += (Join-Path $pf 'Microsoft SDKs\ClickOnce\SignTool\signtool.exe') }
    }
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

$signtool = Find-SignTool
if ($signtool) {
    Write-Host "签名工具：$signtool"
} else {
    Warn '找不到 signtool.exe（VS 2022 的「C++ 桌面开发」工作负载会随 Windows SDK 一起装它；'
    Warn '也可能是 Microsoft SDKs\ClickOnce\SignTool 下的那份）'
    Warn '→ 退回 PowerShell 自带的 Set-AuthenticodeSignature：可签名，但不支持时间戳'
}

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
$isSelfSigned = $false
if ($PfxPath) {
    if (-not (Test-Path $PfxPath)) { Die "PFX 不存在：$PfxPath" }
    # 导入到当前用户证书库（CI runner 是临时环境），之后统一**按指纹**签名：
    # 既避免在命令行/进程参数里暴露密码，也让复核能按指纹比对——自签名证书链在 CI 上
    # 不会被信任（Status≠Valid），不能拿"链是否受信"当成功判据。
    $secure = $null
    if ($PfxPassword) { $secure = ConvertTo-SecureString $PfxPassword -AsPlainText -Force }
    try {
        $cert = Import-PfxCertificate -FilePath $PfxPath -CertStoreLocation Cert:\CurrentUser\My -Password $secure
    } catch {
        Die "导入 PFX 失败：$($_.Exception.Message)"
    }
    if (-not $cert) { Die '导入 PFX 后没拿到证书（PFX 里是不是没有私钥？）' }
    $Thumbprint = $cert.Thumbprint
    $isSelfSigned = ($cert.Issuer -eq $cert.Subject)
    Ok "已导入 PFX 证书：$($cert.Subject)（指纹 $Thumbprint）"
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
    $isSelfSigned = ($cert.Issuer -eq $cert.Subject)
    Ok "使用证书：$($cert.Subject)（指纹 $Thumbprint，有效期至 $($cert.NotAfter.ToString('yyyy-MM-dd'))）"
    if (-not $ExportCer) { $ExportCer = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\certs' }
    if ($isSelfSigned) {
        New-Item -ItemType Directory -Force -Path $ExportCer | Out-Null
        $cerPath = Join-Path $ExportCer 'CampusNet-Keeper-codesign.cer'
        Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null
        Ok "公钥证书已导出：$cerPath（本机信任：Import-Certificate 到 CurrentUser\TrustedPublisher）"
    }
}

# ---------------------------------------------------------------- 签名
Step "签名 $($targets.Count) 个文件"
foreach ($file in $targets) {
    if (-not $signtool) {
        # 兜底：PowerShell 自带签名（无时间戳；证书必须已在当前用户证书库里）
        try {
            Set-AuthenticodeSignature -FilePath $file -Certificate $cert -HashAlgorithm SHA256 `
                -ErrorAction Stop | Out-Null
        } catch {
            Die "签名失败（Set-AuthenticodeSignature）：$($_.Exception.Message)"
        }
        Ok "已签名（Set-AuthenticodeSignature，无时间戳）：$(Split-Path -Leaf $file)"
        continue
    }
    # 注意：变量不能叫 $args（PowerShell 自动变量），也别把参数数组塞进另一个数组再遍历
    #（`$list += $array` 在 PowerShell 里会被扁平化，结果是对 signtool 只传了 "sign" 一个词）。
    # 无论证书来自 PFX 还是本机证书库，统一用指纹（/sha1）— 上一节已把 PFX 导入证书库。
    $signArgs = @('sign', '/fd', 'SHA256')
    if (-not $NoTimestamp) { $signArgs += @('/td', 'SHA256', '/tr', $TimestampUrl) }
    $signArgs += @('/sha1', $Thumbprint, $file)

    & $signtool @signArgs
    $signed = ($LASTEXITCODE -eq 0)
    if (-not $signed -and -not $NoTimestamp) {
        Warn "带时间戳签名失败（可能是离线/时间戳服务器不可达）→ 重试不带时间戳"
        $noTs = @('sign', '/fd', 'SHA256', '/sha1', $Thumbprint, $file)
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
    # 成功判据 = 签名者是我们这张证书。**不能**用"证书链是否受信"当判据：
    # 自签名证书在没导入受信任根的机器（例如 CI runner）上永远是 UnknownError。
    $isOurCert = ($signer -eq $Thumbprint)
    $chainNote = if ($sig.Status -eq 'Valid') { '证书链可信' } else { "证书链状态=$($sig.Status)（未导入受信任根时属预期）" }
    if ($sig.SignerCertificate -and $isOurCert) {
        Ok "$(Split-Path -Leaf $file)：已签名，$chainNote"
    } else {
        Write-Host "  [失败] $(Split-Path -Leaf $file)：$($sig.Status) / $($sig.StatusMessage)" -ForegroundColor Red
        $failed++
    }
}
if ($failed -gt 0) { Die "$failed 个文件签名复核未通过" }

if ($isSelfSigned) {
    Warn '当前是**自签名**：只有把导出的 .cer 装进「受信任的根证书」的机器才认它；'
    Warn '陌生用户下载仍会看到 SmartScreen 警告——要彻底解决需购买 OV/EV 代码签名证书。'
}
Write-Host "`n签名完成 ✓" -ForegroundColor Green
