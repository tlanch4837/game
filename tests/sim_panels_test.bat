@echo off
rem Builds and runs tests\sim_panels_test.cpp (the sim phases' panel model and the overlay layers; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\sim_panels_test" mkdir "%~dp0..\obj\sim_panels_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\sim_panels_test\\" /Fe"%~dp0..\obj\sim_panels_test\sim_panels_test.exe" "%~dp0sim_panels_test.cpp" || exit /b 1
"%~dp0..\obj\sim_panels_test\sim_panels_test.exe"
