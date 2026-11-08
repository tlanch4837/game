@echo off
rem Builds and runs tests\interact_test.cpp (tap vs drag, zone world plane, bulldoze + undo, rotate dial, info pick; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\interact_test" mkdir "%~dp0..\obj\interact_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\interact_test\\" /Fe"%~dp0..\obj\interact_test\interact_test.exe" "%~dp0interact_test.cpp" || exit /b 1
"%~dp0..\obj\interact_test\interact_test.exe"