@echo off
rem Compiles tools\probe_game\probe_game.cpp (the game without windows.h) and lists what still needs Windows.
rem Nothing is linked. Output: obj\probe_game.txt.
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%ROOT%\obj" mkdir "%ROOT%\obj"
set JOLT=%ROOT%\third_party\JoltPhysics
set RECAST=%ROOT%\third_party\recastnavigation
set LUAU=%ROOT%\third_party\luau
cl /nologo /c /std:c++20 /EHsc /bigobj /W0 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%\src" /I"%JOLT%" /I"%RECAST%\Recast\Include" /I"%RECAST%\Detour\Include" /I"%RECAST%\DetourCrowd\Include" /I"%LUAU%\VM\include" /I"%LUAU%\Compiler\include" /Fo"%ROOT%\obj\probe_game.obj" "%~dp0probe_game.cpp" > "%ROOT%\obj\probe_game.txt" 2>&1
set RC=%ERRORLEVEL%
findstr /c:" error " "%ROOT%\obj\probe_game.txt"
exit /b %RC%
