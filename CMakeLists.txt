cmake_minimum_required(VERSION 3.16)
project(RandomPicker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 图标与音效资源：生成一份带绝对路径的 .rc，避免 rc.exe / windres 依赖工作目录
set(WIN_WAV "${CMAKE_CURRENT_SOURCE_DIR}/res/win.wav")
string(REPLACE "\\" "/" WIN_WAV "${WIN_WAV}")
set(WIN_ICO "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
string(REPLACE "\\" "/" WIN_ICO "${WIN_ICO}")
configure_file(res/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

set(RP_SOURCES src/RandomPicker.cpp)

# 有资源编译器就把 wav 打进 exe；没有也能编过，只是要放 win.wav 到 exe 旁边
include(CheckLanguage)
check_language(RC)
if(CMAKE_RC_COMPILER)
    enable_language(RC)
    list(APPEND RP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
else()
    message(STATUS "未找到资源编译器，音效将退回到 exe 同目录的 win.wav")
endif()

add_executable(RandomPicker WIN32 ${RP_SOURCES})

target_link_libraries(RandomPicker PRIVATE user32 gdi32 winmm)

if (MSVC)
    # 静态 CRT，生成单文件 exe；/utf-8 让源码里的中文字面量正确编译
    target_compile_options(RandomPicker PRIVATE /utf-8)
    set_property(TARGET RandomPicker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    # MinGW：-municode 走 wWinMain，-static 不依赖 libgcc/libstdc++ 运行库
    target_compile_options(RandomPicker PRIVATE -O2 -Wall -municode)
    target_link_options(RandomPicker PRIVATE -static -municode -mwindows)
endif()
