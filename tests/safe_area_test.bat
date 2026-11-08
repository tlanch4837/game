@echo off
rem Builds and runs tests\safe_area_test.cpp (the safe area over the touch GUI and the terrain kit, src\gui\safe_area.h, src\gui\terrain_kit.h), headless.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\safe_area_test" mkdir "%~dp0..\obj\safe_area_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\src" /Fo"%~dp0..\obj\safe_area_test\\" /Fe"%~dp0..\obj\safe_area_test\safe_area_test.exe" "%~dp0safe_area_test.cpp" || exit /b 1
"%~dp0..\obj\safe_area_test\safe_area_test.exe"
