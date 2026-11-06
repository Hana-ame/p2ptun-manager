cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(p2ptun_win VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 静态链接 CRT，生成的 exe 在任何裸机 Windows 10/11 上免装运行库直接双击运行
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /W3)
else()
    add_compile_options(-Wall -Wextra)
endif()

# 核心纯 C++17 库 (配置解析、傻瓜端口补全、p2ptun:// 配对码、进程与状态管理)
add_library(p2ptun_core STATIC
    src/core.cpp
    src/process_manager_win32.cpp
)
target_include_directories(p2ptun_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    target_link_libraries(p2ptun_core PUBLIC winhttp ws2_32 shell32 user32)
    add_executable(p2ptun-gui WIN32
        src/main_win32.cpp
        src/app.rc
    )
    target_link_libraries(p2ptun-gui PRIVATE
        p2ptun_core
        user32
        gdi32
        comctl32
        comdlg32
        shell32
        winhttp
        ws2_32
    )
    if(MINGW)
        target_link_options(p2ptun-gui PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
    endif()
    if(MSVC)
        # app.rc 已通过 RT_MANIFEST 嵌入自定义 DPI 清单，
        # 关闭链接器默认 manifest，避免 CVT1100 duplicate resource
        target_link_options(p2ptun-gui PRIVATE "/MANIFEST:NO")
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(p2ptun_core PUBLIC Threads::Threads)
    add_executable(p2ptun-gui src/main_win32.cpp)
    target_link_libraries(p2ptun-gui PRIVATE p2ptun_core Threads::Threads)
endif()

enable_testing()
add_executable(p2ptun_core_tests tests/test_core.cpp)
target_link_libraries(p2ptun_core_tests PRIVATE p2ptun_core)
add_test(NAME p2ptun_core_tests COMMAND p2ptun_core_tests)
