# Hair and beard cards: clean-room spec

How to build AAA-style hair cards with original assets. Numbers were measured from a private study of RDR2's `player_zero` meshes. They are used only as general budgets and ratios. No game asset, UV layout or texture belongs in this repo.

Status key: **Measured** = computed from the data. **Inferred** = my reading of the data, not confirmed from the game's shader. **Recommended** = my design choice for this engine.

## 1. Rules for keeping this original

Not legal advice. Get a lawyer's review before a commercial release.

- **Free to reuse:** the technique. Copyright protects specific expression, not methods or general ideas: card meshes, atlases, palette lookup, wind weights, budgets.
- **Never ship or derive from:** RDR2 meshes, textures, UV layouts, card placement, the palette, Arthur's likeness or his recognisable hairstyles.
- **No tracing or overpainting:** that includes retopologising over their meshes or sampling their colours.
- **No game textures in AI tools:** an "original version" made from one can still be a derivative, and uploading it also sends it to a third party. Use the text brief in section 7.
- **Separate risk:** copyright is not the only issue. Rockstar's licence terms may restrict extraction and reverse engineering, and the archives are encrypted, which can raise anti-circumvention issues in some countries. Keep the study copy private and ask the lawyer about this specifically.
- **Keep it separate:** the study copy stays in `C:\custom windows apps\rdr2_arthur_study`, outside this repo and all build scripts. No code or data is copied from it, including by worker AIs.
- **Keep provenance:** for each shipped hair asset, save its source files or generator seed, reference photo licences and author.
- **Clean authoring:** whoever makes the final assets should work from this spec and licensed photo reference, with the study viewer closed.

## 2. How the system works

Five ideas carry most of the look:

1. **Shingled layers.** Cards lie almost flat against the head and overlap like roof tiles. They do not stick out.
2. **A shared strand atlas.** Every style reuses one sheet of strand clumps, so new styles are mesh work and need no new textures.
3. **Colour separated from shape.** The textures store data, and a palette supplies colour. One atlas gives every hair colour and greying level.
4. **Geometry carries the silhouette.** Normal maps do almost nothing; the hair's normal map is a flat 32×32 placeholder.
5. **Per-vertex data.** Vertex colours store root-to-tip motion weight and random variation, so simple meshes can move convincingly.

## 3. Measured card geometry

All are **Measured**. Ranges span the sampled meshes, so treat them as budgets.

### Scalp hair

| Property | Main cards | Fine hairline and flyaway mesh |
|---|---|---|
| Triangles per style | 1.5k to 10k, median about 8k | about 500 to 650 |
| Cards per style | 386 to 886 | 37 to 78 |
| Triangles per card, median | 8 to 20 | 2 to 4 |
| Card length, median | 58 to 78 mm (90th percentile 85 to 195 mm) | about 24 mm |
| Card width, median | 10 to 21 mm | 9 to 10 mm |
| Length to width | 3.4 to 7.8 | about 2.7 |
| Rows along the strand | 6 to 8 | 2 |
| Vertices across each row | 2 to 3 | 3 |
| Distance from skin | 3 mm to 23 mm (median 8 to 11 mm) | 2 mm to 11 mm (median about 5 mm) |

- **Length drives budget:** the same style goes from about 5k triangles when short to about 10k when long.
- **Curvature:** cards bend. Out-of-plane spread is 7 to 10% of their length, so they are curved strips, not flat planes.
- **Small cards are common:** the 10th percentile card has only 2 triangles, so simple quads fill in between the long strips.
- **Normals:** vertex normals match the card face (0.99) and also point nearly along the nearest skin normal (0.93 to 0.97). This is the evidence for shingling. Side faces of a hairdo were measured the same way.
- **Layer order:** the first fifth of cards drawn sit closest to skin (about 5 to 7 mm). Later ones sit around 8 to 12 mm. Drawing runs inside to outside.
- **Skinning:** 1.7 to 1.9 bone influences per vertex, using 9 to 16 bones.

### Beard

| Property | Merged full beard | Moustache |
|---|---|---|
| Triangles | 1466 + 657 in two pieces | 584 |
| Cards | 396 + 97 | 54 |
| Triangles per card, median | 2 to 8 | 12 |
| Card length, median | 28 to 35 mm | 28 mm |
| Card width, median | 10 to 14 mm | 9 mm |
| Distance from skin, 5th to 95th percentile | 1.7 to 42 mm | 1.1 to 10 mm |
| Bone influences | 3.9, using 44 bones | 3.4, using 30 bones |

- **Beards follow the face:** they use about twice the influences and three times the bones of scalp hair, so they deform with the jaw and lips. Copy the skin weights when authoring.
- **Beards are built from regions:** chin, chops and moustache each have separate meshes at numbered growth stages, and they combine freely.
- **Skin carries the shadow:** the head albedo has stubble and a darkened beard and hair area painted in, with matching material changes. Cards sit on top of this and gaps read as dense hair, not bare skin. This matters as much as the cards.
- **Eyebrows:** 38 cards, 928 triangles, only 0.5 to 3.8 mm off the skin.

## 4. Atlas and UV rules

- **Strand direction (Measured):** on main cards, position along the strand follows V (correlation 0.96 to 0.99), and V rises from root to tip on 87 to 95% of cards. On hairline and brow meshes the strand follows U instead.
- **Tangents (Measured):** tangent follows U and is across the strand on main cards. Compute strand direction from the UV axis in your importer so both layouts work.
- **Strip proportions (Measured):** long strips use about 6.5% of atlas width and about 43% of its height. Full-height strips let one row of UV span the root to tip range.
- **Texel density (Measured):** about 4.5 to 13 pixels per mm on hair, 5 to 8 on beards.
- **Atlas sizes (Measured):** hair 2048×1024, beard 1024×1024, BC3 with full mips.
- **Concentrated usage (Measured):** 20 of 128 grid cells receive over 80% of a style's vertices. A few good clumps do most of the work.
- **Sharing (Measured):** 113 of 114 hair meshes use one atlas, and 268 of 290 other beard meshes use one beard atlas.
- **Atlas fill (Measured):** about 24 to 28% of pixels are solid and 17 to 20% are soft edge, so most of each sheet is empty space.
- **Variety to include (Recommended):** straight, wavy, curly and tangled clumps, in dense and sparse versions, plus thin single flyaways and a soft patch for hairlines.

## 5. Texture channels

| Map | Channel | Measured | Meaning (Inferred) |
|---|---|---|---|
| Albedo | R | Strand detail, mean 0.21 on hair | Lookup into the palette, or per-strand brightness |
| | G | Nearly flat at 0.49 | Unclear, possibly reserved or low-weight detail |
| | B | Strand variation, with a gradient block in the atlas | Second lookup or root to tip variation |
| | A | Solid clump shape | Coverage |
| Material | R | Mean 0.44 to 0.48, noisy | Unclear |
| | G | 0.55 hair, 0.68 beard, low variance | Roughness-like |
| | B | Mostly 0, bright in a few patches | Mask, possibly sheen or secondary specular |
| | A | About 0.95, dark in the gaps between strands | Ambient occlusion |
| Normal | RGB | Flat, 32×32 | Effectively unused |

**Palette (Measured):** 64×256 texture, one row per tint, and outfits store three row indices. Of the three rows Arthur uses, two are flat colours and one is a gradient along its width.

I did not decode the game's hair shader, so the exact blend is unknown. A viewer test with R as the lookup channel gives believable brown hair, but that is a match by eye and not proof.

## 6. Vertex data and materials

All meanings here are **Inferred** from correlations. The values themselves are **Measured**.

| Channel | Behaviour | Likely use |
|---|---|---|
| COLOR1 R | Rises from about 10 at root to 60 to 110 at tip on hair, and 2 to 60 on beards | Wind or motion weight |
| COLOR1 G | Random 96 to 128 within each card, uncorrelated with anything | Noise or phase offset |
| COLOR1 A | Binary, 255 below about z 0.70 on the model and 0 above | Region mask, purpose unknown |
| COLOR0 B | 0 to 160, rises with distance from skin (r about 0.5) on hair, 0 on beards | Layer depth or occlusion |
| COLOR0 R | 17 to 40, varies slowly | Unclear |
| COLOR0 G | Often constant 127 or 128 | Unused or neutral in many meshes |

**Shader parameters (Measured):**

- **Wind:** `scale` 0.03 to 0.11, `exponent` 0.03 to 0.15, `noiseratio` 0.2 to 0.3, and noise scale ranges of 0.1 to 0.15 or 0.1 to 2.
- **Other:** `porosity` 0.5, or 0.8 on some meshes. `_alphatocoveragescale` 1, which suggests alpha to coverage. `bumpiness` 1, `specularcolor` 1, `fresnel` 0 on nearly all.
- **Shader variants:** `ped_standard_hair` for main cards, `ped_standard_hair_alpha` for the fine mesh, and `ped_standard_hair_lod` for lower detail.

## 7. Making original atlases

**Built:** `gameenginetools/hair_atlas` generates these atlases into `Assets/hair/generated`. The engine loads `hair_data` / `beard_data` (albedo slot) and `*_material` (normal slot) for scalp, cap and beard; colour comes from melanin, not the baked `*_colour.png`.

**Recommended: a procedural generator.** Grow strands as noisy splines, rasterise them and write the channels directly. You get exact alpha and data channels, unlimited variety, and clean provenance from a seed. This suits a technical pipeline better than image generation.

Steps:

1. Generate 30 to 200 strands per clump from a shared root region, with per-strand curl, width taper and length variation.
2. Rasterise at 4× and downsample for soft edges. Write coverage to alpha.
3. Write per-strand random value and root to tip gradient to separate channels.
4. Write gap darkness to the occlusion channel and a low-variance roughness value.
5. Pack strips into the atlas with padding, and dilate colour into the empty areas so mips do not bleed.
6. Build mips with coverage preservation so distant hair does not thin out.

**If you use Gemini for reference sheets:** send text only, attach no game image, and check your plan's terms on output rights. AI output will still need cleanup, and it won't give you reliable data channels. Brief:

> Top-down scan of isolated human hair clumps on a pure black background, no person, no face. A sprite sheet with evenly spaced vertical clumps, each tapering from a dense root at the top to fine wispy tips at the bottom. Mix straight, wavy, curly and tangled clumps at varied density. Fine individual strands, no background texture, greyscale, neutral even lighting, sharp focus.

## 8. Engine plan

**Today (2026-11-05), D3D12 and Vulkan:**

- **Pipeline:** `hair_groom/groom.py <groom> --install Assets/hair/wyatt` bakes three card levels; then `hair_groom/cap.py` builds the cap, conforms the cards to it and writes the fine hairline (see `docs/hair_maker_spec.md` section 6).
- **Import** (`src/mesh_import.h`): `AddScalpCards` loads `scalp_cards.json` / `_lod1` / `_lod2` as parts with camera-distance bands (0 to 2.5 m, 2.5 to 7 m, 7 to 45 m) plus `hairline_cards.json` (to 7 m). `AddHairCap` loads `hair_cap.json` (always drawn). `AddHairCards` loads beard, brows and lashes. Every vertex rides its nearest skin vertex (grid lookup). Per-vertex data is packed into the float colour (`src/character/hair_vertex.h`): t, a random value per card, class, depth inside the hair volume and melanin. Per-character colour is in `Assets/hair/<char>/hair_look.json`.
- **Shading** (`HairCardPS`): Marschner R/TT/TRT in Karis's 2016 form, with the strand tangent taken from the screen-space derivative of t. Colour comes from melanin plus per-strand variation. It adds multiple-scattering diffuse with a coloured shadow tint, plus volume occlusion from depth and root darkening.
- **Coverage:** alpha is sharpened by its screen-space slope when magnified, then hash-tested (Wyman and McGuire 2017) and resolved by TAA. This keeps coverage through mips without sorting.
- **Shadows:** hair casts into the tight character map through `DrawShadowCasterCutout` (`ShadowCutoutVS` / `PSShadowCutout`), and receives with a wider soft filter.
- **Not built:** wind and physics, hat compression, the Hair Maker's interactive clay sculpt (the cap reads a sculpted OBJ with `--clay` once it exists), and the hairline shell pass.

**Strand density (owner's review of the study meshes):** about 10 to 15 strands per beard card and 40 to 50 per head card, spaced so light passes through. The generator's cells follow this. Wyatt's scalp cards are short beard-style cards, and long head cards are not built yet.

**All steps are Recommended. Every rendering change must land on both Direct3D 12 and Vulkan.**

1. **Curved strips:** 6 to 8 rows and 2 to 3 vertices across, with a slight curve, replacing flat strips.
2. **Shingle placement:** align card normals to the skin normal, in layers from 3 to 20 mm, drawn inside to outside.
3. **Skin underlay:** paint the dark hair and beard zone into the head albedo.
4. **Original atlas:** the generator from section 7.
5. **Palette colour:** a small tint texture, with rows chosen per character.
6. **Edges:** alpha to coverage, with mips that preserve coverage, then a thin hairline and flyaway mesh.
7. **Hair lighting:** a strand-direction specular model such as Kajiya-Kay or Marschner, using the UV-derived direction. This is a choice for this engine, not something measured from the game.
8. **Vertex data and wind:** per-vertex tip weight and random phase, then vertex-shader wind. Beards should follow skin weights.
9. **LODs:** reduce rows, then remove the small filler cards.

**Budgets:** 6k to 10k triangles for hair, 1.5k to 2.5k for a full beard, about 1k for brows. On mobile, start at half and test overdraw, because layered alpha is the main cost.

## 8a. Placement formula for head cards

**Evidence (Measured):** one local script, about 56k scalp cards across 113 meshes, pooled into coarse regions and three length groups. Only medians and spreads left the script, and left and right were mirrored together. No card positions or per-style data were recorded, so this describes hair in general and cannot rebuild any one hairstyle.

| Region, mid-length group | Flow angle | Flow spread | Face tilt from radial | Root offset | Length |
|---|---|---|---|---|---|
| Front, upper | 114° | 69° | 44° | 5.7 mm | 64 mm |
| Side, upper | 43° | 40° | 22° | 5.1 mm | 83 mm |
| Side, mid | 40° | 17° | 14° | 4.3 mm | 59 mm |
| Side, low | 22° | 30° | 22° | 3.9 mm | 40 mm |
| Back, upper | 5° | 38° | 31° | 6.3 mm | 115 mm |
| Back, mid | 11° | 17° | 11° | 5.8 mm | 67 mm |
| Back, low | 11° | 25° | 26° | 5.3 mm | 53 mm |

Flow angle is measured on the skin: 0° runs toward the neck and 90° runs horizontally, toward the back of the head on the sides. Across all regions, width is about length / 3.5.

**Formula (Recommended, fitted by hand and not yet tested on a head).** For a root at azimuth `az` (0 at the nose, 180 at the back, mirrored) and elevation `el` from the skull centre:

    t        = clamp((az - 20) / 150, 0, 1)
    flow     = style.sweep * (1 - t)^1.5                        // sweep about 105; the measured default
    spread   = 18 + 37 * smoothstep(15, 40, el) + 12 * smoothstep(0, -20, el)   // Gaussian sigma, degrees; add 20 at the front
    offset   = 4 mm + 3 mm * smoothstep(0, 55, el) + 1.5 mm * t
    tilt     = 12 + (style.length / 160 mm) * 40 * smoothstep(10, 55, el)
    length   = style.length * lerp(0.45, 1, smoothstep(-20, 35, el))
    width    = length / 3.5 * random(0.8, 1.2)

Each card starts in the skin's tangent plane, turned by `flow + gaussian(spread)` from the neck direction, then bends to follow the skull. Below the widest part of the skull it lifts off, because the head narrows there: about 15° on the sides, and 20 to 35° at the back on mid and long hair.

**Making a style original:** `style` holds the length, sweep, parting line, curl and a seed. A parting or a crown whorl replaces `az` and `el` with distance from that feature. Pooling removed partings from the data, so each style must author its own.

**Limits:** bins use a sphere around an estimated skull centre, not the true skin normal. Flow is poorly defined near the top of the head, where the spread is largest. This root offset does not separate layers by draw order (5.2 mm in the first fifth, 4.8 mm in the last), so inside-to-outside layering must be set by the generator.

## 9. Not determined

- The exact shader blend of albedo channels, palette and material. I did not read the shader.
- What COLOR0 R, COLOR1 A, albedo G and material R and B actually do.
- The real pass order and sorting. The game's meshes are labelled for alpha rendering, but draw setup was not captured.
- The authoring tools Rockstar used. Nothing here reveals them.
- How hair behaves in motion. Only rest poses were examined, with no animation or physics.
