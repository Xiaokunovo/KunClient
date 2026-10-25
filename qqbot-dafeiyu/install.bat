@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "PY="
py -3.10 -c "import sys" >nul 2>nul && set "PY=py -3.10"
if not defined PY py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys" >nul 2>nul && set "PY=python"

if not defined PY (
  echo   [x] 没找到 Python 3.10+，先装：https://www.python.org/downloads/
  pause
  exit /b 1
)

echo [1/3] 安装 Python 依赖…
%PY% -m pip install -r requirements.txt --disable-pip-version-check
if errorlevel 1 (
  echo   [!] 依赖安装失败，试试换国内源：
  echo       %PY% -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
  pause
  exit /b 1
)

echo.
echo [2/3] 生成配置文件…
if not exist config.json (
  copy /y config.example.json config.json >nul
  echo   已生成 config.json —— 打开它，把 deepseek.api_key 填成你自己的。
) else (
  echo   config.json 已存在，跳过。
)

echo.
echo [3/3] 下载 NapCat 协议端（约 120MB）…
%PY% tools\napcat_setup.py --download

echo.
echo   装完了。下一步：
echo     1) 编辑 config.json，填 deepseek.api_key（https://platform.deepseek.com/api_keys）
echo     2) 双击 start.bat，用手机 QQ 扫码登录
echo.
pause
