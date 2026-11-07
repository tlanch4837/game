@echo off
rem Builds and runs tests\sim_milestones_test.cpp (milestones; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\sim_milestones_test" mkdir "%~dp0..\obj\sim_milestones_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\sim_milestones_test\\" /Fe"%~dp0..\obj\sim_milestones_test\sim_milestones_test.exe" "%~dp0sim_milestones_test.cpp" || exit /b 1
"%~dp0..\obj\sim_milestones_test\sim_milestones_test.exe"
