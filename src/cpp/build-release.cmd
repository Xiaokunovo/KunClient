@echo off
REM ─────────────────────────────────────────────────────────────────────────────
REM  KunClient Release 构建（Windows CMD 包装器）
REM  调用 MSYS2 的 bash 执行 build-release.sh。
REM
REM  用法：
REM    build-release.cmd              ^<- Release 构建 + 部署 + strip
REM    build-release.cmd --no-strip   ^<- 保留调试符号
REM    build-release.cmd --clean      ^<- 先清 build/ 再全量构建
REM ─────────────────────────────────────────────────────────────────────────────
setlocal

set "MSYS_ROOT=E:\msys64"
set "BASH=%MSYS_ROOT%\usr\bin\bash.exe"
set "SCRIPT=%~dp0build-release.sh"

if not exist "%BASH%" (
    echo [错误] 找不到 MSYS2 bash: %BASH%
    echo         请确认 MSYS2 安装在 %MSYS_ROOT%
    exit /b 1
)

REM 用 POSIX 路径传给 bash（E:\msys64\... → /e/msys64/...）
set "SCRIPT_POSIX=%SCRIPT:\=/%"
set "SCRIPT_POSIX=/e%SCRIPT_POSIX:~2%"

"%BASH%" -lc "'%SCRIPT_POSIX%' %*"
if errorlevel 1 (
    echo.
    echo [失败] Release 构建未通过。
    exit /b 1
)

echo.
echo [完成] dist\KunClient.exe 已就绪。
endlocal
