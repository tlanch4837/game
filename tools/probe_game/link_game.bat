@echo off
rem Links the game without the editor (src\game\game_host.cpp, compiled without windows.h) with the portable engine
rem sources, to list what the iOS target will be missing. Needs build.bat to have built the third-party libs once.
rem Output: obj\link_game.txt and bin\link_game.exe.
setlocal
set ROOT=%~dp0..\..
set SRC=%ROOT%\src
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
set OBJ=%ROOT%\obj\link_game\
if not exist "%OBJ%" mkdir "%OBJ%"
set JOLT=%ROOT%\third_party\JoltPhysics
set RECAST=%ROOT%\third_party\recastnavigation
set LUAU=%ROOT%\third_party\luau
set INC=/I"%SRC%" /I"%JOLT%" /I"%RECAST%\Recast\Include" /I"%RECAST%\Detour\Include" /I"%RECAST%\DetourCrowd\Include" /I"%LUAU%\VM\include" /I"%LUAU%\Compiler\include"
set PORTABLE="%SRC%\game\game_host.cpp" "%SRC%\physics.cpp" "%SRC%\ragdoll.cpp" "%SRC%\ai\nav_mesh.cpp" "%SRC%\ai\npc_system.cpp" "%SRC%\script\luau_host.cpp" "%SRC%\script\api_registry.cpp" "%SRC%\script\api_builtin.cpp" "%SRC%\script\script_remote.cpp" "%SRC%\audio.cpp" "%SRC%\render\log.cpp" "%SRC%\platform\platform_win.cpp" "%~dp0link_main.cpp"
cl /nologo /std:c++20 /EHsc /bigobj /W0 /MP /DTBD_NO_WIN32_HEADERS=1 /D_CRT_SECURE_NO_WARNINGS %INC% /Fo"%OBJ%\" /Fe"%ROOT%\bin\link_game.exe" %PORTABLE% /link /SUBSYSTEM:CONSOLE "%ROOT%\obj\jolt\jolt.lib" "%ROOT%\obj\recast\recast.lib" "%ROOT%\obj\luau\luau.lib" > "%ROOT%\obj\link_game.txt" 2>&1
set RC=%ERRORLEVEL%
findstr /c:" error " /c:"unresolved external" "%ROOT%\obj\link_game.txt"
exit /b %RC%
