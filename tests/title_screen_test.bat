@echo off
rem Builds and runs tests\title_screen_test.cpp (the title screen, loading screen and slot picker, src\gui\title_screen.h), headless.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\title_screen_test" mkdir "%~dp0..\obj\title_screen_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\src" /Fo"%~dp0..\obj\title_screen_test\\" /Fe"%~dp0..\obj\title_screen_test\title_screen_test.exe" "%~dp0title_screen_test.cpp" || exit /b 1
"%~dp0..\obj\title_screen_test\title_screen_test.exe"
