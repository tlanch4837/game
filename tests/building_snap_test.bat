@echo off
rem Builds and runs tests\building_snap_test.cpp (the shared building road snap: building_snap.h; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\building_snap_test" mkdir "%~dp0..\obj\building_snap_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\building_snap_test\\" /Fe"%~dp0..\obj\building_snap_test\building_snap_test.exe" "%~dp0building_snap_test.cpp" || exit /b 1
"%~dp0..\obj\building_snap_test\building_snap_test.exe"
