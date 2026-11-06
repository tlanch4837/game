@echo off
rem Converts src\shaders into Metal Shading Language for the iOS Metal backend: HLSL -> SPIR-V (spirv.bat, dxc) ->
rem MSL 2.3 for iOS (spirv-cross, tools\msl.ps1), into the folder given (default src\shaders\msl, which is committed so
rem a Mac build needs no Vulkan SDK). Binding scheme: docs\metal_backend.md.
setlocal
if not defined VULKAN_SDK (echo VULKAN_SDK is not set: no MSL & exit /b 1)
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0src\shaders\msl
set SPV=%~dp0obj\spv_msl
if not exist "%SPV%" mkdir "%SPV%"
call "%~dp0spirv.bat" "%SPV%" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\msl.ps1" -SpvDir "%SPV%" -OutDir "%OUT%" || exit /b 1
