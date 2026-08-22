@echo off
rem ============================================
rem MSVC build script, Visual Studio Build Tools
rem Run from x64 Native Tools Command Prompt or
rem run this script directly.
rem ============================================
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe not found.
    echo Install Visual Studio Build Tools, then run
    echo this script from x64 Native Tools Command Prompt.
    exit /b 1
)

cl /nologo /O2 /GL /W3 /EHsc /utf-8 cpu_led_client.cpp /Fe:cpu_led_client.exe /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib advapi32.lib gdi32.lib
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo Build OK: cpu_led_client.exe
endlocal
