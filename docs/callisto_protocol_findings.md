# The Rendering of The Callisto Protocol: catalog of findings

Source: Jimenez and Petersen, "The Rendering of The Callisto Protocol", SIGGRAPH 2023 Advances in Real-Time Rendering course (335 slides, Striking Distance Studios, modified UE4).
URL: https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf

Method: the full slide text and speaker notes were extracted and read end to end. Formulas are images or math glyphs in the PDF and did not survive text extraction, so where a formula matters only its description and parameter table are recorded here. Slide numbers are in brackets.

## Part 0. Philosophy and framing (Jimenez)

1. Three pillars: Ground Truth, Consistency, Observation [10-13]. Ground truth means minimize real-time shortcuts, use pure runtime raytracing plus accurate cached lighting, and give lights long attenuation radii. Consistency means materials respond correctly in all situations, nearly all lights are shadowed (raytraced) and nearly all surfaces get accurate (raytraced) reflections. Observation means train to see subtleties, use photo reference and digital doubles.
2. The philosophy put them about 125 ms from the frame budget. The slide notes the figure was updated after the live talk [14].
3. Quality target was cinematic quality during gameplay, under any lighting and any view [4-5]. Hair and background were composited out of the comparison renders because they were not part of the digital double process; the jawline was adjusted for stylization.
4. "Controlling from captured texel to rendered pixel": the key skill is understanding capture, cleaning, texture and model processing, and pixel shaders as one chain [17].
5. There is no single key feature that unlocks photorealism. It is a maze of many decisions whose error probabilities multiply, and the perceptual differences that matter are smaller than the individual steps. Without a reference "compass" it is hard to escape [18-29].
6. Example decisions in that maze: gloss value, subsurface amount, skin color [30].
7. Do not detour into physics for its own sake. The more he studied light transport models, the more incomplete they looked [32-34].
8. Hoffman 2019, "Fresnel Equations Considered Harmful": the exact Fresnel equations can be less accurate than Schlick unless every step is done correctly. Lesson: more physical is not automatically closer to a photograph [34-35].
9. Eye reflectance: the by-the-book value from the tear film index of refraction is 2%, which is too low in practice [36-37].
10. Why eyes sparkle more when a person is emotional: PBR reflectance depends on IOR and roughness, neither of which changes with emotion, and the tear film is mirror-smooth. The missing physics is thin film interference (same effect as soap bubbles and oil on water): the reflection gets stronger or weaker depending on film thickness [38-41].
11. Thin film thickness has been measured in ophthalmology since the 1980s by inverse rendering of reflectance. Without thin film interference you cannot get the correct regular reflectance of 2.5 to 5.5%, and you cannot model emotion through the eyes (4%+ reflectance) [41-42].
12. Rule: incomplete physical models used rigidly are dangerous. Either model every nuance or do not treat the model as truth [42].
13. Validate BRDF work against real measured data. The target is not "more correct PBR" but "closer to a photograph". The ultimate reference is OLAT (one light at a time) capture: a subject under many light and view directions [43].
14. Implicit versus explicit BRDF behavior. Implicit means assume the model is right and feed IOR and roughness, with everything else driven by physics (for example retroreflection or Fresnel derived from roughness). Explicit means admit the models are incomplete and expose controls that drive visual features directly. They chose explicit [44].

## Part 1. OLAT tool

15. Three bottlenecks in building digital doubles: iterating materials quickly (covered in their GDC talk), the time to switch lights and cameras to validate, and the fact that reference OLAT images are linear while the engine tonemaps, so side-by-side in other software is not apples to apples [45-46].
16. Solution: an in-engine OLAT Tool with hotkey switching of light and camera and linear-correct visualization against the reference. It was described as a key tool for learning photorealism [47-48].
17. Photorealism is a problem of scales, from material-level properties to lighting-level detail, and all scales matter [49].

## Part 2. The Callisto BRDF

18. Motivating failure: teeth match under frontal light but go too dark when lit from the side. No texture value can fix this; it is a BRDF limitation [50-51].
19. Goal: material consistency over light incidence and view angles [51].
20. BRDF slice visualization has four regions: Fresnel peak (backlighting, top), specular peak (left), retroreflection (frontal light with the surface tilted sideways), diffuse (everything else) [52-56].
21. They classified the MERL database (100 measured BRDFs: plastics, metals, cloth) into categories A to E and found common behavior: brightening or darkening along the top and right edges and a sometimes-dark top-right corner. These are Fresnel, retroreflection and smooth terminator effects, matching what they saw comparing standard shading against photographs of humans [57-60].
22. Expressiveness survey: Lambert has none. Oren-Nayar gives some retroreflection and Fresnel, driven by roughness, from a V-cavity assumption. Burley follows MERL but its features are implicit functions of roughness. Conclusion: existing models were not expressive enough to match photographic reference; adjusting roughness for specular spoils the diffuse look. For human skin, layered interactions cannot be described by simple implicit rules [61-64].
23. Design principles of the Callisto BRDF: universal and expressive, no magic numbers except for range sanity, simple arithmetic, orthogonal parameters, empirical visual match over physics, artist and machine-learning friendly, defaults reproduce Lambert + GGX, good against MERL and their digital doubles [65-67].
24. Components: diffuse = Lambert + configurable retroreflection + diffuse Fresnel + smooth terminator; specular = modified dual GGX with a specular Fresnel falloff control; the smooth terminator applies to both [68].
25. Diffuse Fresnel brightens or darkens the grazing response with a falloff control. Important for teeth and eyes: albedo matched under frontal light goes too dark under side light, and diffuse Fresnel fixes that without changing the frontal look [69-75].
26. Retroreflection has symmetric controls (intensity and falloff). It flattens or enhances the look of an object lit and viewed from the front. Important for teeth and skin; it produces the green hue visible on skin silhouettes [76-84].
27. Math structure: Fresnel term is a Schlick-like (1 - cos)^5 style term and retroreflection is its symmetric counterpart, each with falloff and tangent-falloff remaps. Parameter table [85]: Retroreflection [0,256] default 1, Retroreflection Falloff [0,1] 0.75, Retroreflection Tangent Falloff [0,1] 0.75, Diffuse Fresnel [0,256] default 1, Diffuse Fresnel Falloff [0,1] 0.75, Diffuse Fresnel Tangent Falloff [0,1] 0.75. Intensities are exposed as intensity multiplied by a tint. The math evolved slightly since their GDC 2023 talk. Compared to Burley, the main difference is explicit parameters instead of implicit behavior.
28. Smooth terminator: Lambert's terminator is too harsh versus MERL materials that transition more gradually. Added a smoothing control with a length parameter. It is masked by an alpha so it leaves the retroreflection and Fresnel regions untouched, keeping parameters orthogonal and avoiding iteration loops when setting them. Parameters: Smooth Terminator [0,1]-style intensity with tint, Smooth Terminator Length [-1,1] default 0 and a falloff default 0.5 [86-91].
29. Specular Fresnel falloff: backlit portraits need control over the specular Fresnel peak. They verified Hoffman's F82 gives the control, but a simpler change works: expose the Schlick exponent instead of hardcoding 5, and allow removing the Fresnel term completely (useful for some MERL materials). Parameter in [0,1], default 0.5 reproduces Schlick; the remap is r(x) with t(x) clamped to [0,1]. The same parameterization is used for the diffuse falloffs [91-95].
30. Combined Callisto terms show strongly at glancing angles versus Lambert + GGX [96-97].
31. Three parameter tiers: Base (5 controls: diffuse Fresnel and retroreflection intensity, their falloffs, smooth terminator) for non-specialized users; Advanced (tints, specular Fresnel falloff, dual specular roughness scale and opacity) for technical and material artists; Full (all constants exposed, including tangent falloffs and terminator length) for machine learning [98-99].
32. Authoring workflow order: base color, then roughness and metallic, then dual specular if one lobe cannot match, then retroreflection (frontal light, flatten or enhance), then side light and smooth the terminators, then lights farther around (90 to 140 degrees) to tune diffuse and specular Fresnel [100-105]. Example light angles on the slide: terminator at 90 degrees, Fresnel at 120 to 140 degrees.
33. MERL fit results, Burley versus Callisto, error percent [106-111]:
    - Black Soft Plastic 14.75 vs 3.84
    - Blue Fabric 11.63 vs 2.79
    - Yellow Phenolic 9.48 vs 7.02
    - Yellow Plastic 6.75 vs 4.08
    - Gold Metallic Paint 2 19.68 vs 6.39
    - Light Red Paint 9.22 vs 6.62
    - Red Fabric 2 6.52 vs 5.39
    - Specular Red Phenolic 7.23 vs 5.52
    - Two Layer Silver 13.26 vs 7.08
    - Yellow Matte Plastic 8.17 vs 4.63
    Callisto won every case. Full parameter sets for each fit are on slides 107-111 (for example Black Soft Plastic uses Diffuse Fresnel 175, falloff 0.7, retroreflection 3, specular fresnel falloff 0; Blue Fabric uses a warm Diffuse Fresnel tint (255,218,185)). Typical patterns: fabrics and rough plastics need strong diffuse Fresnel and retroreflection with specular Fresnel falloff near 0; glossy phenolics use dual specular with a roughness scale up to 4.25; metals use high dual specular opacity (0.9) and in one case a roughness scale of 14.
34. Prisoner suit: Callisto BRDF matched the cloth photo reference better than Lambert + GGX [112-114].
35. Dual normals, separate for specular and diffuse, were added. Important for rendering sweat accurately [115].

## Part 3. Material consistency over distance (did not ship)

36. Problem: Lighting(Downsample(inputs)) is not Downsample(Lighting(inputs)). Mipmapping normal and roughness gives wrong lighting at distance. Ground truth would be texture space lighting, which they could not afford [116-117]. Note: the slides say these ideas were developed late and did not ship.
37. Specular antialiasing from Chan 2018 (Call of Duty: WWII): relate normal length to roughness. Offline: convert roughness to a shortened normal length, combine with the normal map, downsample the mips, then convert the resulting normal length back to roughness per mip. Compared with ground truth, "micro" and "macro" variants are drastically better than naive for some angle cases (tested L = 0 to 89.5 degrees, V = 0 to 83 degrees, mips 0 to 4, 50% F0 for visibility) [118-121].
38. Diffuse antialiasing: Chan's trick applies only to specular. To do the same for diffuse you need a diffuse model that uses the same microsurface as GGX so one roughness map serves both. Heitz 2014 defines it but with no closed form. Their approach: a LUT, which would be 4D, reduced to 3D by assuming light and view are coplanar and visibility is 1 (normal maps have no visibility term) [122-124].
39. Proxima BRDF: a functional fit of the 3D LUT. Observations: the characteristic behavior occurs when the normal and view are similar and light and normal are near 90 degrees apart; elsewhere it is close to Lambert. Fit a 2D function for N = V (9 roughness slices with line fits) and fall back to Lambert as light and view go past 90 degrees. Constants on the slide include -0.55 + 0.19 style coefficients and a power of 12. For numerical stability they recommend pre-multiplying to remove the division [125-128].
40. Combining Callisto and Proxima: a LUT over all Callisto parameters is impractical, so just replace the Lambert term inside Callisto with Proxima. Compared against supersampled ground truth, it held up well for frontal and grazing views [129-131].

## Part 4. Realis (data-driven residual) (Jimenez overview [132-142], Petersen details [182-208])

41. Idea: statistical PBR approaches reality but never reaches it, and may take decades to get there. Capture reality, render with the best PBR you have, and bridge the gap with a data-driven residual [133-138].
42. Realis is a BRDF error correction term baked offline from photographs minus renders, reconstructed at runtime for a given light direction. Mostly corrects diffuse [138-142].
43. Related to polynomial texture maps, but PTMs bake the entire lighting into a basis (severe interpolation errors as light moves) while Realis projects only the error, so interpolation errors are much harder to notice [139].
44. Dimensionality reduction: diffuse only, view independent, demodulated and invariant to most lighting parameters, leaving just the incident light direction [187].
45. Error stored as luminance, not RGB. Full RGB was closest but costs memory and bandwidth; luminance was a close match with one third the memory [188].
46. The light vector is represented by windowed spherical Gaussians tied in model space (skinning space). Distribution is non-uniform because of sparse input data and a limited Gaussian count, so the optimal distribution is computed offline. For the protagonist only a hemispherical distribution on the face was needed [189-190].
47. Baking: error = reference minus render. Render in linear space before tonemapping, calibrate and linearize reference. Unwrap to texture space, invert the lighting function, and project into coefficients [191-194].
48. Data is always imperfect: camera noise, foreign objects, geometric mismatch, shadow mismatch, specular removal, texture-space stretching [195-196].
49. Fixes: weight out projection with large gradients (view vector compared against UV gradient, with gradient strength and fade speed parameters) [197]. Shadow mismatch: detect shadow borders by averaging neighbors into two averages (shadowed and lit) with a weighted threshold; a dot product gives the likelihood of a shadow contour. Find both the reference contour and the render contour; the gap between them is the mismatched area. For each texel do a spherical line trace and reduce coefficient weight if it lies between contours, more the larger the contour distance [198-200].
50. Runtime: the model coefficients are exported during G-buffer rendering (no bindless support at the time). Heavily compressed with an offline precision multiplier and temporal jittering, with the dataset reduced via a per-texel dominant light source mask [202].
51. Runtime data lies: gameplay changes albedo and normals (dirt, grime, blood). They export a thin version of the bake-time material properties, compare to runtime properties, and fade out Realis where they differ greatly [203-204].
52. Application: for each light, project the incident vector into model space, evaluate the Gaussians from the dominant mask, and re-apply the lighting function. It is the functional inverse of the baking pipeline [206-207].
53. Limits and future: main drawback is geometric and texture alignment (their character pipeline gave near perfect alignment); bindless would cut memory and bandwidth; it is a multi-dimensional feature fit so machine learning is the likely future [208].

## Part 5. Lighting consistency (Jimenez [143-161])

54. All lights must behave the same way or the material work is lost [144].
55. Foundation is UE stationary lights (most accurate). UE4 supported only 4 overlapping stationary lights; they extended that to 16 [145].
56. Made every light "full class": shadows enabled, no aggressive attenuation, and identical across raytracing, volumetrics and transparents including IES profiles and material functions [146].
57. Robust foundation = raytraced shadows and reflections [147].
58. Shadows are the most salient feature of an image, especially in horror. Light leaks are the most dangerous enemy of photorealism; shadow mapping is not consistent. Raytracing is robust, can approach raster cost with caching, scales better with screen size, and allows many shadowed lights when they are small [148-150].
59. Raytraced reflections avoid the screen-space and probe artifacts of leaking and over-occlusion, and ground objects regardless of screen-space complexity [151].
60. Glossy refractions, screen space [152]: they trace in screen space for performance, with rays following a GGX distribution. Compared against Arnold. Pure ground truth in screen space has inherent issues so workarounds were added. Used for visors.
61. Refraction inside materials such as creature tissue uses a texture space approach: ray march in texture space against an inner-layer heightmap, similar to parallax occlusion mapping, with flipbook and reveal mask animation [153-154].
62. Corridor problem: a linear game of long corridors has heavy light overlap, and UE4's occlusion queries use the light's full shape, so a light bleeds through walls into corridors it does not affect [156-158].
63. Hull-based light culling: fit a set of bounding planes ("light hulls") from point clouds around the shadow edge during the light bake. Used for CPU light rejection, deferred light bounds and forward grid tile culling. Saved about 2 ms. Up to 45 shadowed lights in one scene, most raytraced [159-160].
64. Tonemapping: the Gran Turismo tonemapper, chosen because its linear section helps convey natural results. Calibrate with the Zone System: overlay grayscale zones each one stop apart; the original system has 11 zones, they used 17 for HDR; color coding shows toe, linear and shoulder segments [161].

## Part 6. Observing the world (Jimenez [162-179])

65. A simple light rig was built to capture photographic references (prisoner suit, glass) [163-164].
66. Bloom was fitted to photographic reference of a bright light source (reference with radial blur versus render) [165].
67. A digital double of the Ebro river in Zaragoza was made to learn water behavior, applied to the pipe slide sequence [166].
68. Blood: digital double of blood drops compared to photographs. Color depends on oxygenation, source, and time exposed. They captured drops for 3 hours at different thicknesses and built a realistic blood color palette plus a material function returning color from opacity and time [167-169].
69. Blood motion is captured via traditional VFX with artificial blood and a viscous almond paste, rather than simulation [170].
70. Forensics classify spatter as low speed (gravity), medium speed (melee impacts) and high speed (gunshots). Patterns also depend on distance and impact angle, citing Attinger et al. 2019, a gunshot backspatter dataset (Data in Brief vol 22). They recreated the patterns with artificial blood [171-174].
71. Runtime spatters are procedural from decal types: core, border, outer border, mist, and drips, varied by speed, range and surface (wall or floor), so each spatter in the game differs [175-178].

## Part 7. Implementation (Petersen [180-331])

72. Development methodology: treat Unreal as a stock implementation: take what you need, improve what you can, replace what you must. Years of unlisted optimizations exist [181].

### Raytraced shadows [209-267]

73. Pure raytracing, no hybrid rasterization. Typically many lights around the player (up to 16 large lights at once on screen) and potentially hundreds overall. UE's ray tracing was designed for different content, so they wrote their own implementation, reusing its material hit shader generation and shader record setup [211-213].
74. Six pillars: caching, variable rate tracing, culling, specialization, denoising, transmission [214].
75. Caching: with raster, shadow caching is easy because of a shared light space; with raytracing there is no shared sampling space. Stationary lights solve this: the baked static-on-static shadows in lightmap space are the de facto cache, and only dynamic interactions are traced at runtime. They avoid fully static lights. Future idea: use the offline cache as a starting point and refine at runtime based on sampling needs [215-221].
76. Dynamic-on-static contribution is limited by projecting the moving caster's AABB from the light into a conservative frustum; trace only inside it. This parallels Fatnassi's 2023 "Shadow Techniques from Final Fantasy XVI". It is a retrofit of UE4's shadow projection masks [222-223].
77. Two TLASes: static + dynamic, and dynamic only. Dynamic receivers trace the combined TLAS; static receivers only need the dynamic-only TLAS because static interactions are precomputed [224].
78. Variable rate tracing at 1:4, 1:2 and 1:1. Most shadowing can be one sample per quad, concentrating samples on silhouettes [227-228].
79. Rate selection: you cannot know the perfect rate before tracing, so compute the variance of shadow occlusion after tracing, derive rates from thresholds, and feed them to the next frame (latency is hard to see and the denoiser covers it) [229].
80. Perceptual weighting: shadow variance is not perceived variance (a 1000 nit light vs a 50 nit light; auto exposure and bloom matter). After lighting (perfect information, one frame of latency) the rate is weighted by tile luminance relative to average scene luminance. Alternative: work in absolute tonemapped space. Extended across all lights [230-233].
81. Result: often over half of the samples reduce to one per quad, depending heavily on the scene and lighting [234].
82. They did not ship a traditional scheduler (per-rate work queues with pixel offsets plus indirect args) because of early limitations; they used a "tile packing" mechanism instead (not detailed) [235].
83. Gaps from variable rates are patched with bilateral upsampling in two phases (sample positions before tracing, reconstruction after). Quarter rate: trace the top-left, reconstruct by averaging with neighbors. Half rate on borders: trace the two most significant samples (min and max Z) and copy to nearest neighbors. Half rate on flat areas: trace diagonally and average both samples. Warning: tile culling reshapes lanes within a wave, so be careful reading neighbors [236-237].
84. Culling uses tiles: each light gets a conservative CPU-estimated set of tiles and outputs a job queue plus indirect args; oversampling is limited to tile dimensions [240].
85. Depth bounds culling: reduce min and max depth per tile and reject lights that do not intersect [241-242].
86. Static cache culling: reject a tile when the offline cache says it is fully statically shadowed, because there is no possible dynamic contribution [243-245].
87. Cone culling: summarize two cones per tile (surface normals and surface-to-light directions) using wave intrinsics, and reject the tile if the cones prove N.L has no effect [246-247].
88. Reuse the forward renderer's 3D light grid (depth slices) to collapse lights within the depth bounds without redoing shape tests. One workgroup per tile in the initial design, with an LDS mask to avoid pushing the same light twice [248-249].
89. Tile size must exactly equal the raygen workgroup size (8x4) so an indirect dispatch matches the intended dispatch (a plain 32x32 aligned to workgroup size was not enough and gave an irregular cache signature). But classification became 10x slower because small workgroups under-use resources, so they process 32 tiles per workgroup, with a bit per tile for the light set mask stored externally [250].
90. Wave intrinsics across tiles: reorder scheduling so waves align to tile regions. Use wave64, not wave32 (resource requirements make wave32 sub-par), process two tiles per wave, and use upper and lower lane bounds to mask out the other tile [251].
91. Specialization: divergence and extra resource pressure in a raytracing shader cause global slowdowns, so split the pipeline by feature with compile-time feature selection. Shading model masks: default, subsurface scattering, eye, and a branching fallback when a tile mixes models. Example: the iris needs diffuse and specular shadowing decoupled, which costs two traces per sample but sells the eye [254-257].
92. Multiple queues per light would explode memory, so a pre-sum pass computes sub-queue offsets before the main classifier, letting the queues merge compactly even with atomic tile allocation [258-259].
93. Denoising: the stock denoiser works over the light scissor. Instead denoise only shadowing tiles, skipping culled tiles, and only tiles with sufficient variable rate (high variance). This was the greatest speedup and what made denoising affordable, plus platform-specific bandwidth and memory work [261].
94. Transmission on all lights is based on Jimenez 2010 "Real-Time Realistic Skin Translucency". UE4's built-in version casts up to 3 extra rays (plus an optional early reject). They reverse the shadow ray (light to surface) for free thickness estimation: one ray that no longer stops at the first hit [263-264].
95. Combined result: roughly 10x per pass. Example scene saves 36.74 ms, removes 85% of tracing cost and 89% of denoising cost (excluding general ray optimizations) [266].
96. Raster versus raytraced shadow cost (ms), culling and denoising included, raytraced adds on top [267]:
    - Europa 01: 5.35 raster, +2.5 RT
    - Tunnels 100: 3.57, +4.3
    - Minetown 01: 5.2, +2.3
    - Minetown 150: 2.8, +1.7
    - Escape 02: 3.2, +1.8
    - Escape 37: 7.5, +0 (equal)
    - Escape 71: 9.19, +1.17
    - Habitat 33: 5.7, +4.5
    - Habitat 21: 6.58, +2.9
    - Habitat 10: 2, +2.4
    (The slide's percentage column is relative cost; the notes say raster is ahead in some cases but RT is close, and visually exceeds raster.)

### Raytraced reflections [268-319]

97. Goal: reflections on opaques, translucents and shadows, approaching ground truth. Topics: shadows, translucents, lighting reuse, variable tracing, inline trace, holograms, temporal, ray distances [268-270].
98. Shadows in reflections are visually significant but traced shadows are very expensive. Reuse the offline cache: stationary-on-static shadows from Lightmass cost one texture fetch. Movable geometry and movable lights (e.g. the flashlight) are still traced at runtime. Known gap: movable on static geometry shadows are missing, which is hard to notice but a "sin" [271-276].
99. Pipelining: based on UE's experimental deferred reflection path: forced-opaque gather (exports hit info), sort hits by material ID, then shade with embedded shadow traces (re-traversing if any-hit is needed). They moved all remaining shadow traces to a separate packed pass (static from cache, dynamic by runtime trace), reading it back in shading. This lowers resource usage and helps async compute [277-278].
100. Translucent reflections (mirrors, glass, eyes): stock UE casts extra rays per layer, which is too expensive. They added a T-buffer: a depth-tested geometry pass of the front-most translucent surface. The reflection primary ray uses the T-buffer instead of the G-buffer when it is closer. The top layer is raytraced; deeper layers fall back to probe lookups and SSR depending on which has better visibility [280-284].
101. Layer heuristics (roughness assumptions, EnvBRDF-based reflectivity estimates) were error-prone and always broke somewhere. "Simple is best." Future: checkerboarding across layers and temporal convergence on the layer with the most contribution [285-286].
102. Reuse lighting: always raytrace, never screen-space trace, but reuse previous-frame lit screen color for performance and because the BVH misses some content (large Niagara particle systems with millions of tiny particles, certain decals) [289-291].
103. Parity with raster requires matching light functions, IES profiles and volumetrics. Reused screen data includes volumetrics but raytraced hits do not, so sample the froxel volumetric grid for hits where the screen sample is unavailable and blend inversely with screen weight to avoid double contribution. Future: pre-integrated volumetric spaces rather than expensive integration [292-296].
104. Variable rate for reflections extends the Battlefield V (GDC 2019) approach, which used roughness. They believe roughness is not indicative of final contribution, so rates come from reflection variance per tile, with a frame delay and careful reprojection. Perceptual weighting also applies: compare linear reflection results against linear final scene values and compute variance on weighted samples [298-303].
105. Variable resolution: reflection resolution scales between 50% and 25% on each axis from a headroom factor (scaled exponential of frame dynamic resolution): 25% at 2K 16:9 target and 50% at native 4K. Chosen because it is very scalable; quite a few sequences hit 4K [304-305].
106. Any-hit cost: assets rely too heavily on opacity mapping (too late to change). Fix is "inline summarization": an extra inline lightweight trace in the first gather pass records the final opaque hit position and the real any-hit ranges. A separate "Gather Complex" pass only runs if a real any-hit exists before the final opaque, and exports the final hit. The shading pass then does no traversal, only evaluates hit position and material (vendor extensions), and sorting now operates on real final hits. Costs a few hundred microseconds. Up to 3 ms saved in worst cases; PC drivers gave uneven gains and losses so it was disabled on PC [308-312].
107. Holograms: Gather Complex accumulates emissive radiance along the ray while evaluating any-hits. Kept unordered for speed, which can miss occlusion; inline summarization and content rarity made that acceptable [314-315].
108. Temporal: interleaved gradient noise (IGN, Jimenez 2014) across the board; on disocclusion apply a hierarchical blur whose size is tied to the time since disocclusion, reducing over time, with a moving average rather than exponential accumulation [317].
109. Ray distances: precompute max ray distance per area for thin corridors and constrained spaces (effective limits are a small fraction of world bounds). Evaluated offline volumetric cache, offline surfel cache, and a runtime progressive spatial hash cache; they did not use lightmap space because of memory. Not shipped. His bet: the progressive runtime cache [319].

### Platform optimizations [320-331]

110. Material cat and mouse: raytracing pipelines require per-stage GPR allocation for the worst-case material, so a single heavy material (e.g. video decoding or texture space tracing in a closest hit) can tank everything. Fix: determine the known-good VGPR range for materials and cap VGPRs, letting outliers spill to scratch memory. Good content gets faster, expensive content slower; fix expensive materials separately, since hit shaders should be simple [322-323].
111. Split hit shader tables by use (shadows versus reflections) so shadow pipelines do not inherit reflection VGPR needs. Finer splits are bottlenecked by CPU material parameter binding costs (UE5 mentioned) [324].
112. Long RT tails (unlucky traversal, any-hit such as hair) are hidden by overlapped execution: batch many ray calls together so 32 lights' tails become one effective tail. Measured 2.47 ms to 1.66 ms (-32%) on XSX. Costs memory, and overlapped passes must share the same VGPR scratch layout [325-326].
113. Reflection tails cannot overlap because of inter-stage dependencies, so run reflections on async compute with the wave limit reduced to 50%; kickoff and sync are half a frame apart. Gain about 2.35 to 2.5 ms (32.5 ms to 30.15 ms on XSX). Carefully manage async compute versus graphics scratch space [327-328].
114. Acceleration structure builds (skinned BLAS builds, TLAS builds that underuse hardware) go to async compute: 0.9 ms saved on XSX (the slide marks an 80% reduction of that cost) and about 0.5 ms on PC [329-330].
115. Thousands of smaller optimizations combined had a large effect, plus months of GPU crash hunting with vendor help (Sony, AMD, Microsoft) [331].

## Part 8. Key takeaways [332-334]

116. Algorithmic and platform optimizations do not get you all the way. What landed them in budget was optimizing for their own content and exploiting any generalization the content allows (linear corridors, stationary lighting, character-focused shading).
117. Raytracing is not far from matching raster performance, and Petersen believes it will eventually exceed it.
118. Summary: ground truth through raytracing and accurate caching, consistency through the Callisto BRDF, Realis and shadowed lights with real reflections, and observation of the real world as the final reference.

## Not covered by slide text (flagged by the presenters)

- Hidden slides on holograms and ray distances, the tile packing mechanism, Realis bake details, and the remaining MERL fits are referenced as "offline slides". Only slides in the public PDF were catalogued.
- Exact formulas for the Callisto BRDF terms, the smooth terminator, specular Fresnel remap, Proxima fit and the Realis weights are images in the PDF; read the slides directly (85, 90, 95, 128, 131, 197-198) when implementing.
