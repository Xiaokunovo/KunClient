@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "PY="
py -3.10 -c "import sys" >nul 2>nul && set "PY=py -3.10"
if not defined PY py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys" >nul 2>nul && set "PY=python"

if not defined PY (
  echo.
  echo   [x] 没找到 Python。装一个 3.10 或更新的版本：https://www.python.org/downloads/
  echo       安装时记得勾选 "Add Python to PATH"。
  echo.
  pause
  exit /b 1
)

echo   使用解释器：%PY%
%PY% run.py %*

echo.
echo   机器人已退出。
pause
