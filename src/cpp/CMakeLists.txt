cmake_minimum_required(VERSION 3.24)

# ─────────────────────────────────────────────────────────────────────────────
#  KunClient — C++/Qt6 刷机工具
#  以 SWT-EDL 的 EDL 引擎为基底，扩展为完整的 KunClient 客户端。
#  工具链：MSYS2 MinGW64 GCC 16.x + Qt6 (系统级安装于 mingw64/)
# ─────────────────────────────────────────────────────────────────────────────

project(KunClient VERSION 2.0.6 LANGUAGES CXX)

# ── KC_FORCE_STATIC_DEPS：单文件构建开关 ─────────────────────────────────────
#  让 find_library 优先选静态 .a（否则 MinGW 默认先命中 .dll.a 导入库，
#  导致 libpng/freetype/harfbuzz/openssl 等仍走 DLL）。静态 Qt preset 会打开。
option(KC_FORCE_STATIC_DEPS "Link all third-party deps statically" OFF)
# 体积优化：静态单文件产物用。-Os + -ffunction-sections/-fdata-sections + --gc-sections
# 只影响我们自己的目标文件（Qt 静态库是预编译的，不受影响），收益有限但无害。
option(KC_SIZE_OPT "Compile for size and garbage-collect unused sections" OFF)
# 静态构建下丢弃 OpenSSL（走 Schannel）；见下方详细注释。
option(KC_DROP_OPENSSL "Do not link OpenSSL (static build + Schannel TLS backend)" OFF)
if(KC_FORCE_STATIC_DEPS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;.lib")
    # 静态 Qt 的传递依赖组固定在链接行末尾，直接前置依赖库追不上顺序。
    # 把全部归档包进 --start-group/--end-group 让 ld 反复重扫直到解析完。
    # 注意：Ninja 把对象+库合并进 rsp 并展开在 <OBJECTS> 位置，所以 group
    # 必须包住 <OBJECTS>（rsp）而不是 <LINK_LIBRARIES>（此时为空）。
    set(CMAKE_CXX_LINK_EXECUTABLE
        "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> -Wl,--start-group <OBJECTS> -o <TARGET> <LINK_LIBRARIES> -Wl,--end-group")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── Payload 解包用的压缩库 ───────────────────────────────────────────────────
#  REPLACE_BZ / REPLACE_XZ / REPLACE_ZSTD 三种操作分别对应 libbz2 / liblzma / libzstd。
#  静态单文件构建下 KC_FORCE_STATIC_DEPS 会把 find_library 切到 .a，这里只要给名字。
set(KC_COMPRESSION_LIBS bz2 lzma zstd)

# ── Qt6 模块 ─────────────────────────────────────────────────────────────────
#  Core/Widgets/SerialPort/Xml — SWT-EDL 原有
#  Network                     — KunClient HTTP 通信（登录/下载/更新/环境上报）
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Xml Network)

qt_standard_project_setup()

# ── EDL 引擎源文件（从 SWT-EDL 移入，命名空间 swt::edl，不做改动）─────────────
# 拆为两层：
#   EDL_CORE_SOURCES — 纯逻辑层（core/storage/oplus），无 transport/protocol/app 依赖，
#                      供 core-tests 单独编译（只链接 Qt6::Core）。
#   EDL_FULL_SOURCES — 在 EDL_CORE_SOURCES 基础上加 transport/protocol/app，
#                      供主程序和 ui-tests 使用。
set(EDL_CORE_SOURCES
    core/Types.h
    core/DeviceIdentity.h        core/DeviceIdentity.cpp
    core/EdlMaintenance.h        core/EdlMaintenance.cpp
    core/PartitionProtection.h   core/PartitionProtection.cpp
    storage/GptParser.h          storage/GptParser.cpp
    storage/LunCapacity.h        storage/LunCapacity.cpp
    storage/RawProgramParser.h   storage/RawProgramParser.cpp
    storage/RawProgramWriter.h   storage/RawProgramWriter.cpp
    storage/SparseImage.h        storage/SparseImage.cpp
    storage/DynamicSuper.h       storage/DynamicSuper.cpp
    storage/SystemVersionReader.h storage/SystemVersionReader.cpp
    # Android OTA payload.bin（CrAU + protobuf manifest）解析与提取
    storage/PayloadParser.h      storage/PayloadParser.cpp
    # OCDT 生成/分析（欧加 TDCO 容器）：纯字节逻辑，供 Android 工具箱与单测共用
    storage/OcdtGenerator.h      storage/OcdtGenerator.cpp
    oplus/OplusVipSession.h      oplus/OplusVipSession.cpp
    # OFP / OPS 固件解包（AES-128-CFB + 三套 MBox 自定义密钥流），纯逻辑、可单测
    oplus/AesCfb.h               oplus/AesCfb.cpp
    oplus/OfpExtractor.h         oplus/OfpExtractor.cpp
    oplus/OpsExtractor.h         oplus/OpsExtractor.cpp
)

set(EDL_FULL_SOURCES
    ${EDL_CORE_SOURCES}
    transport/SerialTransport.h  transport/SerialTransport.cpp
    protocol/Sahara.h            protocol/Sahara.cpp
    protocol/Firehose.h          protocol/Firehose.cpp
    app/EdlWorker.h              app/EdlWorker.cpp
)

# ── KunClient 核心源文件（新增，命名空间 kc::）─────────────────────────────────
# ── MTK 协议层 ───────────────────────────────────────────────────────────────
#  三组：
#   MTK_PARSER_SOURCES — 纯文本解析（scatter），零 IO 依赖；
#   MTK_PROTO_SOURCES  — 协议层（BROM 回显 / DA 文件 / XFlash 包格式），只依赖
#                        ITransport 抽象 + QSerialPort 实现 → 可接假设备单测；
#   MTK_DEVICE_SOURCES — 走 SetupAPI/cfgmgr32 的设备发现、整机编排、专线程 worker。
set(MTK_PARSER_SOURCES
    kc/mtk/MtkTypes.h
    kc/mtk/ScatterParser.h          kc/mtk/ScatterParser.cpp
)
set(MTK_PROTO_SOURCES
    kc/mtk/MtkTypes.cpp
    kc/mtk/MtkTransport.h           kc/mtk/MtkTransport.cpp
    kc/mtk/MtkDaFile.h              kc/mtk/MtkDaFile.cpp
    kc/mtk/MtkBrom.h                kc/mtk/MtkBrom.cpp
    kc/mtk/MtkXflash.h              kc/mtk/MtkXflash.cpp
)
set(MTK_DEVICE_SOURCES
    kc/mtk/MtkDevice.h              kc/mtk/MtkDevice.cpp
    kc/mtk/MtkDeviceSession.h       kc/mtk/MtkDeviceSession.cpp
    kc/mtk/MtkWorker.h              kc/mtk/MtkWorker.cpp
)

set(KC_CORE_SOURCES
    kc/config/AppStatePaths.h       kc/config/AppStatePaths.cpp
    kc/config/ClientConfig.h        kc/config/ClientConfig.cpp
    kc/config/AppSettings.h         kc/config/AppSettings.cpp
    # ── 停更离线版（2.0.6）：网络层 / 账号层 / 环境上报 / 自动更新 / 云端引导
    #    全部移除（作者退圈、服务器关停）。以下保留的是纯本地能力：
    #    Sha256（CNG 摘要）/ Digest（固件校验）/ FirmwareChecksum（校验文件口径）/
    #    SessionLog + LogCollector（本地日志）/ PlatformToolsManager（本地 adb 调用）。
    kc/security/StrObf.h
    # SHA-256/HMAC：走 Windows CNG，避免 QCryptographicHash 把整个 OpenSSL（4.4MB）拉进来
    kc/security/Sha256.h            kc/security/Sha256.cpp
    # 流式摘要（MD5 / SHA-256）：固件校验用，同样走 CNG
    kc/security/Digest.h            kc/security/Digest.cpp
    # 校验文件（.md5 / .sha256）的格式读写口径
    kc/tools/FirmwareChecksum.h     kc/tools/FirmwareChecksum.cpp
    # FDL 签名数据的解析口径（十六进制 / Base64）：纯本地，FdlAuthDialog 与
    # XiaomiEdlPage 共用。服务器关停不影响它 —— 签名是用户手工粘贴的。
    kc/auth/SignCodec.h             kc/auth/SignCodec.cpp
    kc/diag/SessionLog.h            kc/diag/SessionLog.cpp
    kc/diag/LogCollector.h          kc/diag/LogCollector.cpp
    kc/tools/PlatformToolsManager.h kc/tools/PlatformToolsManager.cpp
    # 「引导未上报分区大小」补偿链：换二进制 / 加 --force 逐个重试
    kc/tools/FastbootSizeFallback.h kc/tools/FastbootSizeFallback.cpp
    # adb 侧读真实 GPT：dd 磁盘头 → GptParser（fastboot 协议没有读分区命令）
    kc/tools/AdbGptReader.h         kc/tools/AdbGptReader.cpp
    kc/devices/DeviceWatcher.h      kc/devices/DeviceWatcher.cpp
    # 「小米字库牌子」的离线逻辑（解析 INFO: ufs/emmc 行、厂牌表、引导文件发现、9008 端口指纹）
    kc/devices/StorageBrandProbe.h  kc/devices/StorageBrandProbe.cpp
    kc/devices/StorageBrandRunner.h kc/devices/StorageBrandRunner.cpp
    kc/devices/UsbDeviceNotifier.h  kc/devices/UsbDeviceNotifier.cpp
    # 连接状态粘滞（纯逻辑，头文件实现）：设备主页防「一会儿连一会儿断」
    kc/devices/UsbStateStickiness.h
    ${MTK_PARSER_SOURCES}
    ${MTK_PROTO_SOURCES}
    ${MTK_DEVICE_SOURCES}
    kc/version.h
)

# ── UI 源文件 ─────────────────────────────────────────────────────────────────
set(UI_SOURCES
    ui/MainWindow.h               ui/MainWindow.cpp
    ui/MainShell.h                ui/MainShell.cpp
    ui/ShellStatus.h              ui/ShellStatus.cpp
    ui/theme/ThemeManager.h       ui/theme/ThemeManager.cpp
    ui/theme/Motion.h             ui/theme/Motion.cpp
    ui/theme/PageTransition.h     ui/theme/PageTransition.cpp
    ui/theme/GlassSettings.h      ui/theme/GlassSettings.cpp
    ui/widgets/NotificationBar.h  ui/widgets/NotificationBar.cpp
    ui/widgets/PageHeaderBar.h    ui/widgets/PageHeaderBar.cpp
    ui/widgets/ToolProgressBar.h  ui/widgets/ToolProgressBar.cpp
    ui/widgets/LogView.h          ui/widgets/LogView.cpp
    ui/widgets/MarqueeLabel.h     ui/widgets/MarqueeLabel.cpp
    ui/widgets/AppIcons.h         ui/widgets/AppIcons.cpp
    ui/widgets/SvgPath.h          ui/widgets/SvgPath.cpp
    ui/widgets/DynamicIsland.h    ui/widgets/DynamicIsland.cpp
    ui/widgets/LiquidGlass.h      ui/widgets/LiquidGlass.cpp
    ui/widgets/GlassSurface.h     ui/widgets/GlassSurface.cpp
    ui/widgets/GlassPanel.h       ui/widgets/GlassPanel.cpp
    ui/widgets/GlassMenu.h        ui/widgets/GlassMenu.cpp
    ui/widgets/Toast.h            ui/widgets/Toast.cpp
    ui/widgets/BottomSheet.h      ui/widgets/BottomSheet.cpp
    ui/widgets/IosSwitch.h        ui/widgets/IosSwitch.cpp
    ui/widgets/SegmentedControl.h ui/widgets/SegmentedControl.cpp
    ui/widgets/IosSpinner.h       ui/widgets/IosSpinner.cpp
    ui/widgets/IosProgressRing.h  ui/widgets/IosProgressRing.cpp
    ui/widgets/IosSearchField.h   ui/widgets/IosSearchField.cpp
    ui/widgets/IosGroupedList.h   ui/widgets/IosGroupedList.cpp
    ui/widgets/IosPageControl.h   ui/widgets/IosPageControl.cpp
    ui/widgets/IosStepper.h       ui/widgets/IosStepper.cpp
    ui/widgets/IosBadge.h         ui/widgets/IosBadge.cpp
    ui/widgets/IosSwipeRow.h      ui/widgets/IosSwipeRow.cpp
    ui/pages/BasePage.h             ui/pages/BasePage.cpp
    ui/pages/ConsolePage.h          ui/pages/ConsolePage.cpp
    ui/pages/ControlCenterPage.h    ui/pages/ControlCenterPage.cpp
    ui/pages/EdlFlashPage.h         ui/pages/EdlFlashPage.cpp
    ui/pages/DeviceHomePage.h       ui/pages/DeviceHomePage.cpp
    ui/pages/AppManagerPage.h       ui/pages/AppManagerPage.cpp
    ui/pages/FileTransferPage.h     ui/pages/FileTransferPage.cpp
    ui/pages/ScreenMirrorPage.h     ui/pages/ScreenMirrorPage.cpp
    ui/pages/SettingsPage.h         ui/pages/SettingsPage.cpp
    ui/pages/AboutPage.h            ui/pages/AboutPage.cpp
    ui/pages/FastbootFlashPage.h    ui/pages/FastbootFlashPage.cpp
    ui/pages/PayloadPage.h          ui/pages/PayloadPage.cpp
    ui/pages/AndroidToolsPage.h     ui/pages/AndroidToolsPage.cpp
    ui/pages/CommandConsolePage.h   ui/pages/CommandConsolePage.cpp
    ui/pages/OnePlusFlashPage.h     ui/pages/OnePlusFlashPage.cpp
    ui/pages/XiaomiEdlPage.h        ui/pages/XiaomiEdlPage.cpp
    ui/pages/XiaomiFastbootPage.h   ui/pages/XiaomiFastbootPage.cpp
    ui/pages/VisualFlashPage.h      ui/pages/VisualFlashPage.cpp
    ui/pages/MtkFlashPage.h         ui/pages/MtkFlashPage.cpp
    # 视频动态背景解码器：直接走 Windows Media Foundation，不依赖 Qt6::Multimedia
    # （mingw64 没装该模块；ucrt64 静态 Qt 无 FFmpeg 后端，走 Qt 会让单文件包丢掉 mp4）。
    kc/media/VideoBackground.h      kc/media/VideoBackground.cpp
)

# ── 对话框源文件 ─────────────────────────────────────────────────────────────
set(DIALOG_SOURCES
    ui/dialogs/DialogAsync.h
    ui/dialogs/AnimatedDialog.h      ui/dialogs/AnimatedDialog.cpp
    ui/dialogs/GlassDialog.h         ui/dialogs/GlassDialog.cpp
    ui/dialogs/ThemedMessageBox.h    ui/dialogs/ThemedMessageBox.cpp
    ui/dialogs/EraseConfirmDialog.h  ui/dialogs/EraseConfirmDialog.cpp
    # FDL 授权窗（blob 只读 + sig 手工粘贴）：纯本地，小米 FDL 流程必需，保留
    ui/dialogs/FdlAuthDialog.h       ui/dialogs/FdlAuthDialog.cpp
    # ── 停更离线版（2.0.6）：FDL 授权 / 登录 / 更新 / 启动预载 / 云端引导库 /
    #    申诉 / 日志上传 这些联网对话框全部移除。
    ui/dialogs/WelcomeDialog.h     ui/dialogs/WelcomeDialog.cpp
    ui/dialogs/DisclaimerDialog.h  ui/dialogs/DisclaimerDialog.cpp
    ui/dialogs/ComplianceConfirmDialog.h ui/dialogs/ComplianceConfirmDialog.cpp
    # — picker dialogs（本地文件选择，保留）
    ui/dialogs/LoaderPickerDialog.h        ui/dialogs/LoaderPickerDialog.cpp
    ui/dialogs/PackageImageListDialog.h    ui/dialogs/PackageImageListDialog.cpp
    ui/dialogs/WallpaperEnginePickerDialog.h ui/dialogs/WallpaperEnginePickerDialog.cpp
    # WallpaperEngineService：非 UI 的发现服务，随对话框一起编译。
    kc/tools/WallpaperEngineService.h      kc/tools/WallpaperEngineService.cpp
)

# ── CLI 源文件（headless NDJSON 模式）──────────────────────────────────────────
set(CLI_SOURCES
    cli/HeadlessController.h      cli/HeadlessController.cpp
    cli/StdinReader.h             cli/StdinReader.cpp
    cli/HeadlessMain.cpp
)

# ── 资源 ──────────────────────────────────────────────────────────────────────
set(RESOURCE_FILES
    resources/resources.qrc
)

# ── 主可执行文件 ──────────────────────────────────────────────────────────────
qt_add_executable(KunClient WIN32
    main.cpp
    ${EDL_FULL_SOURCES}
    ${KC_CORE_SOURCES}
    ${UI_SOURCES}
    ${DIALOG_SOURCES}
    ${CLI_SOURCES}
    ${RESOURCE_FILES}
    resources/app.rc              # exe 图标 + Windows 版本信息（资源管理器/任务栏）
)

target_include_directories(KunClient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(KunClient PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::SerialPort
    Qt6::Xml
    Qt6::Network
    ${KC_COMPRESSION_LIBS}
)

# ── Win32 系统库（kc:: DPAPI / EnvInspector / WMI 用）──────────────────────────
if(WIN32)
    # bcrypt：kc::crypto::Sha256 用 Windows CNG 做 SHA-256/HMAC（替掉 QCryptographicHash
    # 对 OpenSSL 的强依赖，单文件产物省 4.4MB）。
    # cfgmgr32：MTK 设备枚举用 CM_Get_Parent 读父设备。
    # mfplat/mfreadwrite/mfuuid：视频动态背景走 Windows Media Foundation 解码
    # （MFStartup / IMFSourceReader / RGB32 输出），不需要 Qt6::Multimedia。
    target_link_libraries(KunClient PRIVATE crypt32 bcrypt advapi32 iphlpapi wbemuuid ole32 oleaut32 setupapi cfgmgr32 mfplat mfreadwrite mfuuid)
    if(KC_FORCE_STATIC_DEPS)
        # ── 静态依赖按拓扑序前置（依赖者在前，被依赖者在后）──────────────────
        #  Qt 传递依赖（freetype/openssl/…）会被 CMake 放到链接行末尾，其自身依赖
        #  （bz2/brotli/png/zlib/glib…）必须在它们之后；MinGW 链接器只向右解析，
        #  所以整组静态库在这里显式前置，末尾的重复项无害。
        set(KC_STATIC_DEP_LIBS
            harfbuzz freetype graphite2
            glib-2.0 intl iconv
            pcre2-16 pcre2-8
            b2 bz2 lzma
            png16 z
            brotlicommon brotlidec
            zstd
        )
        foreach(_l ${KC_STATIC_DEP_LIBS})
            find_library(KC_ST_${_l} NAMES ${_l} REQUIRED)
            list(APPEND KC_STATIC_DEP_TARGETS ${KC_ST_${_l}})
        endforeach()
        # ── OpenSSL：默认链接，KC_DROP_OPENSSL=ON 时整组丢掉 ───────────────
        #  🔴 实测（2026-10-11）：收益为 0 —— ld 对静态归档按「被引用才拉取成员」
        #  处理，QtNetwork 走 Schannel 后端、代码里也没有任何 include <openssl/...>，
        #  所以 libcrypto.a/libssl.a 从未真正链进 exe（开/关前后字节数一模一样）。
        #  保留此开关只为防未来误链 OpenSSL 后体积回涨时关掉它。
        if(KC_DROP_OPENSSL)
            message(STATUS "KC_DROP_OPENSSL=ON：不链接 OpenSSL（依赖 Schannel TLS 后端）")
        else()
            find_package(OpenSSL REQUIRED)
            target_link_libraries(KunClient PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        target_link_libraries(KunClient PRIVATE ${KC_STATIC_DEP_TARGETS})
    endif()
    target_link_libraries(KunClient PRIVATE ws2_32 rpcrt4)
endif()

# ── 编译选项 ──────────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(KunClient PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(KunClient PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(NOT QT6_IS_SHARED_LIBS_BUILD)
        set_property(TARGET KunClient PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        target_compile_options(KunClient PRIVATE /O1 /Gy /Gw)
        target_link_options(KunClient PRIVATE /OPT:REF /OPT:ICF)
    endif()
else()
    # MinGW GCC
    target_compile_options(KunClient PRIVATE -Wall -Wextra -Wpedantic)
    # Release：静态链接 GCC 运行时；静态 Qt 构建时整体 -static（真单文件）
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        if(QT6_IS_SHARED_LIBS_BUILD)
            target_link_options(KunClient PRIVATE -static-libgcc -static-libstdc++)
        else()
            target_link_options(KunClient PRIVATE -static)
        endif()
    endif()
    # ── 体积优化（KC_SIZE_OPT）────────────────────────────────────────────────
    #  -Os：按体积优化；-ffunction-sections/-fdata-sections + --gc-sections：
    #  让链接器丢掉未被引用的函数/数据段。注意 Qt/moc 生成的代码也在本目标里，
    #  所以 moc 产物同样受益。判据：编完跑 --netcheck + UI 冒烟。
    if(KC_SIZE_OPT)
        target_compile_options(KunClient PRIVATE -Os -ffunction-sections -fdata-sections)
        target_link_options(KunClient PRIVATE -Wl,--gc-sections)
    endif()
endif()

# ── 静态 Qt 构建时导入：Windows 平台插件 + 原生样式插件 + 🔴 TLS 后端 ──────────
#  TLS 后端是**联网功能的硬依赖**：静态构建下插件靠 qt_import_plugins 编译期导入，
#  漏了它 QSslSocket::supportsSsl() 恒为 false → 登录/所有 HTTPS 请求一律
#  "TLS initialization failed"（界面显示「无法连接服务器」）。
#  优先 Schannel（Windows 原生，不必再链 OpenSSL），其次 OpenSSL 后端。
if(NOT QT6_IS_SHARED_LIBS_BUILD)
    set(KC_STATIC_PLUGINS Qt6::QWindowsIntegrationPlugin)
    if(TARGET Qt6::QModernWindowsStylePlugin)
        list(APPEND KC_STATIC_PLUGINS Qt6::QModernWindowsStylePlugin)
    endif()
    if(TARGET Qt6::QSchannelBackendPlugin)
        list(APPEND KC_STATIC_PLUGINS Qt6::QSchannelBackendPlugin)
    elseif(TARGET Qt6::QTlsBackendOpenSSLPlugin)
        list(APPEND KC_STATIC_PLUGINS Qt6::QTlsBackendOpenSSLPlugin)
    else()
        message(WARNING "静态构建：找不到任何 TLS 后端插件，HTTPS 将不可用（登录必失败）")
    endif()

    # 🔴 图片格式插件（2026-10-12 修「自定义背景失效」）：
    #   QtGui 只内建 png/bmp/pbm/pgm/ppm/xbm/xpm —— **jpg/jpeg/gif/ico 全靠插件**。
    #   上面的 NO_DEFAULT 会连「默认插件集」一起关掉，于是 QImage::load()
    #   对 jpg/gif 一律失败，而背景加载链是静默失败 → 用户选任何 jpg 壁纸都「没反应」
    #   （实测：发布的静态包 2.0.0.3 里 png 壁纸有色像素 4128 个，jpg/gif 均为 0）。
    foreach(_kc_img_plugin Qt6::QJpegPlugin Qt6::QGifPlugin Qt6::QICOPlugin)
        if(TARGET ${_kc_img_plugin})
            list(APPEND KC_STATIC_PLUGINS ${_kc_img_plugin})
        else()
            message(WARNING "静态构建：缺少图片格式插件 ${_kc_img_plugin}（jpg/gif 壁纸将无法显示）")
        endif()
    endforeach()

    qt_import_plugins(KunClient
        NO_DEFAULT
        INCLUDE ${KC_STATIC_PLUGINS}
    )
endif()

# ── 产物属性 ──────────────────────────────────────────────────────────────────
set_target_properties(KunClient PROPERTIES
    OUTPUT_NAME "KunClient"
    WIN32_EXECUTABLE TRUE
)

# ── 版本信息（Windows exe 属性）──────────────────────────────────────────────
if(WIN32)
    set_target_properties(KunClient PROPERTIES
        VERSION ${PROJECT_VERSION}
        PRODUCTNAME "KunClient"
        COMPANYNAME "KunAwa"
        LEGALCOPYRIGHT "Copyright (C) 2026 KunAwa. All rights reserved."
        FILEDESCRIPTION "KunClient"
    )
endif()

# ── 安装 ──────────────────────────────────────────────────────────────────────
install(TARGETS KunClient
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

# ── 测试 ──────────────────────────────────────────────────────────────────────
include(CTest)
if(BUILD_TESTING)
    # 核心逻辑测试（无 UI/协议/传输依赖）
    add_executable(kc-core-tests
        tests/GptParserTest.cpp
        ${EDL_CORE_SOURCES}
        kc/security/Sha256.cpp
        kc/security/Digest.cpp
    )
    target_include_directories(kc-core-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-core-tests PRIVATE Qt6::Core bcrypt ${KC_COMPRESSION_LIBS})
    if(MSVC)
        target_compile_options(kc-core-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-core-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME core-tests COMMAND kc-core-tests)

    # 免合并 super 测试：合成 META 包（super_meta + super_def + 镜像）覆盖布局发现、
    # nvId 指针、触发条件（缺失 / 0 字节占位）与 extent 计划生成。每个测试文件自带
    # main()，与 core-tests 分开链接。
    add_executable(kc-dynsuper-tests
        tests/DynamicSuperTest.cpp
        storage/DynamicSuper.cpp
        storage/RawProgramParser.cpp
        storage/SparseImage.cpp
        kc/security/Sha256.cpp
    )
    target_include_directories(kc-dynsuper-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-dynsuper-tests PRIVATE Qt6::Core bcrypt)
    if(MSVC)
        target_compile_options(kc-dynsuper-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-dynsuper-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME dynsuper-tests COMMAND kc-dynsuper-tests)

    # EDL 维护操作纯逻辑测试（BCB 载荷 / devinfo 解锁字节 / 预设名单 / GPT 命名）
    add_executable(kc-maintenance-tests
        tests/EdlMaintenanceTest.cpp
        core/EdlMaintenance.cpp
        core/PartitionProtection.cpp
    )
    target_include_directories(kc-maintenance-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-maintenance-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(kc-maintenance-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-maintenance-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME maintenance-tests COMMAND kc-maintenance-tests)

    # LUN 容量解析测试（NUM_DISK_SECTORS 表达式 + getstorageinfo 三种响应形态）
    add_executable(kc-luncapacity-tests
        tests/LunCapacityTest.cpp
        storage/LunCapacity.cpp
    )
    target_include_directories(kc-luncapacity-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-luncapacity-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(kc-luncapacity-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-luncapacity-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME luncapacity-tests COMMAND kc-luncapacity-tests)

    # 「小米字库牌子」离线逻辑测试（INFO: ufs/emmc 解析 / 厂牌表 / 引导文件发现）。
    # 只链 Qt6::Core + SerialPort —— 探测流程（Sahara + NOP）要真机，不进单测。
    add_executable(kc-storagebrand-tests
        tests/StorageBrandTest.cpp
        kc/devices/StorageBrandProbe.cpp
    )
    target_include_directories(kc-storagebrand-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-storagebrand-tests PRIVATE Qt6::Core Qt6::SerialPort)
    if(MSVC)
        target_compile_options(kc-storagebrand-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-storagebrand-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME storagebrand-tests COMMAND kc-storagebrand-tests)

    # OCDT 生成/分析测试（TDCO 头 / geyixue 配置块 / 两块平台 OSIG 差异 / 三种变体）
    add_executable(kc-ocdt-tests
        tests/OcdtGeneratorTest.cpp
        storage/OcdtGenerator.cpp
    )
    target_include_directories(kc-ocdt-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-ocdt-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(kc-ocdt-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-ocdt-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ocdt-tests COMMAND kc-ocdt-tests)

    # USB 连接状态粘滞测试（保持期 / 连续确认 / 模式切换立即采纳 / 手动清零）
    add_executable(kc-usbsticky-tests
        tests/UsbStateStickinessTest.cpp
    )
    target_include_directories(kc-usbsticky-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-usbsticky-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(kc-usbsticky-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-usbsticky-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME usbsticky-tests COMMAND kc-usbsticky-tests)

    # OFP / OPS 解包测试（AES 向量 / 密钥派生 / MTK·QC OFP 合成样本 / OPS 三套 MBox）
    add_executable(kc-oplus-tests
        tests/OplusPackageTest.cpp
        oplus/AesCfb.cpp
        oplus/OfpExtractor.cpp
        oplus/OpsExtractor.cpp
        kc/security/Digest.cpp
    )
    target_include_directories(kc-oplus-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-oplus-tests PRIVATE Qt6::Core bcrypt)
    if(MSVC)
        target_compile_options(kc-oplus-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-oplus-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME oplus-tests COMMAND kc-oplus-tests)

    # Android OTA payload.bin（CrAU）解析与提取：REPLACE / BZ / XZ / ZSTD / ZERO / 增量拒绝
    add_executable(kc-payload-tests
        tests/PayloadParserTest.cpp
        storage/PayloadParser.cpp
    )
    target_include_directories(kc-payload-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-payload-tests PRIVATE Qt6::Core ${KC_COMPRESSION_LIBS})
    if(MSVC)
        target_compile_options(kc-payload-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-payload-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME payload-tests COMMAND kc-payload-tests)

    # EDL 假设备回归：不打开串口，覆盖 RAW 预检和擦除哨兵可信度/取消终态。
    add_executable(kc-edlproto-tests
        tests/EdlProtocolTest.cpp
        ${EDL_FULL_SOURCES}
        kc/security/Sha256.cpp
        # OFP 解包用 kc::crypto::hashBytes 做密钥派生（MD5），必须一起链
        kc/security/Digest.cpp
        # 2026-10-18：EdlWorker 的分片大小改成读 AppSettings::edlSliceBytes()（兼容模式），
        # 这两个源文件必须一起链进来，否则 undefined reference。
        kc/config/AppSettings.cpp
        kc/config/AppStatePaths.cpp
    )
    target_include_directories(kc-edlproto-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-edlproto-tests PRIVATE Qt6::Core Qt6::SerialPort bcrypt
        ${KC_COMPRESSION_LIBS})
    add_test(NAME edlproto-tests COMMAND kc-edlproto-tests)

    # MTK scatter 解析测试（纯逻辑：经典 .txt + v6 XML + 错误路径）
    add_executable(kc-mtk-tests
        tests/ScatterParserTest.cpp
        ${MTK_PARSER_SOURCES}
    )
    target_include_directories(kc-mtk-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-mtk-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(kc-mtk-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-mtk-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME mtk-tests COMMAND kc-mtk-tests)

    # MTK 协议测试：接「假设备」（MockMtkDevice）把 BROM 回显 / DA 文件 /
    # XFlash 包格式测穿。本机没有 MTK 真机，这是能拿到的最高强度验证。
    # 单列一个目标是因为每个测试文件自带 main()。
    add_executable(kc-mtkproto-tests
        tests/MtkProtocolTest.cpp
        ${MTK_PROTO_SOURCES}
        # MtkTransport 的波特率 / 包间隔改成读 AppSettings（COM 速率 / 兼容模式）。
        kc/config/AppSettings.cpp
        kc/config/AppStatePaths.cpp
    )
    target_include_directories(kc-mtkproto-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # SerialPort：BromSession::openSerial 引用 SerialTransport（即便单测走假设备，
    # 该函数仍会被编进目标文件，符号必须能解析）。
    target_link_libraries(kc-mtkproto-tests PRIVATE Qt6::Core Qt6::SerialPort)
    if(MSVC)
        target_compile_options(kc-mtkproto-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-mtkproto-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME mtkproto-tests COMMAND kc-mtkproto-tests)

    # ── 停更离线版（2.0.6）：RequestSigner / EnvGuard 等联网单测已随功能移除 ──

    # 日志采集单测（本地日志的纯逻辑层：落盘 / 采集裁剪 / 载荷结构）
    add_executable(kc-diag-tests
        tests/LogCollectorTest.cpp
        kc/diag/SessionLog.cpp
        kc/diag/LogCollector.cpp
        kc/config/AppStatePaths.cpp
    )
    target_include_directories(kc-diag-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-diag-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(kc-diag-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-diag-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME diag-tests COMMAND kc-diag-tests)

    # UI 测试（含完整应用）
    add_executable(kc-ui-tests
        tests/MainWindowLogTest.cpp
        ${EDL_FULL_SOURCES}
        ${KC_CORE_SOURCES}
        ${UI_SOURCES}
        ${DIALOG_SOURCES}
        ${RESOURCE_FILES}
    )
    target_include_directories(kc-ui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-ui-tests PRIVATE
        Qt6::Core Qt6::Widgets Qt6::SerialPort Qt6::Xml Qt6::Network ${KC_COMPRESSION_LIBS})
    if(WIN32)
        # cfgmgr32：MTK 设备枚举用 CM_Get_Parent 读父设备（KC_CORE_SOURCES 里带进来）。
        # mf*：MainShell 的视频背景解码器随 UI_SOURCES 一起被带进测试目标。
        target_link_libraries(kc-ui-tests PRIVATE crypt32 bcrypt advapi32 iphlpapi wbemuuid ole32 oleaut32 setupapi cfgmgr32 mfplat mfreadwrite mfuuid)
    endif()
    if(MSVC)
        target_compile_options(kc-ui-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-ui-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ui-tests COMMAND kc-ui-tests)

    # EDL 高级功能页 UI 结构回归（竞品 9 项功能补齐的界面侧）：页签切换 / 九项操作 /
    # LUN 选择器 / 重启菜单四项 / 未连接置灰。无头下不依赖 show()，沿用 ui-tests 范式。
    add_executable(kc-maintenance-ui-tests
        tests/EdlMaintenanceUiTest.cpp
        ${EDL_FULL_SOURCES}
        ${KC_CORE_SOURCES}
        ${UI_SOURCES}
        ${DIALOG_SOURCES}
        ${RESOURCE_FILES}
    )
    target_include_directories(kc-maintenance-ui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-maintenance-ui-tests PRIVATE
        Qt6::Core Qt6::Widgets Qt6::SerialPort Qt6::Xml Qt6::Network ${KC_COMPRESSION_LIBS})
    if(WIN32)
        target_link_libraries(kc-maintenance-ui-tests PRIVATE crypt32 bcrypt advapi32 iphlpapi wbemuuid ole32 oleaut32 setupapi cfgmgr32 mfplat mfreadwrite mfuuid)
    endif()
    if(MSVC)
        target_compile_options(kc-maintenance-ui-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-maintenance-ui-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME maintenance-ui-tests COMMAND kc-maintenance-ui-tests)

    # 固件校验纯逻辑测试（移植自 TMFTool 的 MD5 / SHA-256 校验 + 生成校验文件）：
    # 摘要测试向量、流式跨块一致性、coreutils 校验文件格式（文本 / 二进制 / CRLF）。
    add_executable(kc-checksum-tests
        tests/ChecksumTest.cpp
        kc/security/Digest.cpp
        kc/tools/FirmwareChecksum.cpp
    )
    target_include_directories(kc-checksum-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kc-checksum-tests PRIVATE Qt6::Core bcrypt)
    if(MSVC)
        target_compile_options(kc-checksum-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(kc-checksum-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME checksum-tests COMMAND kc-checksum-tests)

    # ── 停更离线版（2.0.6）：环境检测 / 上报协议测试（kc-env-tests）已随功能移除 ──
endif()
