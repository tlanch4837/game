@echo off
rem Builds and runs tests\terrain_relief_test.cpp (the default city map's relief, before / after; headless).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\terrain_relief_test" mkdir "%~dp0..\obj\terrain_relief_test"
cl /nologo /std:c++20 /EHsc /O2 /W4 /I "%~dp0..\src" /Fo"%~dp0..\obj\terrain_relief_test\\" /Fe"%~dp0..\obj\terrain_relief_test\terrain_relief_test.exe" "%~dp0terrain_relief_test.cpp" || exit /b 1
"%~dp0..\obj\terrain_relief_test\terrain_relief_test.exe"
