# Platform Tools 目录说明

此目录用于存放 Android Platform Tools (ADB 和 Fastboot)。

## 如何获取工具

### 方法1：从 Android SDK Platform Tools 下载

1. 访问官方下载页面：https://developer.android.com/studio/releases/platform-tools
2. 下载 Windows 版本的 platform-tools
3. 解压下载的 ZIP 文件
4. 将以下文件复制到此目录：
   - `adb.exe`
   - `fastboot.exe`
   - `AdbWinApi.dll`
   - `AdbWinUsbApi.dll`

### 方法2：从 ADB 工具镜像下载

可以从以下镜像源下载：
- https://dl.google.com/android/repository/platform-tools-latest-windows.zip

## 文件列表

需要的文件：
- `adb.exe` - Android Debug Bridge 可执行文件
- `fastboot.exe` - Fastboot 协议工具
- `AdbWinApi.dll` - ADB Windows API 库
- `AdbWinUsbApi.dll` - ADB USB 驱动接口库

## 目录结构

```
KunClient.Core/
├── PlatformTools/
│   ├── adb.exe           # 需要添加
│   ├── fastboot.exe       # 需要添加
│   ├── AdbWinApi.dll      # 需要添加
│   ├── AdbWinUsbApi.dll   # 需要添加
│   ├── PlatformToolsManager.cs
│   └── README.md
```

## 使用说明

应用程序会自动在此目录中查找工具文件。如果工具不存在，将显示相应的错误消息。

## 版本信息

建议使用最新版本的 Android Platform Tools 以获得最佳兼容性。

当前推荐版本：platform-tools r35.0.0 或更高版本

## 许可证

Android Platform Tools 遵循 Apache License 2.0 许可证。