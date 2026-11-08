@echo off
rem Builds and runs tests\road_toolkit_test.cpp (the road / rail touch toolkit, src\gui\road_toolkit.h), headless.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\road_toolkit_test" mkdir "%~dp0..\obj\road_toolkit_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\src" /Fo"%~dp0..\obj\road_toolkit_test\\" /Fe"%~dp0..\obj\road_toolkit_test\road_toolkit_test.exe" "%~dp0road_toolkit_test.cpp" || exit /b 1
"%~dp0..\obj\road_toolkit_test\road_toolkit_test.exe"
