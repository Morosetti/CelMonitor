cmake_minimum_required(VERSION 3.24)
project(CelMonitorHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no redistributable needed

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /WX /permissive- /utf-8 /MP)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Core pipeline, shared by the app, the CLI and the tests.
add_library(celmon_core STATIC
    src/util/Log.cpp
    src/display/VirtualDisplay.cpp
    src/capture/D3DContext.cpp
    src/capture/DesktopDuplicator.cpp
    src/encoder/ColorConverter.cpp
    src/encoder/MfEncoder.cpp
    src/pipeline/VideoPipeline.cpp
    src/transport/AdbTransport.cpp
    src/transport/AoaTransport.cpp
    src/session/HostSession.cpp
    src/app/Settings.cpp
    src/app/Controller.cpp
    src/app/PhoneDetect.cpp
    src/util/ProcessStats.cpp
)
target_include_directories(celmon_core PUBLIC src ${REPO_ROOT}/common ${REPO_ROOT}/windows/shared)
target_link_libraries(celmon_core PUBLIC setupapi d3d11 dxgi mf mfplat mfuuid avrt winmm ws2_32)

# The app (Win32 UI)
add_executable(CelMonitor WIN32 src/ui/main.cpp src/ui/MainWindow.cpp src/ui/AdvancedDialog.cpp src/util/Autostart.cpp res/CelMonitor.rc)
target_link_libraries(CelMonitor PRIVATE celmon_core comctl32 pdh shell32)

add_executable(celmon-cli tools/celmon_cli.cpp)
target_link_libraries(celmon-cli PRIVATE celmon_core dwmapi windowscodecs newdev crypt32)

add_executable(celmon-tests tests/protocol_tests.cpp)
target_link_libraries(celmon-tests PRIVATE celmon_core)
target_compile_definitions(celmon-tests PRIVATE CELMON_TESTVECTOR_DIR="${REPO_ROOT}/common/protocol/testvectors")

enable_testing()
add_test(NAME protocol COMMAND celmon-tests)
