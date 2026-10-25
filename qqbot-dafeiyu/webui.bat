@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem 本机打开服务器上的 Web 控制台：先建 SSH 隧道，再开浏览器。
rem 端口要和服务器 config.json 里的 webui.port 一致。

set "HOST=root@your-server.example.com"
set "PORT=8899"

echo   正在建立 SSH 隧道 %PORT% -> %HOST% ...
start "" /min ssh -i "%USERPROFILE%\.ssh\kunkun_admin_ed25519" -o StrictHostKeyChecking=no -N -L %PORT%:127.0.0.1:%PORT% %HOST%

echo   等隧道起来（约 3 秒）…
timeout /t 3 /nobreak >nul
start "" http://127.0.0.1:%PORT%

echo.
echo   控制台地址：http://127.0.0.1:%PORT%
echo   访问口令在服务器上：/www/wwwroot/dafeiyu-bot/config.json 的 webui.token
echo.
echo   关掉这个窗口不会断隧道；要断就 taskkill /IM ssh.exe /F
pause
