@echo off
rem Builds bin\game.dll, the hot-reloaded gameplay code (game\*.cpp; interface in src\game_api.h).
rem The engine runs this itself when a file under game\ changes, then swaps the new DLL in while it runs.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR call %VCVARS% >NUL 2>&1 || exit /b 1
if not exist "%~dp0bin" mkdir "%~dp0bin"
if not exist "%~dp0obj\game" mkdir "%~dp0obj\game"
rem A fresh PDB name each build: a debugger attached to the engine keeps the last one open.
set PDB=game_%RANDOM%%RANDOM%.pdb
del /q "%~dp0obj\game\game_*.pdb" 2>nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /Zi /LD /I"%~dp0src" /Fo"%~dp0obj\game\\" /Fd"%~dp0obj\game\\" /Fe"%~dp0obj\game\game.dll" "%~dp0game\*.cpp" /link /DEBUG /PDB:"%~dp0obj\game\%PDB%" user32.lib || exit /b 1
copy /y "%~dp0obj\game\game.dll" "%~dp0bin\game.dll" >NUL || exit /b 1
