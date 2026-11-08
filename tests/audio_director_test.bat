@echo off
rem Builds and runs tests\audio_director_test.cpp (the city music playlist, radio duck and ambience mix; headless).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0..\obj\audio_director_test" mkdir "%~dp0..\obj\audio_director_test"
cl /nologo /std:c++20 /EHsc /W4 /O2 /I"%~dp0..\src" /Fo"%~dp0..\obj\audio_director_test\\" /Fe"%~dp0..\obj\audio_director_test\audio_director_test.exe" "%~dp0audio_director_test.cpp" || exit /b 1
"%~dp0..\obj\audio_director_test\audio_director_test.exe"
