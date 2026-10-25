cmake_minimum_required(VERSION 3.24)

project(SWT_EDL VERSION 0.11.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Xml)

qt_standard_project_setup()

qt_add_executable(SWT-EDL WIN32
    src/main.cpp
    src/core/Types.h
    src/core/DeviceIdentity.h
    src/core/DeviceIdentity.cpp
    src/core/PartitionProtection.h
    src/core/PartitionProtection.cpp
    src/transport/SerialTransport.h
    src/transport/SerialTransport.cpp
    src/protocol/Sahara.h
    src/protocol/Sahara.cpp
    src/protocol/Firehose.h
    src/protocol/Firehose.cpp
    src/storage/GptParser.h
    src/storage/GptParser.cpp
    src/storage/RawProgramParser.h
    src/storage/RawProgramParser.cpp
    src/storage/RawProgramWriter.h
    src/storage/RawProgramWriter.cpp
    src/storage/SparseImage.h
    src/storage/SparseImage.cpp
    src/storage/DynamicSuper.h
    src/storage/DynamicSuper.cpp
    src/storage/SystemVersionReader.h
    src/storage/SystemVersionReader.cpp
    src/oplus/OplusVipSession.h
    src/oplus/OplusVipSession.cpp
    src/app/EdlWorker.h
    src/app/EdlWorker.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    # headless CLI（`--cli`）：复用 EdlWorker，把能力暴露成 NDJSON 的 stdin/stdout 协议，
    # 供外部程序（KunClient）以子进程方式驱动。见 src/cli/HeadlessController.h 的协议说明。
    # ⚠️ 本目标仍是 WIN32（GUI 子系统）可执行文件：用管道重定向时 stdio 照常可用，
    #    但**直接在 cmd 里跑看不到输出**（要 `SWT-EDL.exe --cli < in.txt > out.txt`）。
    src/cli/HeadlessController.h
    src/cli/HeadlessController.cpp
    src/cli/StdinReader.h
    src/cli/StdinReader.cpp
    src/cli/HeadlessMain.cpp
    resources/resources.qrc
)

target_include_directories(SWT-EDL PRIVATE src)

target_link_libraries(SWT-EDL PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::SerialPort
    Qt6::Xml
)

if(MSVC)
    target_compile_options(SWT-EDL PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(SWT-EDL PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(NOT QT6_IS_SHARED_LIBS_BUILD)
        set_property(TARGET SWT-EDL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        target_compile_options(SWT-EDL PRIVATE /O1 /Gy /Gw)
        target_link_options(SWT-EDL PRIVATE /OPT:REF /OPT:ICF)
    endif()
else()
    target_compile_options(SWT-EDL PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT QT6_IS_SHARED_LIBS_BUILD)
    qt_import_plugins(SWT-EDL
        NO_DEFAULT
        INCLUDE
            Qt6::QWindowsIntegrationPlugin
    )
endif()

set_target_properties(SWT-EDL PROPERTIES
    OUTPUT_NAME "SWT-EDL"
    WIN32_EXECUTABLE TRUE
)

install(TARGETS SWT-EDL
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

include(CTest)
if(BUILD_TESTING)
    add_executable(swt-core-tests
        tests/GptParserTest.cpp
        src/core/Types.h
        src/core/DeviceIdentity.h
        src/core/DeviceIdentity.cpp
        src/core/PartitionProtection.h
        src/core/PartitionProtection.cpp
        src/storage/GptParser.h
        src/storage/GptParser.cpp
        src/storage/RawProgramParser.h
        src/storage/RawProgramParser.cpp
        src/storage/RawProgramWriter.h
        src/storage/RawProgramWriter.cpp
        src/storage/SparseImage.h
        src/storage/SparseImage.cpp
        src/storage/DynamicSuper.h
        src/storage/DynamicSuper.cpp
        src/storage/SystemVersionReader.h
        src/storage/SystemVersionReader.cpp
        src/oplus/OplusVipSession.h
        src/oplus/OplusVipSession.cpp
    )
    target_include_directories(swt-core-tests PRIVATE src)
    target_link_libraries(swt-core-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(swt-core-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME gpt-parser COMMAND swt-core-tests)

    add_executable(swt-ui-tests
        tests/MainWindowLogTest.cpp
        src/core/Types.h
        src/core/DeviceIdentity.h
        src/core/DeviceIdentity.cpp
        src/core/PartitionProtection.h
        src/core/PartitionProtection.cpp
        src/transport/SerialTransport.h
        src/transport/SerialTransport.cpp
        src/protocol/Sahara.h
        src/protocol/Sahara.cpp
        src/protocol/Firehose.h
        src/protocol/Firehose.cpp
        src/storage/GptParser.h
        src/storage/GptParser.cpp
        src/storage/RawProgramParser.h
        src/storage/RawProgramParser.cpp
        src/storage/RawProgramWriter.h
        src/storage/RawProgramWriter.cpp
        src/storage/SparseImage.h
        src/storage/SparseImage.cpp
        src/storage/DynamicSuper.h
        src/storage/DynamicSuper.cpp
        src/storage/SystemVersionReader.h
        src/storage/SystemVersionReader.cpp
        src/oplus/OplusVipSession.h
        src/oplus/OplusVipSession.cpp
        src/app/EdlWorker.h
        src/app/EdlWorker.cpp
        src/ui/MainWindow.h
        src/ui/MainWindow.cpp
        resources/resources.qrc
    )
    target_include_directories(swt-ui-tests PRIVATE src)
    target_link_libraries(swt-ui-tests PRIVATE
        Qt6::Core Qt6::Widgets Qt6::SerialPort Qt6::Xml)
    if(MSVC)
        target_compile_options(swt-ui-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME ui-log COMMAND swt-ui-tests)
endif()
