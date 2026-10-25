# ═════════════════════════════════════════════════════════════════════════════
#  build-exe.ps1 — KunClient 发布构建入口
#
#  2026-10-11：KunClient 已从 .NET/WPF 迁移到 C++/Qt6（见 src/cpp/BUILD.md）。
#  旧 .NET 版脚本（dotnet publish tests\src\KunClient.App）已备份到
#  .workbuddy/tmp/build-exe.ps1.dotnet-backup，tests\src 下的 C# 工程进入退役状态。
#
#  新流程：调 MSYS2 bash 执行 src/cpp/build-release.sh
#  （CMake + Ninja 构建 → dist/ DLL 闭包 → strip → md5 → 独立运行冒烟）。
# ═════════════════════════════════════════════════════════════════════════════
$ErrorActionPreference = 'Stop'

$cppDir = Join-Path $PSScriptRoot 'src\cpp'
$script = Join-Path $cppDir 'build-release.sh'
if (-not (Test-Path $script)) { throw "找不到 $script —— C++ 主工程不在预期位置" }

# 终止运行中的 KunClient（dist/ 要覆盖写）
$process = Get-Process -Name "KunClient" -ErrorAction SilentlyContinue
if ($process) {
    Write-Host "终止运行中的 KunClient..."
    Stop-Process -Name "KunClient" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

# 走 MSYS2 bash（Git Bash 的 bash 也能跑脚本，但 PATH 定位用绝对 /e/msys64，无歧义）
$bash = 'C:/Program Files\Git\bin\bash.exe'
if (-not (Test-Path $bash)) { $bash = 'E:/msys64/usr/bin/bash.exe' }

Write-Host 'Building KunClient (C++/Qt6, Release)...'
& $bash -lc "cd /e/flashtool/src/cpp && ./build-release.sh"
if ($LASTEXITCODE -ne 0) { throw "build-release.sh failed with exit code $LASTEXITCODE" }

$exe = Join-Path $cppDir 'dist\KunClient.exe'
if (-not (Test-Path $exe)) { throw "构建完成但未找到 $exe" }

$sizeMb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
$md5 = (Get-FileHash $exe -Algorithm MD5).Hash.ToLower()
Write-Host ""
Write-Host "========================================"
Write-Host "  Build Complete!"
Write-Host "========================================"
Write-Host "EXE: $exe ($sizeMb MB)"
Write-Host "MD5: $md5"
Write-Host ""
Write-Host "产物目录 dist\ 自带全部 Qt DLL，可独立运行（无运行时依赖）。"
Write-Host "发版 = 打 dist\ 整目录；版本号统一改用 src\cpp\tools\set-version.js。"
