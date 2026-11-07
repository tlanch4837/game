# CS2 look pass (city game)

## 1. What defines the CS2 look

Measured from the Steam store screenshots of Cities: Skylines II (1920x1080 JPEGs, sRGB 0–255, sampled with a canvas in the browser).
References used:

- **Aerial / city view**: the coastal city with an airport (`ss_ee0b920f…`).
- **Mid zoom / street**: the autumn suburb with a white house (`ss_c5bc1c21…`).
- **Mid zoom, top-down**: the highway interchange with the road tool open (`ss_fcc6a62a…`).
- **Night**: the downtown crossing (`ss_b2ce0025…`). The store has no dusk shot, so dusk below is a description, not a measurement.

| Trait | CS2 (measured) | What it means for us |
|---|---|---|
| Tone curve | Aerial: luminance percentiles 1/5/25/50/75/95/99 = 17/21/44/80/108/148/192. Suburb: 6/13/45/77/123/194/238. | Low key: the median sits near 78–80 of 255. Blacks are deep (dark roofs 17, hedges 9–21). Highlights roll off softly: a sunlit white wall is 227, not clipped. |
| Saturation | Mean HSV saturation 0.31 (aerial), 0.45 (suburb). | Moderate close up, low from the air. Greens are olive and dark: forest 26–43 / 33–52 / 30–34, lit lawn 61/70/14. No lime. |
| White balance | Sunlit asphalt 129/112/92, sunlit sidewalk 135/118/90, sunlit mountain face 173/146/130. Shadowed white wall 61/63/67. | Warm sun, cool neutral shadows. The split is the main colour signature. |
| Sun to shadow | White wall 227 lit against 61–67 shaded: about 12–15:1 in linear light. | Strong key light, shadows dark and readable, not milky. |
| Aerial perspective | Near forest 43/52/34, then about 1–2 km 93/95/83, then far hills 119/114/110 and mountains 132/123/128. The suburb's far city is 145/135/119. | Strong haze. It lifts and desaturates toward a warm, slightly pink grey, not blue. About half-way to the haze colour at 1.5–2.5 km near the ground; a thin veil over the whole city from the air. |
| Haze / horizon colour | Horizon 108/107/105, far water 138/137/135, sky at the top 146/152/161. | The fully hazed colour equals the horizon sky, so terrain melts into the sky without a line. |
| Sky | Pale, low-saturation blue-grey. | A desaturated zenith, a bright warm-grey horizon. |
| Shadows | Long and soft-edged at the far end, sharp near the caster. Cool tint. | We already have PCSS; keep the penumbra and make shadows cool. |
| Ambient occlusion | Visible in creases and under trees, never as a dark halo over open ground. | AO on ambient light only, never on the sun. |
| Water | Deep teal, dark: 4–14 / 28–38 / 36–49. | Water is its own shader; out of scope here except that haze covers it. |
| Night | Deep blue-black, warm sodium street lights, saturated signs. | Not part of this pass. |
| Dusk (unmeasured) | Orange sun, purple-grey haze, long shadows. | Our sun colour and sky already move toward orange at low sun; the haze takes the sky's horizon colour, so it follows. |

The user's own references (`shots/cs2_ref/`) took precedence over the store shots once they arrived. They moved the haze from warm grey
to pale blue-grey and the sky to a clean blue.

| Shot | Measured |
|---|---|
| Aerial valley | Percentiles 16/94/202; saturation 0.23; sky 160/187/199 (blue, not grey); far city 129/130/111 |
| Golden hour | Lit facade 167/150/95; shaded facade 37/36/28; street in shade 14/17/13; haze 149/148/135 |
| Bridge | Upper sky 166/167/137 around the sun, lower sky 124/168/200; road lit 93/93/88, shaded 52/57/62 (cool) |
| Marina | Sea 15/54/69 to 43/76/110 |

## 2. What changed

Everything sits behind the city map (`FrameView::cityAir`: the city game turns the physical sky off in `city_game.inl`). Characters,
the editor and the desert scenes keep their look. `TBD_LOOK=0` brings the old city look back for comparisons.

| Where | Change |
|---|---|
| `src/shaders/city_air.hlsli` (new) | The city's air, shared by post and water. A pale sky gradient from the CPU's zenith and horizon radiance, with a sun glow. Exponential height fog integrated exactly along the ray: 6e-4 per metre at sea level, 250 m scale height, clear for the first 250 m. Fully fogged ground takes the horizon colour: pale blue-grey by day, keeping its orange at dusk. Sky gain falls off at low sun so dusk doesn't burn white. |
| `post.hlsl` `CompositePS` | For `misc.z = 0.5`: `CityComposite`. The sky, the haze, and AO on the ambient share only. Before, the city map took the old sky-off path: AO multiplied the whole colour, sun included, and there was no sky or haze at all. |
| `post.hlsl` `TonemapPS` | `CityGrade`, folded into the tone map: no LUT, no pass. Greens pushed toward olive, saturation ×1.1, a stop lower key (×0.52), log-space midtone contrast (pow 0.28 on luminance, so hue holds), cool blue-grey shade and warm light. Bloom skipped below Medium. |
| `water.hlsl` | On the city map, the land's closed-form haze instead of a 4-step marched air per vertex (cheaper, and the sea now fades like the land). Clear deep-sea absorption and blue-green scatter instead of river silt. |
| `lighting.hlsli` | The PCSS-off early-out moves ahead of the 5-load blocker search, so the one-tap path really costs one tap. |
| `renderer.h`, both renderers | `FrameView::cityAir` sets `misc.z = 0.5`; `FrameView::postTier` goes in `motion.w`. AO skipped at Low on the city map; bloom skipped at Low and Low/Med. Fixed: `gpuPasses_[8]` was written with 10 entries, overrunning into the fence/present timers. It is now `[10]`. |
| `main.cpp` | `LookOn()` (`TBD_LOOK`), `LookTier()` (from `TBD_CHUNK_PRESET`, the same knob as Track R's governor), `cityAir` on the city map, PCSS off at Low. |
| `city_zones.inl` | With the zoning tool closed, the zone overlay is lit (normal up), so sun, building shadows and haze fall on it, in a softer tint (55% grey, ×0.62). With the tool open it is unchanged: unlit and full colour. |

Distant buildings: every change is in post (composite, grade) or in `LitPbr`'s inputs. The chunk volumes (`ChunkVolPS`) and the real
meshes go through the same composite and tone map, so their colour stays matched.

### Round 2 (approved by the user): realism

The user found round 1 lacked realism. Against their golden-hour shot the gaps were:

- no cast shadows past the shadow map's 60–120 m, and none at all from the baked far buildings;
- sunlit and shaded walls nearly the same brightness (CS2: about 15:1);
- a near-noon sun that hid every shadow under its house;
- zone paint covering the ground.

| Where | Change |
|---|---|
| `post.hlsl` `CitySunShadow` | Screen-space sun shadows in the city composite. The depth buffer is marched toward the sun, 3–150 m long (longer as the sun sinks), and only the sunlit share of each pixel darkens, so the shadow map's own shadows never double up. Steps: Low 8, Low/Med and Medium 12, High and Ultra 16. Near meshes and far volumes get the same treatment. |
| `post.hlsl` `AoPS` | AO radius up to 14 m on the city map (5 m elsewhere), so buildings darken the street at their foot from far off. |
| `main.cpp` | City map ambient fill ×0.55 (`AmbientFill`, through `LitPbr` for meshes and volumes alike): a stronger key light. |
| `city_game.inl` | New city maps start at 15:36 with a 30° sun lean (the editor keeps 13:42 / 22.4°). |
| `city_zones.inl` | Zone colours draw only while the zoning tool is open, as in CS2 (round 1's lit soft paint is gone). |
| `city_air.hlsli` | Haze eased to 4.8e-4 per metre. |

`TBD_LOOK=0` still restores all of it: the old sun, the always-on zone paint, no shadows march, the old fill.

## 3. Cheap path per preset

| Preset | Sky / haze | AO | Bloom | Sun shadow filter | Screen-space sun shadow | Grade |
|---|---|---|---|---|---|---|
| Low (iPhone 15, 60%) | analytic | **off** (pass skipped, composite reads none) | **off** | **1 hardware tap** (PCSS off) | 8 steps | ALU in the tone map |
| Low/Med (iPhone 15, 75%) | analytic | half-res HBAO, 2 slices × 3 steps | **off** | PCSS | 12 steps | same |
| Medium | analytic | as above | on | PCSS | 12 steps | same |
| High / Ultra | analytic | as above | on | PCSS (+ tight map) | 16 steps | same |

Round 2's numbers are on the comparison page (`shots/look/timings.html`). The composite goes from 0.04 to 0.33 ms at 1615x812
(16 steps) and from 0.02 to 0.11 ms at 975x483 on Vulkan; at Low (8 steps) it is 0.20 ms at 1615x812. It is the one new cost of
note. On an iPhone 15 at Low it may reach 1–2 ms, so it is the first thing to measure on a device. The fallback is a half-resolution
shadow mask upsampled with the AO's depth-aware filter.

- No new render targets, passes, textures or samplers at any preset. Clouds stay off on the city map at every preset; the old path's
  cloud march was never on there.
- Per pixel, the city composite is about 30 ALU plus the 4 AO loads it already had, and the grade about 25 ALU. The water per vertex
  went from a 4-step march with 3 sun samples each to one exp.

## 4. GPU time (PC, D3D12 and Vulkan)

Fixed mid view (600 m, 10k-lot stress city), window 975x483. Same exe; old look = `TBD_LOOK=0`. Best of two runs. Two runs disturbed by
other GPU load are left out; all logs are in `shots/look/ab_*.log`.

| API | Preset | Look | GPU frame | Shadow | Scene | AO | Composite | TAA + exposure + bloom | Tone map |
|---|---|---|---|---|---|---|---|---|---|
| D3D12 | PC | old | 6.40 | 0.57 | 5.68 | 0.02 | 0.01 | 0.06 | 0.03 |
| D3D12 | PC | new | 6.42 | 0.56 | 5.70 | 0.03 | 0.02 | 0.06 | 0.03 |
| D3D12 | Low | old | 3.45 | 0.56 | 2.77 | 0.02 | 0.01 | 0.06 | 0.02 |
| D3D12 | Low | new | 3.51 | 0.57 | 2.86 | **0.00** | 0.01 | **0.04** | 0.02 |
| Vulkan | PC | old | 7.00 | 0.75 | 6.08 | 0.02 | 0.02 | 0.07 | 0.02 |
| Vulkan | PC | new | 6.81 | 0.74 | 5.87 | 0.02 | 0.02 | 0.07 | 0.02 |
| Vulkan | Low | old | 3.94 | 0.57 | 3.20 | 0.02 | 0.02 | 0.06 | 0.02 |
| Vulkan | Low | new | 3.82 | 0.57 | 3.05 | **0.00** | 0.01 | 0.06 | 0.02 |

Reading the numbers:

- No regression on either API. Post is about 0.1 ms of a 3.5–7 ms frame on this PC; the frame is the scene and shadow passes.
- Low's savings (AO, bloom, single-tap shadow) are small here, because fill rate isn't the PC's limit.
  - On a tile GPU at 60% of 2556x1179, AO (17 depth taps at half res), bloom (20 taps at quarter res) and 15-tap PCSS are bandwidth.
  - They are the first things worth dropping. Their size there needs a device.
- The single-tap shadow doesn't show in Low's scene time (2.86 vs 2.77 ms, noise): at Low most of the city draws as chunk volumes, which use their own cheap shadow.
- The three-view stress runs (`shots/look/{old,after}_{pc,low}_{d3d12,vulkan}.log`, first batch) agree. The engine window grows from 975x483 to 1615x812 partway through some runs, so only same-size rows compare.

## 5. Needs a device

1. The ALU cost of `CityComposite` + `CityGrade` at 60% of iPhone 15 resolution: expected well under 0.2 ms, unmeasured.
2. AO and bloom savings at Low and Low/Med on A16: the passes the PC can't price.
3. PCSS against one tap on real meshes at Low/Med: whether Low/Med should drop to one tap too.
4. Banding in the sky gradient at 8 bits after MetalFX (the PC's TAA + Catmull-Rom hides it; MetalFX may not dither).
5. Whether half-res HBAO at 75% still reads at city zoom on a phone screen, or should also go at Low/Med.
6. Sustained clocks with the grade on (ALU only; no change expected).

## 6. Not in this pass (follow-ups)

- **Emissive windows at dusk** (the user's priority 4). `RoomGlassPS` lights 10% of rooms at all hours.
  - A dusk ramp is one line there, but the Track R bakes store no emissive, so far buildings would stay dark while near ones glow.
  - That breaks the "same colour near and far" rule.
  - It needs a window-glow channel in the bake (the normal atlas's alpha holds AO) and a time-of-day term in `ChunkVolPS`.
- **Lens flare / sun streaks** (the bridge shot). A screen-space flare needs a pass; Medium and up could afford a quarter-res one.
- **Saturation.** The mid view's mean saturation is 0.16 against CS2's 0.23–0.33: one constant in `CityGrade` (×1.1 now).
- **Terrain albedo.** Our grass is a pale tan-olive, CS2's ground a darker forest. That is the terrain material, not the grade.
- **Water reflections** still sample the marched sky table (`gSky`), not `CitySkyOf`. Close in colour, not identical.

## 7. Metal port

The shader interface is unchanged. The side-session notes, `cs2_look_metal_port.md`, list what the port needs:

- `misc.z` and `motion.w` in `PostParams`;
- two `PostPass` guards in `renderer_metal.mm`;
- the `FrameView` fields;
- `LookTier()` in `game_core.inl`, reading the device preset instead of `GetEnvironmentVariableA`.

All 40 shaders convert with ios-metal's `tools/msl.ps1`, with identical slots.
