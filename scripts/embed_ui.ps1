<#
.SYNOPSIS
  把 Avalonia 界面的发布产物打成**存储型（不压缩）** ZIP，并生成引用它的 .rc 资源文件。

.DESCRIPTION
  为什么必须是"存储型"（2026-10-17 的设计结论）：
  C++ 侧不允许引入第三方解压库（Global Constraints），而 Windows 自带的
  compressapi.dll 只有 XPRESS/LZMS/LZ4、**没有 deflate**。所以约定载荷 ZIP 里
  全部条目都用"存储"（method=0），C++ 侧只需解析中央目录 + 按偏移拷数据——
  零依赖、可单测（tests/payload_extract_test.cpp）。
  PowerShell 的 Compress-Archive 会走 deflate，**不能用**；这里用 .NET 的
  ZipFile.CreateFromDirectory(..., CompressionLevel.NoCompression)。

  同时生成版本戳 UI_PAYLOAD_ID = "<版本>+<zip 的 SHA256 前 16 位>"：
  exe 内嵌的载荷一变，戳就变，界面启动时会重新解压（而不是继续用上次那份）。

.PARAMETER UiPublishDir
  界面发布产物目录（dotnet publish 的 -o 目标）。

.PARAMETER OutDir
  生成 ui_payload.zip / ui_payload_id.txt / ui_payload.rc 的目录（默认 build/generated）。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$UiPublishDir,
    [string]$OutDir = 'build/generated'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # scripts\ 的上一级 = 仓库根

function Die($m) { Write-Host "  [失败] $m" -ForegroundColor Red; exit 1 }
function Ok($m) { Write-Host "  [OK] $m" -ForegroundColor Green }

if (-not [System.IO.Path]::IsPathRooted($UiPublishDir)) {
    $UiPublishDir = Join-Path $root $UiPublishDir
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $root $OutDir
}

# ---------------------------------------------------------------- 1) 检查输入
if (-not (Test-Path (Join-Path $UiPublishDir 'CampusNet.UI.dll'))) {
    Die "发布目录里没有 CampusNet.UI.dll：$UiPublishDir（先跑 dotnet publish）"
}
if (-not (Test-Path (Join-Path $UiPublishDir 'CampusNet.UI.runtimeconfig.json'))) {
    Die "发布目录里没有 CampusNet.UI.runtimeconfig.json：$UiPublishDir"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------- 2) 打存储型 ZIP
$zip = Join-Path $OutDir 'ui_payload.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }

Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $UiPublishDir, $zip, [System.IO.Compression.CompressionLevel]::NoCompression, $false)

# ---------------------------------------------------------------- 3) 自检：必须全是存储型
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($archive.Entries)
    $compressed = @($entries | Where-Object { $_.CompressedLength -ne $_.Length })
    $totalBytes = ($entries | Measure-Object -Property Length -Sum).Sum
    if ($entries.Count -lt 2) { Die "载荷条目太少（$($entries.Count) 个），发布目录是不是空的？" }
    if ($compressed.Count -gt 0) {
        $names = ($compressed | Select-Object -First 5 | ForEach-Object { $_.FullName }) -join '、'
        Die "载荷里有 $($compressed.Count) 个压缩条目（C++ 侧会明确拒绝）：$names"
    }
} finally {
    $archive.Dispose()
}
Ok ("载荷已打包：{0:N1} MB，{1} 个条目（全部存储型）" -f ((Get-Item $zip).Length / 1MB), $entries.Count)

# ---------------------------------------------------------------- 4) 版本戳
$versionHeader = Join-Path $root 'src\native\core\Version.h'
$m = [regex]::Match((Get-Content -Raw -Path $versionHeader), 'VersionString\(\)\s*\{\s*return\s*"([^"]+)"')
if (-not $m.Success) { Die "无法从 $versionHeader 解析版本号（VersionString 的写法变了吗？）" }
$version = $m.Groups[1].Value

$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$payloadId = "$version+$($sha.Substring(0, 16))"

$idFile = Join-Path $OutDir 'ui_payload_id.txt'
[System.IO.File]::WriteAllText($idFile, $payloadId, [System.Text.Encoding]::ASCII)
Ok "版本戳：$payloadId"

# ---------------------------------------------------------------- 5) 生成 .rc
# 路径里的反斜杠在 .rc 的引号串里要写成 \\；文件本身用 ASCII，避免 rc.exe 的编码歧义。
$zipAbs = (Resolve-Path $zip).Path.Replace('\', '\\')
$idAbs = (Resolve-Path $idFile).Path.Replace('\', '\\')
$rcPath = Join-Path $OutDir 'ui_payload.rc'
$rc = @"
// 本文件由 scripts/embed_ui.ps1 生成，请勿手工编辑（改动会在下次构建时被覆盖）。
// 刻意不 #include <windows.h>：RCDATA 不需要它，省掉对 rc.exe 包含路径的依赖。
UI_PAYLOAD    RCDATA "$zipAbs"
UI_PAYLOAD_ID RCDATA "$idAbs"
"@
[System.IO.File]::WriteAllText($rcPath, $rc, [System.Text.Encoding]::ASCII)
Ok "已生成 $rcPath"

Write-Host "  载荷资源就绪：CMake 会自动把它编进 exe（见 CMakeLists.txt 的 file(GLOB) 段）" -ForegroundColor DarkGray
