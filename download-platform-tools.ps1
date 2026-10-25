# Download Android Platform Tools for KunClient
# 下载 Android Platform Tools 以支持 ADB 和 Fastboot 功能

$ErrorActionPreference = 'Stop'

$platformToolsDir = Join-Path $PSScriptRoot 'src\KunClient.Core\PlatformTools'
$downloadUrl = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip"
$zipFile = Join-Path $PSScriptRoot 'platform-tools.zip'

Write-Host "开始下载 Android Platform Tools..."
Write-Host "下载地址: $downloadUrl"

# Create directory if it doesn't exist
if (-not (Test-Path $platformToolsDir)) {
    New-Item -ItemType Directory -Path $platformToolsDir -Force | Out-Null
    Write-Host "创建工具目录: $platformToolsDir"
}

# Download the zip file
try {
    Write-Host "正在下载..."
    Invoke-WebRequest -Uri $downloadUrl -OutFile $zipFile -UseBasicParsing
    Write-Host "下载完成"
}
catch {
    Write-Host "下载失败: $_"
    Write-Host "请手动从以下地址下载:"
    Write-Host "https://developer.android.com/studio/releases/platform-tools"
    exit 1
}

# Extract the files
try {
    Write-Host "正在解压..."
    Expand-Archive -Path $zipFile -DestinationPath $PSScriptRoot -Force
    
    # Copy the required files
    $extractedDir = Join-Path $PSScriptRoot 'platform-tools'
    if (Test-Path $extractedDir) {
        $filesToCopy = @('adb.exe', 'fastboot.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll')
        
        foreach ($file in $filesToCopy) {
            $sourceFile = Join-Path $extractedDir $file
            $destFile = Join-Path $platformToolsDir $file
            
            if (Test-Path $sourceFile) {
                Copy-Item -Path $sourceFile -Destination $destFile -Force
                Write-Host "复制文件: $file"
            } else {
                Write-Host "警告: 文件不存在 - $file"
            }
        }
        
        # Clean up
        Remove-Item -Path $extractedDir -Recurse -Force
        Write-Host "清理临时文件"
    }
}
catch {
    Write-Host "解压失败: $_"
    exit 1
}
finally {
    # Remove the zip file
    if (Test-Path $zipFile) {
        Remove-Item -Path $zipFile -Force
        Write-Host "删除下载的ZIP文件"
    }
}

Write-Host "Android Platform Tools 安装完成!"
Write-Host "工具目录: $platformToolsDir"
Write-Host ""
Write-Host "已安装的文件:"
Get-ChildItem $platformToolsDir -File | ForEach-Object {
    $size = [math]::Round($_.Length / 1KB, 1)
    Write-Host "  $($_.Name) - $size KB"
}