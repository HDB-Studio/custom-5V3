@echo off
rem ============================================
rem MinGW-w64 build script
rem Requires g++ in PATH. Install MinGW-w64 and
rem add its bin dir to PATH, e.g. w64devkit.
rem ============================================
setlocal

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found in PATH.
    echo Install MinGW-w64 and add its bin dir to PATH.
    exit /b 1
)

g++ -O2 -s -static -Wall -Wextra -municode -mwindows cpu_led_client.cpp -o cpu_led_client.exe -luser32 -lshell32 -ladvapi32 -lgdi32
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo Build OK: cpu_led_client.exe
endlocal
