cmake_minimum_required(VERSION 3.25)
project(SplitDisplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/W4 /permissive- /EHsc /utf-8 /await:strict)

add_library(sdcommon STATIC src/common/panel.cpp src/common/log.cpp src/common/shm.cpp)
target_include_directories(sdcommon PUBLIC src/common)
target_link_libraries(sdcommon PUBLIC windowsapp d3d11 dxgi)

add_executable(panel_test src/panel_test/main.cpp)
target_link_libraries(panel_test PRIVATE sdcommon)

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
