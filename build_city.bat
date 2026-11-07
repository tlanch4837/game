@echo off
rem Builds bin\tbd_engine.exe and bin\tbd_hub.exe (project manager) with MSVC
rem (Visual Studio 2022 Community), plus the Vulkan shaders (SPIR-V, via dxc from the Vulkan SDK).
rem Run from anywhere.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul 2>&1 || exit /b 1
if not exist "%~dp0bin" mkdir "%~dp0bin"
if not exist "%~dp0bin\shaders" mkdir "%~dp0bin\shaders"
if not exist "%~dp0obj" mkdir "%~dp0obj"

rem --- shaders: Direct3D 12 compiles the .hlsl at startup; Vulkan needs SPIR-V now
copy /y "%~dp0src\shaders\*.hlsl" "%~dp0bin\shaders\" >nul
copy /y "%~dp0src\shaders\*.hlsli" "%~dp0bin\shaders\" >nul
if defined VULKAN_SDK (
  call "%~dp0spirv.bat" "%~dp0bin\shaders" || exit /b 1
) else (
  echo VULKAN_SDK is not set: skipping SPIR-V shaders
)

rem --- the ray traced ambient occlusion for Direct3D 12 (DXR 1.1 ray queries) needs DXIL: the Windows SDK's dxc, which signs it
set WDXC="%WindowsSdkVerBinPath%x64\dxc.exe"
if exist %WDXC% (
  %WDXC% -nologo -T vs_6_5 -E FullscreenVS -Fo "%~dp0bin\shaders\post_fullscreen_vs.dxil" "%~dp0src\shaders\post.hlsl" || exit /b 1
  %WDXC% -nologo -T ps_6_5 -E AoPS -D RT_AO -Fo "%~dp0bin\shaders\post_ao_rt_ps.dxil" "%~dp0src\shaders\post.hlsl" || exit /b 1
) else (echo No Windows SDK dxc: Direct3D 12 runs without ray traced occlusion)

rem --- executables
set CFLAGS=/nologo /bigobj /std:c++20 /EHsc /W4 /O2 /Zi /Fd"%~dp0obj\\" /Fo"%~dp0obj\\"
set VKINC=
set VKLIB=
if defined VULKAN_SDK set VKINC=/I"%VULKAN_SDK%\Include" /DTBD_HAVE_VULKAN
if defined VULKAN_SDK set VKLIB=/LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib
rem --- physics: Jolt (third_party\JoltPhysics, MIT), built once into obj\jolt\jolt.lib. Same CRT and math flags as the engine.
set JOLT=%~dp0third_party\JoltPhysics
set JOLTOBJ=%~dp0obj\jolt\
if not exist "%JOLTOBJ%jolt.lib" (
  if not exist "%JOLTOBJ%" mkdir "%JOLTOBJ%"
  if exist "%JOLTOBJ%sources.rsp" del "%JOLTOBJ%sources.rsp"
  for /r "%JOLT%\Jolt" %%f in (*.cpp) do echo "%%f">>"%JOLTOBJ%sources.rsp"
  cl /nologo /std:c++20 /EHsc /O2 /W0 /MP /c /I"%JOLT%" /Fd"%JOLTOBJ%\" /Fo"%JOLTOBJ%\" @"%JOLTOBJ%sources.rsp" || exit /b 1
  lib /nologo /OUT:"%JOLTOBJ%jolt.lib" "%JOLTOBJ%*.obj" || exit /b 1
)
rem --- navigation: Recast / Detour / DetourCrowd (third_party\recastnavigation, zlib), built once into obj\recast\recast.lib
set RECAST=%~dp0third_party\recastnavigation
set RECASTOBJ=%~dp0obj\recast\
if not exist "%RECASTOBJ%recast.lib" (
  if not exist "%RECASTOBJ%" mkdir "%RECASTOBJ%"
  cl /nologo /std:c++20 /EHsc /O2 /W0 /MP /c /I"%RECAST%\Recast\Include" /I"%RECAST%\Detour\Include" /I"%RECAST%\DetourCrowd\Include" /Fo"%RECASTOBJ%\" "%RECAST%\Recast\Source\*.cpp" "%RECAST%\Detour\Source\*.cpp" "%RECAST%\DetourCrowd\Source\*.cpp" || exit /b 1
  lib /nologo /OUT:"%RECASTOBJ%recast.lib" "%RECASTOBJ%*.obj" || exit /b 1
)
rem --- scripting: Luau (third_party\luau, MIT), built once into obj\luau\luau.lib. Same block as script_test.bat (headless runner).
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
set SCRIPTSRC="%~dp0src\script\luau_host.cpp" "%~dp0src\script\api_registry.cpp" "%~dp0src\script\api_builtin.cpp" "%~dp0src\script\script_remote.cpp"
rem --- face mocap: ONNX Runtime (third_party\onnxruntime) + Media Foundation video decode
set ORT=%~dp0third_party\onnxruntime
set MOCAPSRC="%~dp0src\mocap\face_tracker.cpp" "%~dp0src\mocap\video_reader.cpp" "%~dp0src\mocap\face_preview.cpp" "%~dp0src\mocap\face_driver.cpp" "%~dp0src\mocap\face_draw.cpp" "%~dp0src\mocap\face_live.cpp" "%~dp0src\mocap\face_live_window.cpp" "%~dp0src\mocap\face_live_tab.cpp" "%~dp0src\mocap\face_fit.cpp" "%~dp0src\mocap\face_lipsync.cpp"
set MOCAPLIB=mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib winmm.lib
cl %CFLAGS% %VKINC% /I"%~dp0src" /I"%JOLT%" /I"%RECAST%\Recast\Include" /I"%RECAST%\Detour\Include" /I"%RECAST%\DetourCrowd\Include" /I"%LUAU%\VM\include" /I"%LUAU%\Compiler\include" /D_CRT_SECURE_NO_WARNINGS /DTBD_NO_ONNX /Fe"%~dp0bin\tbd_engine.exe" %SCRIPTSRC% %MOCAPSRC% "%~dp0src\mocap\face_mocap_ui.cpp" "%~dp0src\character\creator.cpp" "%~dp0src\character\clothes_maker.cpp" "%~dp0src\main.cpp" "%~dp0src\platform\platform_win.cpp" "%~dp0src\tool_panel.cpp" "%~dp0src\outliner.cpp" "%~dp0src\audio.cpp" "%~dp0src\inspector.cpp" "%~dp0src\content_browser.cpp" "%~dp0src\render\log.cpp" "%~dp0src\render\renderer_d3d12.cpp" "%~dp0src\render\renderer_vulkan.cpp" "%~dp0src\physics.cpp" "%~dp0src\ragdoll.cpp" "%~dp0src\ai\nav_mesh.cpp" "%~dp0src\ai\npc_system.cpp" /link /DEBUG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib d3d12.lib dxgi.lib d3dcompiler.lib %MOCAPLIB% %VKLIB% "%JOLTOBJ%jolt.lib" "%RECASTOBJ%recast.lib" "%LUAUOBJ%luau.lib" || exit /b 1
cl %CFLAGS% /Fe"%~dp0bin\tbd_hub.exe" "%~dp0src\hub\hub_main.cpp" "%~dp0src\platform\platform_win.cpp" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib ole32.lib shell32.lib uuid.lib uxtheme.lib || exit /b 1
echo Built %~dp0bin\tbd_engine.exe and tbd_hub.exe
