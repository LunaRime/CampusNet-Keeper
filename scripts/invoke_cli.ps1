<#
.SYNOPSIS
  运行 CampusNet-Keeper.exe 并**完整捕获**它的输出与退出码。

.DESCRIPTION
  为什么不能直接 `& $exe --version 2>&1`：
  主程序自 2026-10-27 起是 **GUI 子系统**（CMake 里的 `WIN32_EXECUTABLE TRUE`），
  为的是开机自启时不闪控制台窗口、且启动项里显示的是我们自己的 exe（而不是 wscript
  之类的启动器——这类启动器在火绒的启动项扫描里看不见，用户无法确认/管理）。
  代价是 GUI 子系统进程不继承父进程控制台，PowerShell 直接调用时：
    - 拿不到任何 stdout/stderr（输出落进虚空，`$out` 是空的）；
    - **也不等待进程结束**，`$LASTEXITCODE` 因此是上一个命令的、不可信。
  ⇒ 必须显式把标准输出/错误重定向到文件再读回（Start-Process 的
    -RedirectStandardOutput / -RedirectStandardError + -Wait），退出码从
    PassThru 回来的进程对象上取。

  本函数对**控制台子系统**的 exe（如 cnk_tests.exe）同样适用，所以调用方不需要
  关心被测程序的子系统类型。

  中文：exe 输出的是 UTF-8 字节，读文件时显式按 UTF-8 解码（PowerShell 5.1 的
  Get-Content 默认按 ANSI 解码，会把中文变成乱码 → 后面的中文断言静默失配）。

.OUTPUTS
  PSCustomObject：@{ ExitCode = <int>; Lines = <string[]>; Text = <string> }

.EXAMPLE
  $r = Invoke-CnkCli -Exe 'build\Release\CampusNet-Keeper.exe' -CliArgs @('--version')
  if ($r.ExitCode -ne 0) { throw "退出码 $($r.ExitCode)" }
#>
function Invoke-CnkCli {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$CliArgs = @()
    )

    $tag = 'cnk-cli-' + [guid]::NewGuid().ToString('N')
    $outFile = Join-Path ([System.IO.Path]::GetTempPath()) "$tag.out"
    $errFile = Join-Path ([System.IO.Path]::GetTempPath()) "$tag.err"
    try {
        $spArgs = @{
            FilePath               = $Exe
            NoNewWindow            = $true
            Wait                   = $true
            PassThru               = $true
            RedirectStandardOutput = $outFile
            RedirectStandardError  = $errFile
        }
        if ($CliArgs.Count -gt 0) { $spArgs['ArgumentList'] = $CliArgs }
        $proc = Start-Process @spArgs

        $utf8 = [System.Text.UTF8Encoding]::new($false)
        $lines = @()
        foreach ($f in @($outFile, $errFile)) {
            if (Test-Path -LiteralPath $f) {
                # ReadLines 不会把"最后一行没有换行"的情况丢掉，也不会多出一个空元素
                $lines += [System.IO.File]::ReadAllLines($f, $utf8)
            }
        }
        return [pscustomobject]@{
            ExitCode = [int]$proc.ExitCode
            Lines    = @($lines)
            Text     = (@($lines) -join "`n")
        }
    } finally {
        Remove-Item -LiteralPath $outFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue
    }
}
