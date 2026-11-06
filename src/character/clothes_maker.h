// Clothes Maker (Character Creator, CLOTHES tab): garments built from a recipe and fitted to the character's own body mesh.
// First garment: a button-up collared work shirt (pinstripe chambray by default), sleeves rolled below the elbow, tucked into
// the scene's jeans. Pipeline (all CPU, runs on a worker thread, ~1 s):
//   1. the body torso GLB (skinned, T-pose, metres) is welded; joints give shoulders, elbows, hands, neck, clavicle.
//   2. the triangles the shirt covers are cut out (sleeve length, hem, neckline, the V of the undone buttons), subdivided once,
//      and again (red-green, no cracks) where folds live: the waist band, above the rolled cuff, the elbow.
//   3. offset along the body normal by ease + cloth thickness (less on the shoulders, more under the sleeves), then relaxed:
//      Laplacian smoothing with a "stay outside the body" constraint, so the cloth bridges hollows instead of painting the skin.
//   4. the jeans are ray cast (radius per angle and height around the waist): below the waistband the shirt is pulled inside
//      them, above it blouses out and droops over the band with gathered folds that run up from the belt and lean toward the
//      front centre; sleeves bunch above the roll and crease at the inner elbow.
//   5. pieces: collar (stand + fold + leaf, points at the front, opened when buttons are undone), placket, buttons, chest
//      pockets with pointed flaps, rolled cuffs; all pushed outside the shirt body.
//   6. UVs: box charts on the torso (front/back/sides/yoke), cylindrical sleeves, own charts for every piece, shelf packed into
//      one 2048 atlas; albedo (stripes, chambray slub, fade, dirt, sweat) and a normal map are generated per texel.
//   7. written as a skinned GLB (the body's skeleton, weights from the nearest body vertex) with the uncovered body skin as an
//      M_Skin part, like the other clothing GLBs, plus albedo.png / normal.png / recipe.json beside it.
//
// Since 2026-11 the CLOTHES tab builds the shirt as real cloth instead (BuildShirtCloth): a sewing pattern drafted from the body's
// measurements, sewn and simulated in Blender headless (gameenginetools/clothes_sim/build_garment.py), styled by simulating with
// pins (tail tucked into the jeans and pulled down, sleeves rolled below the elbow), baked to a 12-20k triangle skinned GLB with
// pattern UVs. BuildShirt is the legacy remeshed-skin builder, kept for reference.
#pragma once
#include <cstdint>
#include <string>

namespace tbd::character::clothes {

struct ShirtRecipe {
    std::string name = "button_shirt";
    // fit, metres unless said
    float ease = 0.007f;          // air between body and cloth on the torso (tucked and pulled down: it hugs)
    float sleeveEase = 0.012f;    // around the arms
    float thickness = 0.0022f;
    float sleeveLength = 0.60f;   // 0 shoulder .. 1 wrist; the elbow is near 0.5
    bool rolled = true;           // a rolled cuff at the sleeve end
    float rollWidth = 0.048f, rollThick = 0.006f;
    bool tucked = true;           // into the jeans
    float tuckDepth = 0.07f;      // shirt tail below the waistband
    float blouse = 0.12f;         // 0..1 how much fabric hangs over the waistband (pulled tight: almost none)
    float collarStand = 0.025f, collarLeaf = 0.042f, collarSpread = 0.55f;
    int openButtons = 1;          // undone from the top (0: buttoned to the collar)
    bool pockets = true;
    float drape = 1.0f;           // 0 off .. 1 the cloth simulation (gravity, slack, folds)
    float wrinkle = 0.9f;         // 0..1.5 fold depth
    float wrinkleScale = 1.0f;    // fold spacing
    uint32_t seed = 7;
    // sewing pattern and fabric feel (the cloth builder; the fit fields above are the legacy builder's)
    float chestEase = 0.09f;      // total over the chest girth
    float waistEase = 0.06f;      // total over the waist girth: the spare width that gathers above the belt
    float tuckPull = 0.03f;       // how far the tail is pulled down inside the jeans (the vertical tension)
    float backLength = 0.0f;      // extra back length
    float fabricWeight = 1.0f, stretch = 1.0f, bend = 1.0f, collarStiffness = 1.0f;
    // fabric
    float base[3] = {0.16f, 0.27f, 0.52f};           // linear: a mid chambray blue
    float stripeA[3] = {0.52f, 0.60f, 0.74f}; // light pinstripe
    float stripeB[3] = {0.05f, 0.07f, 0.15f}; // dark pinstripe
    float stripeSpacing = 0.0050f, stripeWidth = 0.0007f;
    float wear = 0.55f, dirt = 0.6f;
    float button[3] = {0.86f, 0.83f, 0.74f};

    bool Save(const std::wstring& path) const;
    bool Load(const std::wstring& path, std::string* err = nullptr);
};

struct BuildInput {
    std::string bodyGlb;  // body_torso.glb
    std::string legsGlb, headGlb; // body_legs.glb, body_head.glb beside it (collision; the neck size)
    std::wstring script;  // gameenginetools\clothes_sim\build_garment.py
    std::string jeansGlb; // "" or a pants GLB in the same space (the shirt tucks into it)
    std::wstring outDir;  // folder, created; <name>.glb, albedo.png, normal.png, recipe.json go there
};

struct BuildResult {
    bool ok = false;
    std::string glbPath;  // UTF-8
    std::string error, summary;
    double ms = 0;
    size_t triangles = 0;
};

BuildResult BuildShirt(const ShirtRecipe& r, const BuildInput& in);       // legacy: offset body skin
BuildResult BuildShirtCloth(const ShirtRecipe& r, const BuildInput& in);  // sewn + simulated in Blender (1-2 minutes)
std::string ClothProgress(); // the running cloth build's last progress line, safe from any thread

} // namespace tbd::character::clothes
