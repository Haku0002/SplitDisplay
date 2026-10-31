cmake_minimum_required(VERSION 3.25)
# Single source of truth for the version: the generated manifest, the VERSIONINFO resource,
# the settings window and scripts/make-release.ps1 all read it from here.
project(SplitDisplay VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_testing()

option(SD_WERROR "Treat compiler warnings as errors" ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/W4 /permissive- /EHsc /utf-8 /await:strict /sdl /guard:cf)
if(SD_WERROR)
    add_compile_options(/WX)
endif()
add_link_options(/guard:cf /CETCOMPAT)

add_library(sdcommon STATIC src/common/panel.cpp src/common/log.cpp src/common/shm.cpp src/common/layout.cpp)
target_include_directories(sdcommon PUBLIC src/common)
target_link_libraries(sdcommon PUBLIC windowsapp d3d11 dxgi ntdll ole32)


# ---- IDD driver (UMDF 2 / IddCx), built against the WDK NuGet package in third_party/wdk ----
set(WDK_ROOT ${CMAKE_SOURCE_DIR}/third_party/wdk/c)
set(WDK_VER 10.0.26100.0)
add_library(SplitDisplayIdd SHARED driver/Driver.cpp)
target_include_directories(SplitDisplayIdd PRIVATE
    src/common
    ${WDK_ROOT}/Include/wdf/umdf/2.25
    ${WDK_ROOT}/Include/${WDK_VER}/um/iddcx/1.10)
target_compile_definitions(SplitDisplayIdd PRIVATE
    UMDF_DRIVER UMDF_VERSION_MAJOR=2 UMDF_VERSION_MINOR=25
    IDDCX_VERSION_MAJOR=1 IDDCX_VERSION_MINOR=10 IDDCX_MINIMUM_VERSION_REQUIRED=4
    _ATL_NO_WIN_SUPPORT)
target_compile_options(SplitDisplayIdd PRIVATE /wd4201 /wd4324 /wd4005 /wd4471)
target_link_libraries(SplitDisplayIdd PRIVATE
    ${WDK_ROOT}/Lib/wdf/umdf/x64/2.25/WdfDriverStubUm.lib
    ${WDK_ROOT}/Lib/${WDK_VER}/um/x64/iddcx/1.10/iddcxstub.lib
    OneCoreUAP ntdll avrt d3d11 dxgi)

add_executable(sdctl src/sdctl/main.cpp)
target_link_libraries(sdctl PRIVATE sdcommon setupapi newdev)

configure_file(src/splitdisplay/splitdisplay.manifest.in ${CMAKE_BINARY_DIR}/generated/splitdisplay.manifest @ONLY)

add_executable(splitdisplay
    src/splitdisplay/main.cpp src/splitdisplay/topology.cpp src/splitdisplay/gui.cpp src/splitdisplay/autostart.cpp
    src/splitdisplay/guard.cpp src/splitdisplay/frames.cpp
    src/splitdisplay/splitdisplay.rc ${CMAKE_BINARY_DIR}/generated/splitdisplay.manifest)
target_link_libraries(splitdisplay PRIVATE sdcommon comctl32 shcore taskschd secur32 comsuppw oleaut32 shell32 dwmapi slc wtsapi32 dbghelp mscms uxtheme d3dcompiler avrt)
target_compile_definitions(splitdisplay PRIVATE SPLITDISPLAY_VERSION="${PROJECT_VERSION}"
    SD_VER_MAJOR=${PROJECT_VERSION_MAJOR} SD_VER_MINOR=${PROJECT_VERSION_MINOR} SD_VER_PATCH=${PROJECT_VERSION_PATCH})
# No console window when started at logon; everything goes to splitdisplay.log.
target_link_options(splitdisplay PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFESTUAC:NO)

add_executable(layout_test tests/layout_test.cpp)
target_link_libraries(layout_test PRIVATE sdcommon)
add_test(NAME layout_test COMMAND layout_test)

add_executable(ring_test tests/ring_test.cpp)
target_include_directories(ring_test PRIVATE src/common)
add_test(NAME ring_test COMMAND ring_test)
