@echo off
REM build_demo.bat — 编译 ttcc 并生成 demo.HEX
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
if errorlevel 1 exit /b 1
ttcc.exe --target mcs51 --model small -Idemo -o demo\demo.HEX demo\main.c demo\timer.c demo\uart.c demo\gpio.c
if errorlevel 1 (
    echo FAILED
    exit /b 1
)
echo demo\demo.HEX generated successfully
