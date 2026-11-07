@echo off
rem Compiles the Vulkan SPIR-V shaders from src\shaders into the folder given (default bin\shaders).
rem build.bat runs it; the engine runs it too (F5, or a shader file changing), into a scratch folder first.
setlocal
if not defined VULKAN_SDK (echo VULKAN_SDK is not set: no SPIR-V & exit /b 1)
set SRC=%~dp0src\shaders
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0bin\shaders
if not exist "%OUT%" mkdir "%OUT%"
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E VSMain -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E PSMain -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E PSOverlay -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_overlay_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E FullscreenVS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_vs.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E AoPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_ao_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E CloudsPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_clouds_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E CompositePS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_composite_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E TaaPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_taa_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E TonemapPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_tonemap_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E CloudShadowPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_cloudshadow_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E CloudResolvePS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_cloudresolve_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E ProbeTracePS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_probetrace_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E ProbeBlendPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_probeblend_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E ExposurePS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_exposure_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E BloomPS -DVULKAN -I "%SRC%" -Fo "%OUT%\post_bloom_ps.spv" "%SRC%\post.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E ShadowVS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_shadow_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E VSInstanced -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_inst_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E ShadowInstancedVS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_shadow_inst_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E FullscreenVS -DVULKAN -I "%SRC%" -Fo "%OUT%\selftest_vs.spv" "%SRC%\selftest.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E FillPS -DVULKAN -I "%SRC%" -Fo "%OUT%\selftest_fill_ps.spv" "%SRC%\selftest.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E SamplePS -DVULKAN -I "%SRC%" -Fo "%OUT%\selftest_sample_ps.spv" "%SRC%\selftest.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E VSVegetation -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_veg_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E ShadowVegetationVS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_shadow_veg_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E ShadowCutoutVS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_shadow_cut_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E PSShadowCutout -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_shadow_cut_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E CSReset -DVULKAN -I "%SRC%" -Fo "%OUT%\vegetation_CSReset.spv" "%SRC%\vegetation.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E CSPlace -DVULKAN -I "%SRC%" -Fo "%OUT%\vegetation_CSPlace.spv" "%SRC%\vegetation.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E CSArgs -DVULKAN -I "%SRC%" -Fo "%OUT%\vegetation_CSArgs.spv" "%SRC%\vegetation.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E CSRipple -DVULKAN -DWATER_COMPUTE -I "%SRC%" -Fo "%OUT%\water_CSRipple.spv" "%SRC%\water.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E CSWaves -DVULKAN -DWATER_COMPUTE -I "%SRC%" -Fo "%OUT%\water_CSWaves.spv" "%SRC%\water.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E CSSky -DVULKAN -DWATER_COMPUTE -I "%SRC%" -Fo "%OUT%\water_CSSky.spv" "%SRC%\water.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E WaterVS -DVULKAN -I "%SRC%" -Fo "%OUT%\water_vs.spv" "%SRC%\water.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E WaterPS -DVULKAN -I "%SRC%" -Fo "%OUT%\water_ps.spv" "%SRC%\water.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E PSMainMotion -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_motion_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E VSSkinnedMotion -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_skinned_motion_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E VSVegetationDepth -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_veg_depth_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E PSVegetationDepth -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_veg_depth_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E PSVegetationShade -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_veg_shade_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
rem City chunk bakes and volumes (Track R): the bake pass and ChunkVolPS (src\city_buildings.inl).
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E ChunkBakeVS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_chunk_bake_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E ChunkBakePS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_chunk_bake_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E ChunkVolVS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_chunk_vol_vs.spv" "%SRC%\mesh.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E ChunkVolPS -DVULKAN -I "%SRC%" -Fo "%OUT%\mesh_chunk_vol_ps.spv" "%SRC%\mesh.hlsl" || exit /b 1
rem The smooth in-game UI (Renderer::DrawUi): rounded boxes, shadows, capsules and SDF text.
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T vs_6_0 -E UiVS -DVULKAN -I "%SRC%" -Fo "%OUT%\ui_vs.spv" "%SRC%\ui.hlsl" || exit /b 1
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_0 -E UiPS -DVULKAN -I "%SRC%" -Fo "%OUT%\ui_ps.spv" "%SRC%\ui.hlsl" || exit /b 1
rem The ray traced ambient occlusion (VK_KHR_ray_query), used when the device has it.
"%VULKAN_SDK%\Bin\dxc.exe" -spirv -fspv-target-env=vulkan1.3 -T ps_6_5 -E AoPS -DVULKAN -DRT_AO -I "%SRC%" -Fo "%OUT%\post_ao_rt_ps.spv" "%SRC%\post.hlsl" || exit /b 1
