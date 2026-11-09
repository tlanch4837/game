@echo off
rem Builds and runs tests\sim_land_bench.cpp (Phase 2 main-thread bench; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\sim_land_bench" mkdir "%~dp0..\obj\sim_land_bench"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\sim_land_bench\\" /Fe"%~dp0..\obj\sim_land_bench\sim_land_bench.exe" "%~dp0sim_land_bench.cpp" || exit /b 1
"%~dp0..\obj\sim_land_bench\sim_land_bench.exe"
