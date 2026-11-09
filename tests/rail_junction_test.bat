@echo off
rem Builds and runs tests\rail_junction_test.cpp (rail junctions: switches, diamonds, crossovers, level crossings, routing, models; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\rail_junction_test" mkdir "%~dp0..\obj\rail_junction_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\rail_junction_test\\" /Fe"%~dp0..\obj\rail_junction_test\rail_junction_test.exe" "%~dp0rail_junction_test.cpp" || exit /b 1
"%~dp0..\obj\rail_junction_test\rail_junction_test.exe"
