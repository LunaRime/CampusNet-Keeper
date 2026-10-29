@echo off
rem ---------------------------------------------------------------------------
rem Build the router-side client on Windows. This is a DEV convenience only:
rem it lets us run the protocol against the project's own simulator and
rem cross-check AES against the already-verified BCrypt implementation.
rem For deployment, cross-compile with musl (scripts/build_openwrt.sh).
rem
rem Notes learned the hard way:
rem   * vcvars64.bat calls reg.exe, which some sandboxes block -> set INCLUDE /
rem     LIB ourselves instead of calling vcvars.
rem   * vswhere -latest returns VS2026 here, which has no C++ toolset ->
rem     pin the range [17.0,18.0).
rem   * Crypto.cpp (C++) and crypto.c (C) both produce Crypto.obj on a
rem     case-insensitive filesystem -> compile them to distinct names.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "OUT=%ROOT%\build"
if not exist "%OUT%" mkdir "%OUT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
rem 用临时文件取值：for /f 里带引号且含空格的 exe 路径会被 cmd 拆词
"%VSWHERE%" -latest -version "[17.0,18.0)" -products * -property installationPath > "%TEMP%\cnk_vs.txt" 2>nul
set "VSPATH="
set /p VSPATH=<"%TEMP%\cnk_vs.txt"
del "%TEMP%\cnk_vs.txt" >nul 2>nul
if not defined VSPATH goto no_vs

set "MSVCVER="
for /f "delims=" %%d in ('dir /b /ad /o-n "%VSPATH%\VC\Tools\MSVC" 2^>nul') do if not defined MSVCVER set "MSVCVER=%%d"
if not defined MSVCVER goto no_vs

set "SDKVER="
for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\Include" 2^>nul') do if not defined SDKVER set "SDKVER=%%d"
if not defined SDKVER goto no_sdk

set "MSVC=%VSPATH%\VC\Tools\MSVC\%MSVCVER%"
set "SDK=%ProgramFiles(x86)%\Windows Kits\10"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
echo MSVC %MSVCVER% / SDK %SDKVER%

pushd "%OUT%"
cl -nologo -W3 -O2 -utf-8 -D_CRT_SECURE_NO_WARNINGS -Fe:cnkrouter.exe ^
   "%ROOT%\src\crypto.c" "%ROOT%\src\net.c" "%ROOT%\src\portal.c" "%ROOT%\src\main.c" ^
   -link -SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl -nologo -W3 -O2 -utf-8 -c -EHsc -std:c++17 -D_CRT_SECURE_NO_WARNINGS -I"%ROOT%\..\src\native" -Fo:cross_main.obj "%ROOT%\tests\aes_crosscheck.cpp"
if errorlevel 1 goto fail
cl -nologo -W3 -O2 -utf-8 -c -EHsc -std:c++17 -D_CRT_SECURE_NO_WARNINGS -I"%ROOT%\..\src\native" -Fo:cross_cpp.obj "%ROOT%\..\src\native\core\Crypto.cpp"
if errorlevel 1 goto fail
cl -nologo -W3 -O2 -utf-8 -c -D_CRT_SECURE_NO_WARNINGS -Fo:cross_c.obj "%ROOT%\src\crypto.c"
if errorlevel 1 goto fail
cl -nologo -Fe:aes_crosscheck.exe cross_main.obj cross_cpp.obj cross_c.obj bcrypt.lib crypt32.lib
if errorlevel 1 goto fail
popd

echo.
echo OK: %OUT%\cnkrouter.exe  %OUT%\aes_crosscheck.exe
exit /b 0

:fail
popd
echo BUILD FAILED
exit /b 1

:no_vs
echo [ERROR] Visual Studio 2022 (v17) with C++ tools not found
exit /b 1

:no_sdk
echo [ERROR] Windows SDK not found under "%ProgramFiles(x86)%\Windows Kits\10"
exit /b 1
