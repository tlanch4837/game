@echo off
rem Builds tools\rail_junction_tex\dump_layouts.cpp and writes obj\rail_junction_layouts.json (input of gen_junction_tex.py).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\..\obj\rail_junction_tex" mkdir "%~dp0..\..\obj\rail_junction_tex"
cl /nologo /std:c++20 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\..\src" /Fo"%~dp0..\..\obj\rail_junction_tex\\" /Fe"%~dp0..\..\obj\rail_junction_tex\dump_layouts.exe" "%~dp0dump_layouts.cpp" >nul || exit /b 1
"%~dp0..\..\obj\rail_junction_tex\dump_layouts.exe" "%~dp0..\..\obj\rail_junction_layouts.json"
