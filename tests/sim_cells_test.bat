@echo off
rem Builds and runs tests\sim_cells_test.cpp (the city sim's per-cell counters; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\sim_cells_test" mkdir "%~dp0..\obj\sim_cells_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\sim_cells_test\\" /Fe"%~dp0..\obj\sim_cells_test\sim_cells_test.exe" "%~dp0sim_cells_test.cpp" || exit /b 1
"%~dp0..\obj\sim_cells_test\sim_cells_test.exe"
