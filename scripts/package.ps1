<#
.SYNOPSIS
  产出两个发布包：精简版（单 exe，需目标机装 .NET 10 运行时才有设置界面）与
  完整版（单 exe 双形态，内嵌界面载荷，约 24 MB）——就是 GitHub Release 里上传的那两个。

.DESCRIPTION
  为什么要单独一个脚本，而不是让 CI 直接 cmake + Compress-Archive：
  ① **形态必须由脚本保证**：完整版要经过"发布界面 → 生成存储型载荷 → 编进 exe"，
     漏了任何一步都会得到一个点「设置」只弹"精简版"提示的 exe（2026-10-17 审阅 P2-8）；
  ② **包要自检**：脚本会把 zip 解到临时目录，对**解出来的那个 exe** 跑 `--version` 与
     `--ui-info`，断言形态与名字相符（精简版必须退 3、完整版必须退 0）；
  ③ 版本号取自 `Version.h` 并与 `-Version`（CI 传 tag）比对，防止把 0.1.0 的代码发成 v0.2.0。

.PARAMETER Version
  版本串（可带或不带前缀 v；默认从 Version.h 推导）。CI 里传 `${{ github.ref_name }}`。

.PARAMETER SkipTests
  跳过 C++ 测试（只打包，用于快速重打包）。

.PARAMETER LiteOnly / FullOnly
  只打其中一个包（本地调试用；CI 两个都要）。

.EXAMPLE
  pwsh -File scripts/package.ps1
  pwsh -File scripts/package.ps1 -Version v0.1.0
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [switch]$SkipTests,
    [switch]$LiteOnly,
    [switch]$FullOnly
)

$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$root = Split-Path -Parent $PSScriptRoot          # scripts\ 的上一级 = 仓库根
$dist = Join-Path $root 'dist'
$checkRoot = Join-Path $root 'build\pkg-check'

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }
function Ok($m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Die($m) { Write-Host "  [失败] $m" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 版本口径
$versionHeader = Join-Path $root 'src\native\core\Version.h'
$m = [regex]::Match((Get-Content -Raw -Path $versionHeader), 'VersionString\(\)\s*\{\s*return\s*"([^"]+)"')
if (-not $m.Success) { Die "无法从 $versionHeader 解析版本号（VersionString 的写法变了吗？）" }
$codeVersion = $m.Groups[1].Value
$tag = if ([string]::IsNullOrWhiteSpace($Version)) { "v$codeVersion" }
       elseif ($Version.StartsWith('v')) { $Version }
       else { "v$Version" }
if ($tag -ne "v$codeVersion") {
    Die "版本不一致：-Version 给的是 $tag，而 Version.h 是 v$codeVersion（先改版本号再打包）"
}
Write-Host "打包版本：$tag（Version.h = $codeVersion）" -ForegroundColor White

New-Item -ItemType Directory -Force -Path $dist | Out-Null
Remove-Item $checkRoot -Recurse -Force -ErrorAction SilentlyContinue
# 清掉上一轮的包：CI 用 dist/*.zip 上传，留着旧版本会一起发出去
Get-ChildItem $dist -Filter 'CampusNet-Keeper-*-win-x64.zip' -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- 打包 + 自检
function Build-Package([string]$flavor) {
    $isFull = ($flavor -eq 'full')
    $zipName = "CampusNet-Keeper-$tag-$flavor-win-x64.zip"
    $zip = Join-Path $dist $zipName
    $stage = Join-Path $dist "stage-$flavor"
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    Step "构建$(if ($isFull) { '完整版（单 exe 双形态，内嵌界面载荷）' } else { '精简版（不含界面载荷）' })"
    $buildArgs = @('-NoProfile', '-File', (Join-Path $root 'build.ps1'))
    if ($isFull) { $buildArgs += '-SingleExe' }
    if ($SkipTests) { $buildArgs += '-SkipTests' }
    & pwsh @buildArgs
    if ($LASTEXITCODE -ne 0) { Die "build.ps1$(if ($isFull) { ' -SingleExe' }) 失败（退出码 $LASTEXITCODE）" }

    $exe = Join-Path $root 'build\Release\CampusNet-Keeper.exe'
    if (-not (Test-Path $exe)) { Die "构建产物不存在：$exe" }
    Copy-Item $exe $stage -Force
    foreach ($doc in @('README.md', 'LICENSE')) {
        $p = Join-Path $root $doc
        if (Test-Path $p) { Copy-Item $p $stage -Force }
    }
    $guide = Join-Path $root 'docs\M1-实测指南.md'
    if (Test-Path $guide) { Copy-Item $guide $stage -Force }

    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force

    # ------------------------------------------------------------ 包自检
    # 解出来再跑：证明"用户拿到手的那份"能跑，而不只是构建树里的那份
    $checkDir = Join-Path $checkRoot $flavor
    New-Item -ItemType Directory -Force -Path $checkDir | Out-Null
    Expand-Archive -Path $zip -DestinationPath $checkDir -Force
    $packedExe = Join-Path $checkDir 'CampusNet-Keeper.exe'
    if (-not (Test-Path $packedExe)) { Die "$zipName 里没有 CampusNet-Keeper.exe" }

    $verOut = & $packedExe --version 2>&1
    if ($LASTEXITCODE -ne 0 -or ($verOut -notmatch [regex]::Escape($codeVersion))) {
        Die "解包后的 exe --version 异常（退出码 $LASTEXITCODE，输出：$verOut）"
    }
    $infoOut = & $packedExe --ui-info 2>&1
    $infoRc = $LASTEXITCODE
    $wantRc = if ($isFull) { 0 } else { 3 }
    if ($infoRc -ne $wantRc) {
        Die ("解包后的 exe 形态不符：--ui-info 退出码 $infoRc，期望 $wantRc" +
             "（完整版=0 内嵌载荷就绪；精简版=3 无载荷）。输出：`n" + ($infoOut -join "`n"))
    }
    if ($isFull -and -not ($infoOut -match '全部为存储型')) { Die '完整版载荷里含压缩条目（运行时会被拒绝）' }
    if ($isFull -and -not ($infoOut -match '载荷校验: 通过')) { Die '完整版载荷未通过干跑校验' }
    if (-not $isFull -and ($infoOut -match '载荷: 存在')) { Die '精简版包里居然有内嵌载荷' }

    $info = Get-Item $zip
    Ok ("{0}  {1:N1} MB（解包自检：--version OK，--ui-info 退出 {2}）" -f $zipName, ($info.Length / 1MB), $infoRc)
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    return $zip
}

$made = @()
if (-not $FullOnly) { $made += Build-Package 'lite' }
if (-not $LiteOnly) { $made += Build-Package 'full' }

Remove-Item $checkRoot -Recurse -Force -ErrorAction SilentlyContinue

Step '发布包'
Get-ChildItem $dist -Filter '*.zip' | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } |
    Format-Table -AutoSize | Out-String | Write-Host
Write-Host "发布包已就绪：$dist" -ForegroundColor Green
Write-Host "  精简版：~1 MB，需目标机装 .NET 10 运行时才有设置界面" -ForegroundColor DarkGray
Write-Host "  完整版：单 exe 双形态（内嵌界面载荷）" -ForegroundColor DarkGray
