@echo off
chcp 65001 >nul
setlocal

rem ---------------------------------------------------------------
rem  用 MSVC 构建（推荐在 "x64 Native Tools Command Prompt" 里运行）
rem ---------------------------------------------------------------

where cl >nul 2>nul
if errorlevel 1 (
    echo [错误] 没找到 cl.exe。
    echo        请从开始菜单打开 "x64 Native Tools Command Prompt for VS"，
    echo        在该窗口里进入本目录后再运行本脚本。
    exit /b 1
)

cl /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE ^
   src\RandomPicker.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib /OUT:RandomPicker.exe

if errorlevel 1 (
    echo [失败] 编译出错。
    exit /b 1
)

echo.
echo [完成] 已生成 RandomPicker.exe
endlocal
