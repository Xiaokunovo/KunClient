@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "PY="
py -3.10 -c "import sys" >nul 2>nul && set "PY=py -3.10"
if not defined PY py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys" >nul 2>nul && set "PY=python"
if not defined PY (
  echo   没找到 Python，先跑 install.bat
  pause
  exit /b 1
)

echo   正在启动二维码看板…
start "" http://127.0.0.1:7788
%PY% tools\qr-server.py --port 7788
pause
