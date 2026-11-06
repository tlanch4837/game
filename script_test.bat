@echo off
rem Builds bin\tbd_script_test.exe: the headless script runner (Luau + the API registry, no window, no GPU). Separate from build.bat so
rem it can be built and run while the engine is open. Usage after building:
rem   bin\tbd_script_test.exe --scripts testproject\WesternGame\Scripts --script-test testproject\WesternGame\Scripts\tests\door_test.luau
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0bin" mkdir "%~dp0bin"
if not exist "%~dp0obj" mkdir "%~dp0obj"

rem --- Luau (third_party\luau, MIT), built once into obj\luau\luau.lib
set LUAU=%~dp0third_party\luau
set LUAUOBJ=%~dp0obj\luau\
set LUAUINC=/I"%LUAU%\VM\include" /I"%LUAU%\VM\src" /I"%LUAU%\Compiler\include" /I"%LUAU%\Compiler\src" /I"%LUAU%\Ast\include" /I"%LUAU%\Common\include" /I"%LUAU%\Bytecode\include" /I"%LUAU%\Config\include"
if not exist "%LUAUOBJ%luau.lib" (
  if not exist "%LUAUOBJ%" mkdir "%LUAUOBJ%"
  if exist "%LUAUOBJ%sources.rsp" del "%LUAUOBJ%sources.rsp"
  for /r "%LUAU%\VM\src" %%f in (*.cpp) do echo "%%f">>"%LUAUOBJ%sources.rsp"
  for /r "%LUAU%\Compiler\src" %%f in (*.cpp) do echo "%%f">>"%LUAUOBJ%sources.rsp"
  for /r "%LUAU%\Ast\src" %%f in (*.cpp) do echo "%%f">>"%LUAUOBJ%sources.rsp"
  for /r "%LUAU%\Bytecode\src" %%f in (*.cpp) do echo "%%f">>"%LUAUOBJ%sources.rsp"
  for /r "%LUAU%\Config\src" %%f in (*.cpp) do echo "%%f">>"%LUAUOBJ%sources.rsp"
  for /r "%LUAU%\Common\src" %%f in (*.cpp) do echo "%%f">>"%LUAUOBJ%sources.rsp"
  cl /nologo /std:c++17 /EHsc /O2 /W0 /MP /c %LUAUINC% /Fd"%LUAUOBJ%\" /Fo"%LUAUOBJ%\" @"%LUAUOBJ%sources.rsp" || exit /b 1
  lib /nologo /OUT:"%LUAUOBJ%luau.lib" "%LUAUOBJ%*.obj" || exit /b 1
)

if not exist "%~dp0obj\script_main" mkdir "%~dp0obj\script_main"
cl /nologo /std:c++20 /EHsc /W4 /O2 /Zi /Fd"%~dp0obj\\script_test.pdb" /Fo"%~dp0obj\script_main\\" %LUAUINC% /Fe"%~dp0bin\tbd_script_test.exe" "%~dp0src\script\script_test_main.cpp" "%~dp0src\script\luau_host.cpp" "%~dp0src\script\api_registry.cpp" "%~dp0src\script\api_builtin.cpp" "%~dp0src\script\script_remote.cpp" /link "%LUAUOBJ%luau.lib" || exit /b 1
echo Built %~dp0bin\tbd_script_test.exe
