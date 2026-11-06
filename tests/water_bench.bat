@echo off
rem Builds and runs the water benchmark (MSVC, optimized). Output: obj\water_bench.exe
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj" mkdir "%~dp0..\obj"
cl /nologo /std:c++20 /EHsc /O2 /W4 /Fo"%~dp0..\obj\\" /Fe"%~dp0..\obj\water_bench.exe" "%~dp0water_bench.cpp" || exit /b 1
"%~dp0..\obj\water_bench.exe"
