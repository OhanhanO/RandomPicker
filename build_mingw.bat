@echo off
chcp 65001 >nul
setlocal

rem ---------------------------------------------------------------
rem  用 MinGW-w64 (g++) 构建
rem ---------------------------------------------------------------

where g++ >nul 2>nul
if errorlevel 1 (
    echo [错误] 没找到 g++.exe，请先把 MinGW-w64 的 bin 目录加进 PATH。
    exit /b 1
)

g++ -std=c++17 -O2 -Wall -municode -static -mwindows ^
    src\RandomPicker.cpp -o RandomPicker.exe -luser32 -lgdi32

if errorlevel 1 (
    echo [失败] 编译出错。
    exit /b 1
)

echo.
echo [完成] 已生成 RandomPicker.exe
endlocal
