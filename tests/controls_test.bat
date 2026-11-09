@echo off
rem Builds and runs tests\controls_test.cpp (Settings > Controls: defaults, clamping, settings.ini, gestures, camera, menu scroll; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\controls_test" mkdir "%~dp0..\obj\controls_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\controls_test\\" /Fe"%~dp0..\obj\controls_test\controls_test.exe" "%~dp0controls_test.cpp" || exit /b 1
"%~dp0..\obj\controls_test\controls_test.exe"