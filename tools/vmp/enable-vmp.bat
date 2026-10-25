@echo off
REM ═══════════════════════════════════════════════════════════════════════════
REM  enable-vmp.bat — 一键启用 VMProtect 商业壳（"壳使用 vmp"）
REM
REM  前置（唯一需要你做的）：本机已安装【合法授权】的 VMProtect
REM        （含 VMProtect_Con.exe / VMProtectGUI.exe）。切勿使用破解版。
REM  用法：
REM        enable-vmp.bat               自动在 Program Files 搜索 VMProtect
REM        enable-vmp.bat "D:\VMProtect"  指定安装目录
REM
REM  脚本会：① 复制 VMProtect_Con.exe -> tools\vmp\
REM          ② 缺 KunClient.vmp 时拉起 GUI 让你 Save Project（点一下）
REM          ③ 跑 build-release.sh 自动加壳构建
REM ═══════════════════════════════════════════════════════════════════════════
setlocal
set "VMP_DIR=%~dp0"
set "REPO=%~dp0..\.."
set "DIST=%REPO%\src\cpp\dist"

REM ── [1] 定位 VMProtect 安装目录 ────────────────────────────────────────────
set "VMP_INSTALL="
if not "%~1"=="" (
    set "VMP_INSTALL=%~1"
) else (
    for /d %%D in ("C:\Program Files\VMProtect*" "C:\Program Files (x86)\VMProtect*") do (
        if exist "%%D\VMProtect_Con.exe" set "VMP_INSTALL=%%D"
    )
)
if "%VMP_INSTALL%"=="" (
    echo [错误] 找不到 VMProtect 安装目录。请先安装合法授权的 VMProtect，
    echo        或把安装目录作为第 1 个参数传入：enable-vmp.bat "D:\VMProtect"
    exit /b 1
)
echo [1] VMProtect 安装目录: %VMP_INSTALL%

REM ── [2] 复制控制台到 tools\vmp\ ─────────────────────────────────────────────
if not exist "%VMP_INSTALL%\VMProtect_Con.exe" (
    echo [错误] %VMP_INSTALL%\VMProtect_Con.exe 不存在。请确认安装的是含控制台版的完整授权。
    exit /b 1
)
copy /Y "%VMP_INSTALL%\VMProtect_Con.exe" "%VMP_DIR%VMProtect_Con.exe" >nul
echo [2] 已复制 VMProtect_Con.exe -^> tools\vmp\

REM ── [3] 缺 .vmp 工程则拉起 GUI 让你另存（仅此一步需手动）─────────────────
if not exist "%VMP_DIR%KunClient.vmp" (
    echo [3] 未找到 KunClient.vmp，启动 VMProtect GUI 生成工程……
    echo     - File -^> Open 选：%DIST%\KunClient.exe
    echo     - 右侧 Options：Mode 选 Virtualization（或 Mutation+Virtualization）
    echo     - 勾 Miscellaneous -^> Compress output file（可选填 Watermark）
    echo     - Settings 填入你的合法 License
    echo     - 顶部 Input  = %DIST%\KunClient.exe
    echo     - 顶部 Output = %DIST%\KunClient.vmp.exe
    echo     - File -^> Save Project As 存为：%VMP_DIR%KunClient.vmp
    if exist "%VMP_INSTALL%\VMProtectGUI.exe" (
        start "" "%VMP_INSTALL%\VMProtectGUI.exe" "%DIST%\KunClient.exe"
    ) else (
        start "" "%VMP_INSTALL%\VMProtect.exe"
    )
    echo     保存完成后按任意键继续构建……
    pause >nul
) else (
    echo [3] 已有 KunClient.vmp，跳过 GUI。
)

REM ── [4] 跑构建（build-release.sh 会自动调用 VMProtect 加壳）────────────────
set "BASH=C:\Program Files\Git\bin\bash.exe"
if not exist "%BASH%" set "BASH=E:\msys64\usr\bin\bash.exe"
if not exist "%BASH%" (
    echo [错误] 找不到 bash（Git Bash 或 MSYS2）。请检查路径。
    exit /b 1
)
echo [4] 运行 build-release.sh（自动加壳）……
"%BASH%" -lc "cd /e/flashtool/src/cpp && ./build-release.sh"
echo === enable-vmp 完成 ===
endlocal
