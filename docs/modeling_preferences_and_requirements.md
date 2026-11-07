# 3D modeling preferences and requirements

Compiled from the project's chat history (about 185 of your messages across ~55 sessions), `docs/city_draw_budget_plan.md`, `tools/*_gen`, and the `assets/` tree. Statements marked **(you said)** are near-verbatim from your messages.

## 1. The core rule

**Fewest possible polys, maximum visible detail.** Detail comes from textures, normal maps and silhouette/parallax-occlusion mapping (SPOM/POM), not geometry.
- "abuse the fuck out of normal maps and silhouette POM" (you said)
- "use SPOM to increase detail while reducing polys" (you said)
- "low poly but high detail" / "super low poly, super high detail" is your standard brief for almost every asset.
- Structural 3D (balconies sticking out, canopies, roof overhangs) is done with SPOM. "forget the lamp post and stupid fine details" (you said). Detail should be on the structure, not small props.
- Tall thin things (poles, pylons, smoke stacks) get baked into an image on 1 rectangle or 2 polys.
- Tracks, roads and similar linear features are a spline plus a texture, not meshes. Rail is SPOM on a flat plane.
- Workflow you asked for: build the model, bake a custom texture from it, then use the 3D data to drive the SPOM.

## 2. Performance constraints (hard limits)

- **Target: iPhone 15 at 25 fps, rendered below 720p and upscaled to 720p** (you said).
- Thousands of citizens and hundreds of distinct meshes, so minimal draw calls matters as much as tri count.
- Plan in `docs/city_draw_budget_plan.md`: buildings' share is at most 80 draws, 300k tris and 120 MB GPU memory. iOS needs ASTC textures.
- You asked whether many separate SPOM textures would eat RAM, and whether to make everything first, then pack into a shared atlas. The answer pointed to the shared atlas and baked blocks.
- Cargo ship: bake 5 cargo meshes to cut draw count and cull hidden faces. Make the cargo boxes 12 polys each.
- Per-asset caps you set: apartments under about 1,200 tris each. Substations, water pumps and plane/boat models were rejected for being too high-poly.
- Calibrate against "the rest of the game's models". Twice you rejected new models for being out of line with existing poly levels.

## 3. Style: realistic, not cartoonish

- "make them more realistic", "dont make so stylized", "dont make these cartoonish" (you said). Redo designs you dislike rather than tweaking.
- Match real-world or American references. Example: ambulance "realistic like american".
- Variety matters. "L1 all look identical, give them character, make each one unique" (you said). Row houses, windows, factories and apartments should not look like copies.
- Glass needs correct reflections and roughness textures, with real lobbies and bigger windows on ground floors. Windows should have a way to vary.
- Fix broken walls, flat ground floors with no detail ("the base has no detail"), and clipping textures at a distance.
- Avoid: helicopters, spinning turbines, and stylized designs.

## 4. Game fidelity: copy Cities: Skylines 2

- Research what CS2 has first (types, sizes, levels) and model that set. When a list gets too long, cut it back to CS2's ("reduce to the ones the CS2 game has").
- Use `cs2.paradoxwikis.com` as the source. Check it before answering.
- Model every plot size for each type, for example every low-density business building per plot size, with compatible parking.
- Buildings follow grid footprints (2x2, 3x3, 4x5 and so on), with levels L1 to L5 where CS2 has them (residential, apartments, offices, schools).
- The goal is a 1:1 CS2 clone with extra life-system features. You want a full asset list: "EVERY ASSET", not only vehicles.

## 5. Layout and context rules

- Footprint must stay inside the lot. Row houses sit next to each other with no gaps.
- **No trees, cars, planes, outer roads or sidewalks** baked into building models ("no trees", "no cars because we already have cars"). Only internal facility roads where needed (for example the sorting facility leading to the truck bay), and smaller parking up front.
- Scale up facilities that should be big (sorting facility "amazon size", high school taking more land, nuclear plant with more space and buildings).
- Anything with signage text: leave it blank so any texture or lettering can be applied.
- Buildings flatten the terrain to meet the road height at their center. Fix clipping.
- Underground stations: ground goes translucent, stairway slope, entrance aboveground, no road or sidewalk.
- Rail yard: a big garage with entrances, a spawn point for trains, and no trains in the mesh.
- Roads: keep pieces as separate meshes because roads are procedural. Include elevated and ground variants, and a driveway on houses.

## 6. Vehicles and characters

- Vehicles are low-poly with high detail. Gemini (via Chrome extension) makes **textures only**, not models ("No idiot use Gemini for the textures").
- Avoid repeated tries; get it right in few attempts ("too many tries").
- Planes: realistic, animate only the landing gear (retracts into the mesh). The plane animates a flare, taxis to a random gate, then takes off.
- People: MakeHuman base reduced to a tiny LOD (350 to 375 tris body, with higher-detail hands and face). Clothes come as texture variation or small meshes with minimal draw calls. Don't raise polys, fix the texture instead.
- Subway, trains and cargo cars reuse the same track and train model family.

## 7. Process preferences

- List the types first, confirm, then model. Verify the 3D models exist before building the system that needs them.
- Show results visually: grids with poly count next to the name, mesh view, and screenshots.
- Ask "how many polys" often. Report counts per asset.
- You call out low effort and wrong direction bluntly. Fix the specific complaint rather than doing a general rework.
- Commit after the work is done.

## 8. What exists today (GLB counts under `assets/`)

| Area | Count |
|---|---|
| Low commercial | 1,165 (large because of variants) |
| Industry | 60 (plus 240 backup variants) |
| Medium apartments | 125 |
| High density | 80 |
| Low office | 60 |
| Low density homes | 35 |
| Row housing | 25 |
| Education 9, fire 6, health 6, police 5, power 9, telecom 4, waste 4, water 6 | |
| Cars 121, commercial 27, service 25, transit 26, train 36, subway 4 | |
| Rail 8 | |

Generators live in `tools/building_gen/` and `tools/vehicle_gen/`, so changes should go through those scripts.

## 9. Known gaps

- Everything is "an estimate until measured on a device". No iOS build or measured fps yet.
- Engine is D3D12/Vulkan only. iOS needs Metal or MoltenVK.
- Substation, pylons, ports, airports and agriculture are modeled in sessions but the checklist HTMLs (`asset_needs_checklist.html`, `game_checklist.html`) hold the full missing list.
