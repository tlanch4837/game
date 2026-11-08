@echo off
rem Builds and runs tests\rail_yard_test.cpp (rail yards: road snap, track ends, rail snaps, trains; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\rail_yard_test" mkdir "%~dp0..\obj\rail_yard_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\rail_yard_test\\" /Fe"%~dp0..\obj\rail_yard_test\rail_yard_test.exe" "%~dp0rail_yard_test.cpp" || exit /b 1
"%~dp0..\obj\rail_yard_test\rail_yard_test.exe"
