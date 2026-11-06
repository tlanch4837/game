# Skin and eye realism plan

Target: the close-up reference (young East Asian man, studio side light, ~8 cm of face across the frame).
Scope: the base head (`testproject/WesternGame/Assets/characters/asian/body_head.glb`) and every head made from it.
Every renderer change lands on both D3D12 and Vulkan.

## Where we are (measured 2026-11-05)

| Thing | Now | What the reference shows |
|---|---|---|
| Albedo (glb image 0, 4096²) | Face island is one tan, std dev of about 9/17/6 in RGB, a few faint pink dabs | Warm and cool zones, redness at the nose and eyes, darker under the eyes, blue-grey stubble shadow, moles, mottling |
| Surface map (`body_head.skin_surface.png`, 1024²) | R roughness is uniform noise with no regions; G cavity is nearly white; B and A are flat | A shiny T-zone (nose, brow, cheekbones), duller cheeks and jaw, pores that break up the highlight |
| Pores | Tiled atlas ×32, darken albedo by 18%, normal at half strength | Pores read through the highlight: round and open on the nose, stretched and angled down on the cheeks, fine criss-cross under the eyes |
| Screen-space SSS (`post.hlsl` ScatterSkin) | Blurs the whole lit colour, highlight included, then restores 75% of the luma | Sharp surface highlight over a soft red diffuse |
| Eyes (`LitEyeball`) | Good cornea refraction and caustic. Lid shadow is a UV guess (`lid` in EyePS). Sclera is plain. | Lids shade the eyeball, a wet tear line on the lower lid, a pink wet caruncle with its own glint, a greyish sclera with veins toward the corners, a dark limbal ring |
| Caruncle | A painted pink card in the lash card file, lit like hair | A wet, rounded, glossy mound with a highlight |

## 1. Colour: skin is not one colour (biggest win)

Skin colour comes from two pigments under a thin surface: **melanin** (brown, even and slow-changing) and **haemoglobin** (red, patchy, following blood supply). Paint those two, not the final colour, then let a bake combine them. That keeps every head natural and adjustable (age, sunburn, flush, ethnicity) without repainting. The model follows Jimenez et al. 2010, *A Practical Appearance Model for Dynamic Facial Color*.

### 1a. Zones by blood and pigment (the classic three-band face, plus details)

- **Forehead and temples:** most yellow/olive, slightly more melanin, least blood.
- **Middle band (cheeks, nose, ears):** reddest. Strongest blood at the nostril wings (alar creases), nose tip, apples of the cheeks and earlobes.
- **Lower band (upper lip, chin, jaw):** cooler and blue-grey from beard follicles under the skin (strong on this face even when clean-shaven), plus a little green.
- **Eyes:** upper lids pinker and thinner, with veins faintly showing. Under the eyes, a darker band in purple-brown. Lower lid rims and inner corners red.
- **Lips:** separate map, already in hand.
- **Neck:** a little paler and yellower than the face, darker in the folds.
- **Creases (nasolabial fold, under the nostrils, inner brow):** a touch darker and redder.

### 1b. Frequencies (what makes it look photographed, not painted)

1. Low (zones above): smooth, in face space.
2. Mid (1–10 mm): blotchy redness and melanin mottling. Two noise fields, one per pigment, not one colour noise.
3. High (under 1 mm): per-pore colour. Pore openings slightly darker and browner, with a faint red rim. This ties the colour to the pore normal tile, so it has to use the same UVs and tile.
4. Features: a few moles, small sun spots on the cheekbones and nose bridge, tiny capillaries on the nostril wings, faint stubble dots on the upper lip and jaw.

### 1c. How we build it

- **New tool `gameenginetools/skin_tex/skin_colour_bake.py`.** It reads the head mesh, places zones by **3D distance to landmarks** (nose tip, alar creases, eye corners, mouth corners, chin, ears), and bakes into the face UVs. That means no hand-painted masks, and it works for any head made from the base.
  - Outputs: `melanin.png`, `blood.png`, `beard_shadow.png` (each L8, 2048²) and the final 4096² albedo.
  - Combine in linear space: `albedo = base_epidermis * melanin_tint(m) * blood_tint(h) * beard(b)`.
- **Character Creator sliders** drive the bake: melanin amount, blood amount, beard shadow, age (more spots, more yellow, less blood), sun exposure.
- **Optional, later: blood at runtime.** Keep `blood.png` as a texture and scale it in the shader for flush/anger/cold/exertion. Wrinkle compression can blanch it: we already have `SkinWrinkle`, and squeezed skin goes paler.
- **ChatGPT role:** reference sheets and mole/freckle stamp sheets only. Its photos have baked-in lighting, so they never become the albedo directly.
- **Rules for the albedo:** no lighting, shadow or highlight baked in. Check it under flat light in the engine.

## 2. Pores: more accurate

1. **Region and direction.** Add a low-res (256²) *flow* map: R and G hold the stretch direction, B the stretch amount. Rotate and stretch the tiled UVs per pixel:
   - Nose: round, open, about 0.15 mm deeper.
   - Cheeks: oval, pointing down and out.
   - Forehead: fine, with faint horizontal lines.
   - Eyelids and under the eyes: almost none.
   - Upper lip and chin: follicle dots.
   Hide tile seams with two rotated samples blended by noise (stochastic tiling).
2. **Micro criss-cross.** Add a second, finer tile (×96): the diamond pattern skin makes under the eyes, on the cheeks and on the neck. Weight it by region, using `extra.G` like the pore atlas.
3. **Face-wide detail normal (4096²).** Sculpt in Blender: nasolabial folds, crow's feet, the folds under the eyes, the nose tip, lip lines. Bake it and layer the tiles on top (combine with reoriented normal mapping, not adding).
4. **Pores belong to the highlight.** Drop the albedo darkening from 18% to about 6%, which is only the colour of the pore opening (see 1b). Use the full-strength pore normal for specular and a blurred one for diffuse (diffuse light is already softened under the skin).
5. **Bring the cavity map to life.** Bake it from the same pore height and multiply it into specular occlusion, so the highlight's holes line up with the bumps.
6. **Distance.** When pores get smaller than a pixel they must not just fade out. Turn the lost normal variation into extra roughness (Toksvig / specular AA), so the face keeps the slightly broken sheen it has from across a room.
7. **Resolution.** The surface map goes from 1024² to 2048² for the head.

## 3. Shine and the skin shader

1. **Split highlight from scatter (do first).** Skin pixels write their surface highlight to its own target (or skin keeps it out of `gScene`). `ScatterSkin` blurs diffuse only, then the highlight is added back sharp. This drops the "restore 75% luma" hack.
2. **Roughness by region** (in the surface map R, baked by the same landmark tool):
   - T-zone (nose bridge and tip, inner cheeks, mid-forehead): primary lobe about 0.28, oily.
   - Cheeks, temples: about 0.42.
   - Jaw, neck: about 0.5.
   - Lips: separate.
   Keep the two lobes (oil film + skin) but let the map move both.
3. **Oil and sweat as a runtime layer.** One float per character (heat, exertion) lowers roughness and raises the oily lobe's weight in the T-zone mask, with tiny droplets as a tiled normal at high values.
4. **Diffusion colour follows melanin.** Darker skin scatters less far and less red. Scale `kScatter` and the transmission tint by the melanin amount from the bake.
5. **Peach fuzz (vellus hair).** A faint, sheen-like rim at grazing angles on the cheeks and jaw: cheap, soft, and it gives the silhouette edge that soft glow.
6. **Lookdev scene and camera preset.** A close-up with one soft key from the side, a dark background and a 70–85 mm lens equivalent. It's for judging changes against the reference and for cutscene close-ups.

## 4. Eyes: higher fidelity

1. **Eye occlusion shell.** A thin transparent mesh hugging the eyeball under the lids, darkening and blurring the eyeball near the lid edges and in the corners. It replaces the UV-guessed `lid` term in `EyePS`, which can't follow blinks or gaze. This is how real-time digital humans get the eye to sit *in* the head.
2. **Tear line (lacrimal meniscus).** A thin strip mesh along the lower lid edge, from the inner to the outer corner, generated from the lid edge like the lash strips in `lash_cards.py`. Shader: almost clear, roughness about 0.03, strong Fresnel. It gives the bright wet line on the lower lid in the reference, and it hides the seam where the lid meets the eyeball.
3. **Caruncle and plica.** Swap the pink card for a small real mesh: a rounded mound in the inner corner, plus the thin fold (plica semilunaris) between it and the eyeball. Light it with the wet mouth shader (`LitMouth`): roughness 0.15–0.2, red scatter, so it picks up its own glint. Join it to the tear line so they read as one wet surface.
4. **Waterline.** The inner rim of the lower lid gets a wet pink strip (part of the tear line mesh or a lid material region): redder, glossier, no pores.
5. **Sclera.**
   - Colour: not white. A slightly grey/blue-ish white that warms to yellow-pink toward the corners.
   - Veins: paint thin red ones in the eye texture, mostly near the corners and fading toward the iris. They also need a little normal relief so they catch light.
   - Keep the existing red lift toward the rim.
6. **Iris and limbus.**
   - Limbal ring: darken the band right at the iris edge (r 0.46–0.53); it reads strongly in the reference.
   - Iris texture: radial fibres, a collarette ring, crypts.
   - Iris normal: a fine height map for the fibres so the caustic breaks up.
   - Eye colour stays 2048²; add a separate height channel.
7. **Catchlight and reflection.** The cornea now reflects only the sky. Add a reflection of the local probe/environment, so the eye shows the room or the street. Allow a per-shot catchlight light for close-ups. Clamp the glint so TAA doesn't flicker on it.
8. **Eye moisture shared.** One "wetness" parameter for the eye group (tear line, caruncle, waterline, cornea): tired or crying raises it, a dry wind lowers it.

## Order of work

| # | Step | Kind | Why this order |
|---|---|---|---|
| 1 | Lookdev scene and camera preset | Engine | Every later step is judged here |
| 2 | Split highlight from SSS | Shader, both backends | Unblocks all shine and pore work |
| 3 | Landmark bake tool: colour (melanin, blood, beard), region roughness, region masks | Tool | Biggest visual change, and its masks feed 4 and 5 |
| 4 | Pores: flow map, criss-cross tile, full-strength specular normal, cavity bake, Toksvig | Shader + tool | |
| 5 | Shine: oil layer, melanin-driven scatter, peach fuzz | Shader | |
| 6 | Eyes: occlusion shell and tear line (generated like the lash strips) | Mesh + shader | Biggest eye gain |
| 7 | Eyes: caruncle mesh with wet shader, waterline | Mesh + shader | |
| 8 | Eyes: sclera veins, limbal ring, iris height, environment reflection | Texture + shader | |
| 9 | Face detail normal sculpt (4K) | Blender | Can run in parallel from step 3 on |

Check each step: a screenshot in the lookdev scene beside the reference, on D3D12 and Vulkan, plus one gameplay-distance shot so the pores don't shimmer or vanish.
