@echo off
REM ============================================================
REM  WinUSB Driver Installer for Qualcomm HS-USB QDLoader 9008
REM  Automatically replaces qcusbser driver with WinUSB
REM ============================================================

echo ============================================================
echo  KunFlashTool - WinUSB Driver Installer
echo  For Qualcomm HS-USB QDLoader 9008 (VID_05C6 PID_9008)
echo ============================================================
echo.

REM Check for administrator privileges
net session >nul 2>&1
if %errorLevel% == 0 (
    echo [OK] Running with administrator privileges.
) else (
    echo [ERROR] This script must be run as Administrator!
    echo.
    echo Please right-click on this file and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

echo.
echo [1/5] Creating INF file for WinUSB driver...

set INF_PATH=%TEMP%\WinUSB_Qualcomm_9008.INF

(
echo [Version]
echo Signature="%%WINDOWS NT$"
echo Class=USB
echo ClassGuid={36FC9E60-C465-11CF-8056-444553540000}
echo Provider="KunFlashTool"
echo DriverVer=01/01/2024,1.0.0.0
echo.
echo [Manufacturer]
echo "Qualcomm"=DeviceList
echo.
echo [DeviceList]
echo "Qualcomm HS-USB QDLoader 9008 (WinUSB)"=WinUSB_Inst,USB\VID_05C6^&PID_9008
echo.
echo [WinUSB_Inst.NT]
echo Include=winusb.inf
echo Needs=WinUSB.Device
echo.
echo [Strings]
echo ProviderName="KunFlashTool"
echo MFGNAME="Qualcomm"
echo DeviceDesc="Qualcomm HS-USB QDLoader 9008 (WinUSB)"
) > %INF_PATH%

if not exist %INF_PATH% (
    echo [ERROR] Failed to create INF file.
    pause
    exit /b 1
)
echo [OK] INF file created.

echo.
echo [2/5] Finding Qualcomm 9008 device...

REM Find all device instances matching VID_05C6&PID_9008
setlocal enabledelayedexpansion
set DEVICE_INSTANCES=
set COUNT=0

for /f "tokens=3 delims= " %%a in ('pnputil /enum-devices /class USB 2^>nul ^| findstr /i "VID_05C6 PID_9008"') do (
    set /a COUNT+=1
    set "DEVICE_!COUNT!=%%a"
    set "DEVICE_INSTANCES=!DEVICE_INSTANCES! %%a"
)

if "!COUNT!"=="0" (
    REM Try broader search
    for /f "tokens=3 delims= " %%a in ('pnputil /enum-devices 2^>nul ^| findstr /i "VID_05C6 PID_9008"') do (
        set /a COUNT+=1
        set "DEVICE_!COUNT!=%%a"
        set "DEVICE_INSTANCES=!DEVICE_INSTANCES! %%a"
    )
)

if "!COUNT!"=="0" (
    echo [ERROR] Could not find Qualcomm 9008 device.
    echo.
    echo Please make sure:
    echo   1. The device is connected via USB
    echo   2. The device is in EDL mode (9008)
    echo   3. Device Manager shows "Qualcomm HS-USB QDLoader 9008"
    echo.
    pause
    exit /b 1
)

echo [OK] Found !COUNT! device(s).
for /l %%i in (1,1,!COUNT!) do (
    echo      !DEVICE_%%i!
)

echo.
echo [3/5] Uninstalling current qcusbser driver...

for /l %%i in (1,1,!COUNT!) do (
    set "INST=!DEVICE_%%i!"
    echo      Removing: !INST!
    pnputil /remove-device "!INST!" 2>nul
)

echo      Waiting for device to disconnect...
timeout /t 3 /nobreak >nul

echo.
echo [4/5] Installing WinUSB driver...

REM First, add the driver to the driver store
pnputil /add-driver %INF_PATH% 2>nul

REM Then install for each device
for /l %%i in (1,1,!COUNT!) do (
    set "INST=!DEVICE_%%i!"
    echo      Installing for: !INST!
    
    REM Try install
    pnputil /install-driver %INF_PATH% 2>nul
    
    REM Or update existing
    pnputil /update-driver "!INST!" 2>nul
)

echo      Waiting for device to reappear...
timeout /t 3 /nobreak >nul

echo.
echo [5/5] Verifying installation...

set VERIFY_COUNT=0
for /f "tokens=3 delims= " %%a in ('pnputil /enum-devices 2^>nul ^| findstr /i "VID_05C6 PID_9008"') do (
    set /a VERIFY_COUNT+=1
)

if "!VERIFY_COUNT!" GTR "0" (
    echo [SUCCESS] WinUSB driver installed successfully!
    echo           Device re-enumerated. You can now use KunFlashTool.
) else (
    echo [WARNING] Device may not have re-enumerated.
    echo           Try unplugging and replugging the device.
    echo           The WinUSB driver INF is at: %INF_PATH%
)

echo.
echo ============================================================
echo Installation complete!
echo ============================================================
echo.
echo If the device still doesn't work, try manual method:
echo   1. Open Device Manager (devmgmt.msc)
echo   2. Find "Qualcomm HS-USB QDLoader 9008"
echo   3. Right-click ^> Update driver ^> Browse
echo   4. Select "Let me pick from list"
echo   5. Choose "Universal Serial Bus devices" ^> "WinUSB Device"
echo   6. Have Disk ^> Browse to: %INF_PATH%
echo.

pause
endlocal
