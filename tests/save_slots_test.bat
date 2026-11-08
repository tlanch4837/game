@echo off
rem Builds and runs tests\save_slots_test.cpp (the ten save slots, src\game\save_slots.h), headless.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\save_slots_test" mkdir "%~dp0..\obj\save_slots_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\src" /Fo"%~dp0..\obj\save_slots_test\\" /Fe"%~dp0..\obj\save_slots_test\save_slots_test.exe" "%~dp0save_slots_test.cpp" || exit /b 1
"%~dp0..\obj\save_slots_test\save_slots_test.exe"
