@echo off
rem Builds and runs tests\road_reticle_test.cpp (the aim dot and the connection snaps, src\gui\road_reticle.h, src\city\road_tool.h), headless.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\road_reticle_test" mkdir "%~dp0..\obj\road_reticle_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\src" /Fo"%~dp0..\obj\road_reticle_test\\" /Fe"%~dp0..\obj\road_reticle_test\road_reticle_test.exe" "%~dp0road_reticle_test.cpp" || exit /b 1
"%~dp0..\obj\road_reticle_test\road_reticle_test.exe"
