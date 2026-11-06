@echo off
rem Builds and runs tests\road_test.cpp (the city road network and tool, headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\road_test" mkdir "%~dp0..\obj\road_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\road_test\\" /Fe"%~dp0..\obj\road_test\road_test.exe" "%~dp0road_test.cpp" || exit /b 1
"%~dp0..\obj\road_test\road_test.exe"
