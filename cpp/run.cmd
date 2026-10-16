@echo off
if not exist "%~dp0dist\Release\jietuba_cpp.exe" (
    echo Build first: build.cmd
    exit /b 1
)
start "" "%~dp0dist\Release\jietuba_cpp.exe" %*
