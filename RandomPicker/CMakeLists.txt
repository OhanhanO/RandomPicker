cmake_minimum_required(VERSION 3.16)
project(RandomPicker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(RandomPicker WIN32 src/RandomPicker.cpp)

target_link_libraries(RandomPicker PRIVATE user32 gdi32)

if (MSVC)
    # 静态 CRT，生成单文件 exe；/utf-8 让源码里的中文字面量正确编译
    target_compile_options(RandomPicker PRIVATE /utf-8 /W3 /O2 /EHsc)
    target_link_options(RandomPicker PRIVATE /SUBSYSTEM:WINDOWS)
    set_property(TARGET RandomPicker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    # MinGW：-municode 走 wWinMain，-static 不依赖 libgcc/libstdc++ 运行库
    target_compile_options(RandomPicker PRIVATE -O2 -Wall -municode)
    target_link_options(RandomPicker PRIVATE -static -municode -mwindows)
endif()
