@echo off
rem Builds and runs the city camera tests (MSVC). Output: obj\city_camera_test.exe
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj" mkdir "%~dp0..\obj"
cl /nologo /std:c++20 /EHsc /O2 /W4 /I "%~dp0..\src" /Fo"%~dp0..\obj\\" /Fe"%~dp0..\obj\city_camera_test.exe" "%~dp0city_camera_test.cpp" || exit /b 1
"%~dp0..\obj\city_camera_test.exe"
