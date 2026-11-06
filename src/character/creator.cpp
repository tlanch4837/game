// Character Creator workspace, implementation (overview and API in creator.h). GDI, like the face tab and the inspector.
#define UNICODE
#define _UNICODE
#include "creator.h"

#include <commdlg.h>
#include <windowsx.h>
#include <map>
#include <memory>

#pragma push_macro("small") // rpcndr.h defines small as char; face_draw.h's Style has a member of that name
#undef small
#include "../mocap/face_draw.h"
#include "../project.h"
#include "character_preset.h"
#include "eye_paint.h"
#include "hair_paint.h"
#include "hair_clay.h"
#include "brow_maker.h"
#include "skin_paint.h"
#include "photo_fit.h"
#include "photo_skin.h"
#include "../image_decode.h"
#include "clothes_maker.h"
#include <atomic>
#include <thread>
#include "../undo.h"

namespace tbd::character {
namespace {

using namespace facedraw;

enum Action {
    A_None, A_TabBody, A_TabHead, A_New, A_Save, A_Load, A_Reset, A_Random, A_SeedDown, A_SeedUp, A_Age, A_Height,
    A_BodyRandom, A_BodySeedDown, A_BodySeedUp,
    A_BrowDraw = 20, A_BrowDefault, A_BrowBake, A_BrowGuides, // brow maker (brow_maker.h)
    A_BrowDensity = 330, A_BrowLength,          // brow maker sliders
    A_View3D = 30, A_View2D, A_ViewBoth, // view mode: 3D viewport / 2D texture (UV) view / side by side
    A_HairTool = 40, // + HairPainter::Tool (Paint..Smooth)
    A_Features = 60, // feature view: rig regions + wireframe over the head
    A_InnerCorner = 62, A_PaintClear, A_EyeZoom, A_EyeReset, A_EyeIrisColor, // skin paint / eye tools
    A_LayerTool = 70, // + (SkinPaint tool id - 10): 70..75
    A_TintType = 80,  // + 0..7
    A_SpotKind = 90,  // + 0..2
    A_Eye = 300,      // + slider index (see EyeSlider)
    A_Groom = 370,    // + index into State::grooms; A_Groom - 1: no head hair
    A_ClayTool = 380, // + HairClay::Tool (Inflate..Comb)
    A_ClayLoad = 387, A_ClayUndo, A_ClayGenerate, A_ClayHair, A_ClayCombView, A_ClayHairline, A_ClayHeight, A_ClayPaint, A_ClayErase, A_ClayFill,
    A_EyePlace = 310, // + 0..3: eyeball depth, spread, height, size
    A_MapView = 190,  // + map index (see kMapNames); below A_Face, which starts the slider range
    A_HairFill = 50, A_HairClear, A_HairBake, A_HairColor, A_HairRadius, A_HairStrength, A_HairHardness, A_HairSymmetry,
    A_Skin = 100, A_Face = 200, A_Body = 400, A_Fold = 900, // + index into State::foldKeys
    A_Cloth = 500, // + index into kClothSliders (CLOTHES tab)
    A_TabClothes = 2000, A_ClothBuild, A_ClothAuto, A_ClothRestore, A_ClothRolled, A_ClothTucked, A_ClothPockets,
    A_ClothBtnDown, A_ClothBtnUp, A_ClothSeedDown, A_ClothSeedUp, A_ClothSave, A_ClothLoad,
    A_ClothColor = 2030, // + 0 base, 1 light stripe, 2 dark stripe, 3 buttons
    A_TabPhoto = 2040, A_PhotoAdd, A_PhotoRemove, A_PhotoLabel, A_PhotoHide, A_PhotoClear, A_PhotoAuto, A_PhotoOverlay, A_PhotoSbs, A_PhotoFit, A_PhotoCustom, A_PhotoCustomDel, A_PhotoShape, A_PhotoShapeReset, A_PhotoSkinRead, A_PhotoSkinProject, A_PhotoSkinClear, A_PhotoPlaceReset, // PHOTO FIT tab
    A_PhotoFov = 2060, A_PhotoWire, A_PhotoYaw, A_PhotoSkinStr, A_PhotoSkinWarm, A_PhotoSkinExp, A_PhotoSize, A_PhotoOffX, A_PhotoOffY, // sliders
    A_PhotoPin = 2100,  // + pin 0..49
    A_PhotoSel = 2160   // + photo 0..15
};
struct Hit { RECT r; int action; };

struct BodyPart {
    const char* slot;   // file stem: body_torso / body_legs
    MorphSet set;
    HeadMorpher morph;
    int target = -1;
    std::string name;
};

struct State {
    Host host;
    bool hostSet = false;
    std::wstring tools; // ...\gameenginetools
    // face
    MorphSet face;
    std::wstring facePath;
    bool faceTried = false;
    std::string faceError;
    HeadMorpher head;
    int target = -1;
    std::string targetName;
    std::vector<float> values;
    std::vector<Vertex> headNow; // the head as drawn (morphed or rest), for hair picking
    float age = 0.0f;
    uint32_t seed = 1;
    long long presetSeed = -1;
    std::string presetName = "custom";
    Json extra; // preset blocks the creator does not edit
    bool faceDirty = false;
    // body: body_maker's three part files, one values vector for all of them
    BodyPart parts[2] = {{"body_torso"}, {"body_legs"}};
    MorphSet bodyHead; // the neck part on body_head: added to the face deltas
    std::vector<float> bodyValues;
    bool bodyLoaded = false, bodyDirty = false;
    float heightScale = 1.0f, heightMin = 0.92f, heightMax = 1.08f, heightSigma = 0.035f, heightShown = 1.0f;
    uint32_t bodySeed = 1;
    long long bodyPresetSeed = -1;
    std::string bodyError = "body morphs not built yet";
    ULONGLONG bodyPoll = 0;
    // skin / hair grooms
    std::vector<std::string> skins, grooms; // grooms: the styles in Assets\hair\styles (hair_groom/groom.py --install + cap.py)
    std::string groom;                      // the head's style now ("none", a style, or "own" = cards in its own hair folder)
    int skin = -1;
    std::vector<std::string> skinInfo;
    // skin paint (tint / spots / shine layers, SkinPaint), the maps viewer and the eye look
    SkinPaint sp;
    int tintType = 0, spotKind = 0;
    int mapView = 0; // 2D view shows: 0 final albedo, 1 hair density, 2 hair flow, 3 skin tint, 4 dark spots, 5 shine
    std::vector<uint8_t> mapImg;
    std::vector<uint8_t> baseSurface; // the skin surface map as loaded (shine is painted into a copy)
    int surfW = 0, surfH = 0;
    TextureId surfTex = kNoTexture, surfOrig = kNoTexture;
    bool surfDirty = false;
    EyeLook eye;
    // eyeball placement: depth back, spread out (each eye), height (all mm), size. The authored balls leave a gap at the outer corners where
    // the inside of the head shows red, so the default fits them: 0.3 mm back, 1.8 mm out, 2% larger (all 0 / 1: as authored)
    float eyePlace[4] = {0.3f, 1.8f, 0.0f, 1.02f};
    std::vector<uint8_t> eyeBase;
    int eyeBaseW = 0, eyeBaseH = 0;
    size_t eyeFirst = 0;
    bool eyeFound = false, eyeDirty = false, eyeAuto = false;
    TextureId eyeTex = kNoTexture, eyeOrig = kNoTexture;
    double eyeLastBake = 0;
    // hair painting
    HairPainter hair;
    int hairTool = HairPainter::None;
    bool symmetry = false; // hair dabs also land mirrored across the head's x = 0 plane
    // Hair Maker: clay volume + comb (hair_clay.h), grown into a style by clay_groom.py + cap.py
    HairClay clay;
    int clayTool = HairClay::None;
    bool clayStroke = false, clayShowHair = false, clayCombView = true;
    bool clayLoopDraw = false, clayLoopStroke = false; // drawing the hairline loop on the scalp
    bool clayMaskMode = false; // the clay follows the painted scalp (the hair painter's density): rebuilt after every stroke
    std::vector<HairClay::V3> clayLoop;
    float clayHeight = 12.0f; // mm the hairline clay is inflated by
    HairClay::V3 clayAt = {0, 0, 0}, clayPlaneN = {0, 0, 1};
    POINT clayMouse = {0, 0};
    bool clayHover = false;
    HairClay::V3 clayHoverP = {0, 0, 0}, clayHoverN = {0, 1, 0};
    HANDLE genProc = nullptr;
    double genStart = 0;
    float hairRadius = 8.0f, hairStrength = 0.6f, hairHardness = 0.0f; // hardness 0: the old soft falloff
    bool hairReady = false, stroking = false, hairDirty = false, bakeNow = false;
    HairHit lastHit, lastMirror, hover; // hover: where the 3D brush is now (mouse over the head with a tool on)
    double lastLiveBake = 0;
    POINT strokeAt = {}, stroke2dAt = {}; // last mouse position of the 3D / 2D stroke: moves are filled in between
    std::vector<uint8_t> tile, albedo, baseAlbedo; // albedo = baseAlbedo recoloured by the skin tone (toneGain)
    float toneGain[3] = {1, 1, 1};
    TextureId bodyTex[2] = {kNoTexture, kNoTexture}; // torso / legs recoloured albedo (kNoTexture: their own)
    TextureId bodyOrig[2] = {kNoTexture, kNoTexture};
    int tileW = 0, tileH = 0, albedoW = 0, albedoH = 0;
    std::string albedoPath;
    TextureId origTex = kNoTexture, bakedTex = kNoTexture;
    double bakeMs = 0, texMs = 0;
    std::vector<uint8_t> shown; // the last baked albedo (RGBA), what the 2D view draws
    float uvPerMetre2D = 0;    // 2D brush radius in mm -> UV (mean over the head)
    // feature view: which face-rig group moves each texel (morphs.json groups), region outlines and the UV wireframe
    bool features = false;
    std::vector<int8_t> featRegion; // per albedo texel: group index, -1 skin no group moves, -2 not skin
    std::vector<uint8_t> featWire;  // per albedo texel: 1 on a triangle edge, 2 inside the eyebrow hair zone (bit 2)
    // brow maker: the drawn root boundary, sliders, the baked cards' texture (new per character)
    BrowMaker brow;
    bool browDraw = false, browStroke = false, browBaked = false, browFull = false, browGuides = true;
    std::vector<Vertex> browGuideLines; // world-space line pairs (cached: rebuilt at most every 150 ms)
    double browGuideAt = -1e9;
    double browLastPreview = 0;
    std::vector<std::array<float, 2>> browStrokePts;
    TextureId browTex = kNoTexture;
    size_t browFirst = 0, browOrdinal = 0;
    std::string browStatus;
    std::wstring hairDensityFile, hairFlowFile; // as loaded from / saved to a preset
    // ui
    std::string status = "Pick BODY, HEAD or CLOTHES.";
    double deformMs = 0, uploadMs = 0;
    int tab = 1; // 0 body, 1 head, 2 clothes, 3 photo fit
    // clothes maker (CLOTHES tab, clothes_maker.h): the recipe, a build on a worker thread, the scene's shirt entity
    clothes::ShirtRecipe shirt;
    std::thread clothJob;
    std::atomic<bool> clothDone{false};
    bool clothBusy = false, clothAgain = false, clothAuto = true;
    clothes::BuildResult clothRes;
    std::string clothStatus = "Press BUILD SHIRT: it is fitted to body_torso and tucked into the scene's jeans.";
    std::string clothOrigPath; // the shirt entity's file before the made shirt replaced it ("" none)
    int clothEntity = -1;
    bool open = false;
    HWND wnd = nullptr, wnd2d = nullptr;
    RECT dock = {}, dock2d = {};
    int view = 0; // 0 3D, 1 2D, 2 3D + 2D
    bool stroke2d = false;
    float last2dU = -1, last2dV = -1;
    float zoom2d = 1.0f, cu2d = 0.5f, cv2d = 0.5f; // 2D view zoom and the atlas point at its centre
    bool pan2d = false, hover2d = false, bgraDirty = true;
    POINT panFrom = {}, mouse2d = {};
    std::vector<uint8_t> bgra; // the shown atlas as BGRA for GDI
    Style st;
    std::vector<Hit> hits;
    std::vector<std::wstring> foldKeys; // this paint's drop-down headers, A_Fold + index
    std::map<std::wstring, bool> foldOpen; // drop-down state by key (tab + title); missing: the header's default
    int drag = 0, scroll[4] = {0, 0, 0, 0};
    bool envDone = false;
    // photo fit (PHOTO FIT tab, photo_fit.h)
    std::vector<photo::Photo> photos;
    int photoActive = -1, photoPin = 0, photoNext = -1, dragPin = -1, viewBeforePhoto = 0;
    photo::BasePins pins;
    bool pinsTried = false, photoSbs = false, photoAutoNext = true, photoLoaded = false;
    float pZoom = 1.0f, pCx = 0.5f, pCy = 0.5f, wireAlpha = 0.85f, faceCentre[3] = {0, 0, 0};
    std::vector<Vertex> photoTris;
    photo::Layer layer;
    uint64_t layerKey = 0;
    std::string photoStatus;
    std::vector<float> shapeD; // pin displacement per unit of each face slider (FitShape), built once per pin set
    uint64_t shapeKey = 0;
    float skinStrength = 1.0f, skinWarm = 0.0f, skinExp = 0.4f; // photos are display-referred: a little brighter albedo than the picture
    int albedoStamp = 0; // counts bakes, so the side-by-side head redraws with the new skin
    photo::SkinReport skinRep;
    bool customPlacing = false;
    float viewYaw = 0; // radians: turns the rendered head (side by side) away from the photo's angle, to place pins on other sides
    std::vector<uint32_t> photoSrc; // photoTris vertex -> headNow vertex
    int hoverPin = -1, prevPin = -2; // the pin row under the mouse; the pin the preview was drawn for
    bool hoverTrack = false;
    photo::Layer prevLayer;
};
State& S() { static State s; return s; }

double NowMs() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string Narrow(const std::wstring& w) { return WideToUtf8(w); }
void Redraw() { if (S().wnd) InvalidateRect(S().wnd, nullptr, FALSE); }

// ---------------------------------------------------------------- data
std::wstring ToolsDir() {
    State& s = S();
    if (!s.tools.empty()) return s.tools;
    std::wstring here = Utf8ToWide(__FILE__); // ...\src\character\creator.cpp, when built from the tree
    for (wchar_t& c : here) if (c == L'/') c = L'\\';
    const size_t cut = here.rfind(L"\\src\\character\\");
    const std::wstring tree = cut == std::wstring::npos ? L"" : here.substr(0, cut) + L"\\gameenginetools";
    const std::wstring nextToBin = ExeDirectory() + L"\\..\\gameenginetools";
    s.tools = Exists(nextToBin + L"\\face_maker") ? nextToBin : tree;
    return s.tools;
}

bool LoadFace() {
    State& s = S();
    if (s.faceTried) return s.face.Valid();
    s.faceTried = true;
    wchar_t env[MAX_PATH] = {};
    s.facePath = GetEnvironmentVariableW(L"TBD_MORPHS", env, MAX_PATH) > 0 ? std::wstring(env) : ToolsDir() + L"\\face_maker\\out\\morphs.json";
    if (!s.face.Load(s.facePath, &s.faceError)) {
        s.faceError = "face morphs: " + s.faceError + " (" + Narrow(s.facePath) + ")";
        Log("Character: %s", s.faceError.c_str());
        return false;
    }
    s.values.assign(s.face.sliders.size(), 0.0f);
    Log("Character: %zu face sliders from %ls (rest_checksum %s)", s.face.sliders.size(), s.facePath.c_str(), s.face.restChecksum.c_str());
    return true;
}

// body_maker\out\body_morphs_{torso,legs,head}.json (morphs.json format each), polled every 2 s until they appear.
void PollBody() {
    State& s = S();
    if (s.bodyLoaded || GetTickCount64() < s.bodyPoll) return;
    s.bodyPoll = GetTickCount64() + 2000;
    wchar_t env[MAX_PATH] = {};
    const std::wstring dir = GetEnvironmentVariableW(L"TBD_BODYMORPHS", env, MAX_PATH) > 0 ? std::wstring(env) : ToolsDir() + L"\\body_maker\\out";
    const std::wstring torso = dir + L"\\body_morphs_torso.json", legs = dir + L"\\body_morphs_legs.json", headPart = dir + L"\\body_morphs_head.json";
    if (!Exists(torso) || !Exists(legs)) { s.bodyError = "body morphs not built yet (" + Narrow(torso) + " is missing)"; return; }
    std::string err;
    if (!s.parts[0].set.Load(torso, &err) || !s.parts[1].set.Load(legs, &err)) { s.bodyError = "body morphs: " + err; s.bodyPoll = GetTickCount64() + 10000; return; }
    if (Exists(headPart) && !s.bodyHead.Load(headPart, &err)) Log("Character: body_morphs_head.json: %s (neck stays as it is)", err.c_str());
    const auto same = [&](const MorphSet& m) {
        if (m.sliders.size() != s.parts[0].set.sliders.size()) return false;
        for (size_t i = 0; i < m.sliders.size(); ++i) if (m.sliders[i].name != s.parts[0].set.sliders[i].name) return false;
        return true;
    };
    if (!same(s.parts[1].set) || (s.bodyHead.Valid() && !same(s.bodyHead))) { s.bodyError = "body morph files disagree on the sliders"; s.bodyPoll = GetTickCount64() + 10000; return; }
    Json doc;
    if (MorphSet::ReadJson(torso, doc, nullptr))
        if (const Json* h = doc.Find("height")) {
            if (const Json* v = h->Find("min")) s.heightMin = (float)v->Num(0.92);
            if (const Json* v = h->Find("max")) s.heightMax = (float)v->Num(1.08);
            if (const Json* v = h->Find("sigma")) s.heightSigma = (float)v->Num(0.035);
        }
    s.bodyValues.assign(s.parts[0].set.sliders.size(), 0.0f);
    s.bodyLoaded = true;
    Log("Character: %zu body sliders from %ls (torso, legs%s)", s.bodyValues.size(), dir.c_str(), s.bodyHead.Valid() ? ", neck" : "");
}

// The body limiter: believable range, group RMS, and the displacement cap over ALL parts together (never per part).
void LimitBody(std::vector<float>& v) {
    State& s = S();
    const MorphSet& t = s.parts[0].set;
    for (size_t i = 0; i < v.size(); ++i) v[i] = std::clamp(v[i], t.sliders[i].lo, t.sliders[i].hi);
    for (const std::string& g : t.groups) {
        double sum = 0; int n = 0;
        for (size_t i = 0; i < v.size(); ++i) if (t.sliders[i].group == g) { sum += (double)v[i] * v[i]; ++n; }
        const double rms = n ? std::sqrt(sum / n) : 0;
        if (rms > t.groupRmsCap) for (size_t i = 0; i < v.size(); ++i) if (t.sliders[i].group == g) v[i] *= (float)(t.groupRmsCap / rms);
    }
    float dm = std::max(s.parts[0].set.MaxDisplacementMm(v), s.parts[1].set.MaxDisplacementMm(v));
    if (s.bodyHead.Valid()) dm = std::max(dm, s.bodyHead.MaxDisplacementMm(v));
    if (dm > t.dispCapMm) for (float& x : v) x *= t.dispCapMm / dm;
}

// Assets\characters\regular\: the engine's default character, a copy of the base 4K skin the importer leaves without Wyatt's hair
std::wstring RegularDir() { return ToolsDir() + L"\\..\\testproject\\WesternGame\\Assets\\characters\\regular\\"; }
std::string SkinDir(int k) {
    const std::string& n = S().skins[(size_t)k];
    return n == "Regular" ? Narrow(RegularDir()) : Narrow(ToolsDir()) + "\\skin_tex\\out\\" + n + "\\";
}

void ScanLooks() {
    State& s = S();
    if (!s.skins.empty() || !s.grooms.empty()) return;
    WIN32_FIND_DATAW fd;
    const auto dirs = [&](const std::wstring& root, const wchar_t* need, std::vector<std::string>& out) {
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && Exists(root + L"\\" + fd.cFileName + L"\\" + need))
                out.push_back(Narrow(fd.cFileName));
        while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    dirs(ToolsDir() + L"\\skin_tex\\out", L"body_head.glb", s.skins);
    dirs(ToolsDir() + L"\\..\\Assets\\hair\\styles", L"scalp_cards.json", s.grooms);
    // the default is always "Regular": the generic human (plain 4K base skin, no Wyatt beard / brows / scalp cards), first in the list
    if (Exists(RegularDir() + L"body_head.glb")) s.skins.insert(s.skins.begin(), "Regular");
}

int FindEntity(const char* stem) {
    State& s = S();
    for (int i = 0; i < s.host.entityCount(); ++i)
        if (s.host.entityPath(i).find(stem) != std::string::npos) return i;
    return -1;
}

bool Neutral(const std::vector<float>& v) {
    for (float x : v) if (x != 0.0f) return false;
    return true;
}
bool EyePlaceAuthored() {
    const State& s = S();
    return s.eyePlace[0] == 0 && s.eyePlace[1] == 0 && s.eyePlace[2] == 0 && s.eyePlace[3] == 1;
}
bool HeadNeutral() {
    State& s = S();
    return Neutral(s.values) && (!s.bodyHead.Valid() || !s.bodyLoaded || Neutral(s.bodyValues)) && EyePlaceAuthored();
}
// Eyeball placement after the face morphs: each ball scales about its own centre and moves (depth back, spread out, height), so the
// lids close over it and nothing behind shows at the corners. Ball vertices are the importer's eye parts, told apart left / right by
// the side of the eyes' middle, the way ApplyEyeMaps splits them.
void ApplyEyePlacement(std::vector<Vertex>& v) {
    State& s = S();
    if (EyePlaceAuthored() || s.target < 0) return;
    const ImportedModel* model = s.host.entityModel(s.target);
    if (!model) return;
    double midX = 0;
    size_t total = 0;
    for (const ImportedPart& p : model->parts)
        if (p.eye) for (size_t k = p.first; k < p.first + p.count && k < v.size(); ++k) { midX += v[k].px; ++total; }
    if (!total) return;
    midX /= (double)total;
    double c[2][3] = {};
    size_t n[2] = {};
    for (const ImportedPart& p : model->parts)
        if (p.eye) for (size_t k = p.first; k < p.first + p.count && k < v.size(); ++k) { const int sd = v[k].px >= midX ? 1 : 0; c[sd][0] += v[k].px; c[sd][1] += v[k].py; c[sd][2] += v[k].pz; ++n[sd]; }
    for (int sd = 0; sd < 2; ++sd) if (n[sd]) for (double& x : c[sd]) x /= (double)n[sd];
    const float depth = s.eyePlace[0] * 0.001f, spread = s.eyePlace[1] * 0.001f, height = s.eyePlace[2] * 0.001f, size = s.eyePlace[3];
    for (const ImportedPart& p : model->parts)
        if (p.eye)
            for (size_t k = p.first; k < p.first + p.count && k < v.size(); ++k) {
                const int sd = v[k].px >= midX ? 1 : 0;
                if (!n[sd]) continue;
                Vertex& x = v[k];
                x.px = (float)c[sd][0] + (x.px - (float)c[sd][0]) * size + (sd ? spread : -spread);
                x.py = (float)c[sd][1] + (x.py - (float)c[sd][1]) * size + height;
                x.pz = (float)c[sd][2] + (x.pz - (float)c[sd][2]) * size - depth;
            }
}

bool BindMorph(HeadMorpher& m, const MorphSet& set, int i, std::string& why) {
    std::vector<Vertex> rest;
    const ImportedModel* model = S().host.entityModel(i);
    std::string path = S().host.entityPath(i);
    path = path.substr(0, path.rfind("#native"));
    if (!model || !S().host.entityMesh(i, &rest) || !m.Bind(set, path, rest, *model)) { why = m.status; return false; }
    return true;
}

// With no head selected the creator works on the default human: a plain MakeHuman with the base 4K skin (characters\regular),
// never Wyatt. An existing Regular head is reused; otherwise head, torso and legs are added at the origin.
int DefaultHuman() {
    State& s = S();
    const std::string dir = Narrow(RegularDir());
    for (int i = 0; i < s.host.entityCount(); ++i)
        if (s.host.entityPath(i).find("characters\\regular\\body_head.glb") != std::string::npos) return i;
    if (!Exists(RegularDir() + L"body_head.glb") || !s.host.addEntity) return -1;
    const int head = s.host.addEntity(-1, "Human Head", dir + "body_head.glb#native");
    s.host.addEntity(-1, "Human Torso", dir + "body_torso.glb#native");
    s.host.addEntity(-1, "Human Legs", dir + "body_legs.glb#native");
    s.skin = 0; // Regular is first in the list
    Log("Character: added the default human (characters\\regular)");
    return head;
}

bool BindHead() {
    State& s = S();
    if (!LoadFace()) { s.status = s.faceError; return false; }
    int pick = -1;
    const int sel = s.host.selectedIndex();
    if (sel >= 0 && s.host.entityPath(sel).find("body_head") != std::string::npos) pick = sel;
    if (s.target >= 0 && s.target < s.host.entityCount() && s.host.entityName(s.target) == s.targetName && (pick < 0 || pick == s.target) && s.head.Bound()) return true;
    if (pick < 0) // nothing selected: the scene's own character first; the default human only in a scene without one
        for (int i = 0; i < s.host.entityCount() && pick < 0; ++i) if (s.host.entityPath(i).find("body_head") != std::string::npos) pick = i;
    if (pick < 0) pick = DefaultHuman();
    if (pick < 0) { s.status = "Cannot load the default human (Assets\\characters\\regular\\body_head.glb)."; return false; }
    std::string why;
    if (!BindMorph(s.head, s.face, pick, why)) {
        s.status = "Cannot morph " + s.host.entityName(pick) + ": " + why;
        Log("Character: %s", s.status.c_str());
        s.target = -1;
        return false;
    }
    s.target = pick;
    s.targetName = s.host.entityName(pick);
    s.headNow = s.head.Rest();
    s.hairReady = false; // the picking mesh changed
    Log("Character: %s %s", s.targetName.c_str(), s.head.status.c_str());
    return true;
}

void Report(const std::string& what, float mm) {
    State& s = S();
    char b[200];
    snprintf(b, sizeof(b), "%s: largest move %.1f mm, deform %.2f ms, upload %.2f ms", what.c_str(), mm, s.deformMs, s.uploadMs);
    s.status = b;
}

// ---------------------------------------------------------------- hair
// The baked albedo stays on the head when its mesh is made again (morphs, skin preset): swap it back in.
void SyncHairTexture() {
    State& s = S();
    if (s.target < 0) return;
    if (s.surfTex != kNoTexture && s.host.skinSurface) { // the painted shine, and the eye look, ride along the same way
        const TextureId cs = s.host.skinSurface(s.target);
        if (cs != kNoTexture && cs != s.surfTex) { s.surfOrig = cs; s.host.swapSurface(s.target, cs, s.surfTex); }
    }
    if (s.eyeTex != kNoTexture && s.eyeFound && s.host.partAlbedo) {
        const TextureId ce = s.host.partAlbedo(s.target, s.eyeFirst);
        if (ce != kNoTexture && ce != s.eyeTex) { s.eyeOrig = ce; s.host.swapAlbedo(s.target, ce, s.eyeTex); }
    }
    if (s.bakedTex == kNoTexture) return;
    const TextureId cur = s.host.skinAlbedo(s.target);
    if (cur == kNoTexture || cur == s.bakedTex) return;
    s.origTex = cur;
    s.host.swapAlbedo(s.target, cur, s.bakedTex);
}

// ---------------------------------------------------------------- hair style
// The head's hair folder, as the importer finds it (mesh_import.h HairDir): "hair\" beside the glb when that exists or the head
// is the generic human, else Wyatt's Assets\hair\wyatt. hair_style.json there picks a style from Assets\hair\styles.
void SyncBrowTexture();
std::wstring GroomFolder() {
    State& s = S();
    if (s.target < 0) return std::wstring();
    std::string file = s.host.entityPath(s.target);
    file = file.substr(0, file.find('#'));
    const size_t slash = file.find_last_of("\\/");
    if (slash == std::string::npos) return std::wstring();
    const std::wstring own = Utf8ToWide(file.substr(0, slash + 1)) + L"hair\\";
    const bool generic = file.find("characters\\regular\\") != std::string::npos || file.find("characters/regular/") != std::string::npos;
    if (generic || Exists(own + L"hair_style.json") || Exists(own + L"scalp_cards.json") || Exists(own + L"hair_cap.json")) return own;
    return ToolsDir() + L"\\..\\Assets\\hair\\wyatt\\";
}
std::string CurrentGroom() {
    const std::wstring dir = GroomFolder();
    std::vector<uint8_t> bytes;
    Json doc;
    if (dir.empty() || !ReadBytes(dir + L"hair_style.json", bytes) || !JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc))
        return Exists(dir + L"scalp_cards.json") ? "own" : "none";
    const Json* st = doc.Find("style");
    return st && !st->s.empty() ? st->s : "none";
}
// Writes the style into the head's hair folder and imports the head again; the face morphs, painted hair and brows are re-applied
// on the new mesh (its hair parts changed, so the rig binds again). "none": no head hair.
void ApplyGroom(const std::string& name) {
    State& s = S();
    const double t0 = NowMs();
    if (!BindHead()) return;
    if (!s.host.reloadEntity) { s.status = "Hair style: this editor cannot re-import a head."; return; }
    const std::wstring dir = GroomFolder();
    if (dir.empty()) { s.status = "Hair style: the head has no file to keep its hair beside."; return; }
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string text = "{\n \"style\": \"" + name + "\",\n \"_note\": \"Head hair style, written by the Character Creator (HAIR STYLE). "
                             "The cards, cap and hairline come from Assets\\\\hair\\\\styles\\\\<style>; none = bald.\"\n}\n";
    FILE* f = _wfopen((dir + L"hair_style.json").c_str(), L"wb");
    if (!f) { s.status = "Hair style: cannot write " + Narrow(dir) + "hair_style.json"; return; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    s.host.reloadEntity(s.target);
    std::string why;
    if (!BindMorph(s.head, s.face, s.target, why)) { s.status = "Hair style " + name + " set, but the face rig could not bind again: " + why; return; }
    s.headNow = s.head.Rest();
    s.hairReady = false;
    s.faceDirty = true;
    s.groom = name;
    SyncHairTexture();
    SyncBrowTexture();
    // the preset's hair block carries the style
    Json* hair = nullptr;
    for (auto& kv : s.extra.o) if (kv.first == "hair") hair = &kv.second;
    if (!hair) { s.extra.type = Json::Object; s.extra.o.push_back({"hair", Json()}); hair = &s.extra.o.back().second; hair->type = Json::Object; }
    bool set = false;
    for (auto& kv : hair->o) if (kv.first == "groom") { kv.second.type = Json::String; kv.second.s = name; set = true; }
    if (!set) { Json v; v.type = Json::String; v.s = name; hair->o.push_back({"groom", v}); }
    char b[200];
    snprintf(b, sizeof(b), "Hair style %s (%.0f ms).", name.c_str(), NowMs() - t0);
    s.status = b;
    Log("Character: %s", b);
}

// ---------------------------------------------------------------- hair maker (clay + comb)
const char* ClayToolName(int t) {
    static const char* names[] = {"off", "Inflate", "Deflate", "Smooth", "Grab", "Comb"};
    return t >= 0 && t < HairClay::ToolCount ? names[t] : "?";
}
std::wstring StyleDir(const std::string& style) { return ToolsDir() + L"\\..\\Assets\\hair\\styles\\" + Utf8ToWide(style) + L"\\"; }
void ClayLoad() {
    State& s = S();
    if (!BindHead()) return;
    if (s.groom.empty()) s.groom = CurrentGroom();
    std::string from = s.groom == "none" || s.groom == "own" || s.groom.empty() ? "side_part" : s.groom;
    if (!Exists(StyleDir(from) + L"clay.json")) from = "side_part";
    if (!s.clay.Load(StyleDir(from) + L"clay.json")) { s.status = "Clay: cannot load " + Narrow(StyleDir(from)) + "clay.json (" + s.clay.status + ")"; return; }
    s.clay.status = "Clay from " + from + ": " + s.clay.status;
    s.status = s.clay.status + ". Pick a tool and drag on the clay.";
    if (s.clayTool == HairClay::None) s.clayTool = HairClay::Inflate;
}
// The drawn hairline loop -> the clay (HairClay::FromHairline), inflated by the thickness slider. The loop must close: its ends
// within 3 cm of each other.
void ClayFromLoop() {
    State& s = S();
    const auto& L = s.clayLoop;
    if (L.size() < 12) { s.status = "Hairline: too short. Drag all the way around the hair."; return; }
    const float gap = std::hypot(std::hypot(L.front()[0] - L.back()[0], L.front()[1] - L.back()[1]), L.front()[2] - L.back()[2]);
    if (gap > 0.03f) { s.status = "Hairline: not closed (ends " + std::to_string((int)(gap * 1000)) + " mm apart). Finish near where you started."; return; }
    const double t0 = NowMs();
    if (!s.clay.FromHairline(L, s.headNow, s.clayHeight * 0.001f)) { s.status = "Hairline: " + s.clay.status; return; }
    s.clayShowHair = false;
    s.clayLoopDraw = false;
    char b[160];
    snprintf(b, sizeof(b), "Hairline -> clay %zu triangles, %.0f mm thick (%.0f ms). Sculpt it, comb it, then GENERATE.", s.clay.idx.size() / 3, s.clayHeight, NowMs() - t0);
    s.status = b;
    Log("Character: %s", b);
}

bool InitHair();
// The painted scalp (the hair painter's density map) -> the clay; its painted edge is the hairline.
void ClayFromMask() {
    State& s = S();
    if (!InitHair()) return;
    const double t0 = NowMs();
    if (!s.clay.FromMask(s.headNow, s.hair.density, HairPainter::N, s.clayHeight * 0.001f)) { s.status = "Scalp: " + s.clay.status; return; }
    s.clayShowHair = false;
    char b[160];
    snprintf(b, sizeof(b), "Scalp -> clay %zu triangles, %.0f mm thick (%.0f ms). Keep painting, or sculpt / comb, then GENERATE.", s.clay.idx.size() / 3, s.clayHeight, NowMs() - t0);
    s.status = b;
}

// Saves the clay into the "sculpt" style and runs clay_groom.py in the background (cards from clay + comb, then cap.py on this clay);
// Tick puts the style on the head when it finishes.
void ClayGenerate() {
    State& s = S();
    if (s.genProc) { s.status = "Hair Maker: still growing the last one."; return; }
    if (s.clay.Empty()) { s.status = "Hair Maker: load the clay first."; return; }
    const std::wstring dir = StyleDir("sculpt");
    CreateDirectoryW((ToolsDir() + L"\\..\\Assets\\hair\\styles").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!s.clay.Save(dir + L"clay.json")) { s.status = "Hair Maker: cannot write " + Narrow(dir) + "clay.json"; return; }
    const std::wstring script = ToolsDir() + L"\\hair_groom\\clay_groom.py";
    std::wstring cmd = L"cmd.exe /c python \"" + script + L"\" --clay \"" + dir + L"clay.json\" --out \"" + dir + L".\" > \"" + dir + L"generate.log\" 2>&1";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, (ToolsDir() + L"\\hair_groom").c_str(), &si, &pi)) {
        s.status = "Hair Maker: could not start python (" + std::to_string(GetLastError()) + ").";
        return;
    }
    CloseHandle(pi.hThread);
    s.genProc = pi.hProcess;
    s.genStart = NowMs();
    s.status = "Hair Maker: growing cards inside the clay along the comb...";
    Log("Character: Hair Maker generate started");
}
void ClayPoll() {
    State& s = S();
    if (!s.genProc || WaitForSingleObject(s.genProc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 1;
    GetExitCodeProcess(s.genProc, &code);
    CloseHandle(s.genProc);
    s.genProc = nullptr;
    if (code != 0) { s.status = "Hair Maker failed (see Assets\\hair\\styles\\sculpt\\generate.log)."; Log("Character: %s", s.status.c_str()); Redraw(); return; }
    if (std::find(s.grooms.begin(), s.grooms.end(), "sculpt") == s.grooms.end()) s.grooms.push_back("sculpt");
    const double took = NowMs() - s.genStart;
    s.groom.clear();
    ApplyGroom("sculpt");
    s.clayShowHair = true;
    char b[160];
    snprintf(b, sizeof(b), "Hair Maker: sculpt style grown in %.1f s and put on the head (Hair shown).", took / 1000.0);
    s.status = b;
    Log("Character: %s", b);
    Redraw();
}

// ---------------------------------------------------------------- skin tone
// The skin buttons only recolour the skin: every face, body and hair edit stays. The preset's "tone" (0 pale .. 1 deep, skin_tex's
// tone ramp) against the base skin's (0.355, #CF9678) gives a per-channel gain; lips, gums and eye corners (red with green below
// blue) keep their colour.
void ToneRamp(float t, float out[3]) {
    static const float ramp[7][3] = {{238, 208, 190}, {226, 186, 160}, {200, 156, 124}, {176, 140, 98}, {160, 114, 80}, {114, 76, 52}, {70, 46, 34}};
    const float x = std::clamp(t, 0.0f, 1.0f) * 6.0f;
    const int a = std::min(5, (int)x);
    const float f = x - (float)a;
    for (int c = 0; c < 3; ++c) out[c] = ramp[a][c] + (ramp[a + 1][c] - ramp[a][c]) * f;
}
void ToneAlbedo(std::vector<uint8_t>& out, const std::vector<uint8_t>& base) {
    const State& s = S();
    out = base;
    if (s.toneGain[0] == 1 && s.toneGain[1] == 1 && s.toneGain[2] == 1) return;
    uint8_t lut[3][256];
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v) { // in linear light, so darker tones stay saturated
            const float l = std::pow(v / 255.0f, 2.2f) * std::pow(s.toneGain[c], 2.2f);
            lut[c][v] = (uint8_t)std::clamp(std::pow(std::min(l, 1.0f), 1 / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    for (size_t i = 0; i + 3 < out.size(); i += 4) {
        if (base[i + 1] < base[i + 2] * 1.02f && base[i] > base[i + 1] + 40) continue; // mucosa
        out[i] = lut[0][base[i]]; out[i + 1] = lut[1][base[i + 1]]; out[i + 2] = lut[2][base[i + 2]];
    }
}
// Torso and legs: their own skin albedo, recoloured and swapped in (the head goes through the hair bake).
void ToneBody() {
    State& s = S();
    const char* stems[2] = {"body_torso", "body_legs"};
    for (int k = 0; k < 2; ++k) {
        const int i = FindEntity(stems[k]);
        if (i < 0) continue;
        const ImportedModel* model = s.host.entityModel(i);
        if (!model) continue;
        int img = -1;
        for (const ImportedPart& p : model->parts) if (p.skin) { img = p.albedo; break; }
        if (img < 0 || img >= (int)model->images.size()) continue;
        std::vector<uint8_t> base, px;
        int w = 0, h = 0;
        if (!DecodeImage(model->images[(size_t)img].data(), model->images[(size_t)img].size(), base, w, h)) continue;
        ToneAlbedo(px, base);
        const TextureId cur = s.host.skinAlbedo(i);
        if (cur == kNoTexture) continue;
        if (s.bodyTex[k] == kNoTexture || cur != s.bodyTex[k]) s.bodyOrig[k] = cur;
        const TextureId tex = s.host.createTexture(px.data(), w, h);
        s.host.swapAlbedo(i, cur, tex);
        if (s.bodyTex[k] != kNoTexture && s.bodyTex[k] != tex) s.host.destroyTexture(s.bodyTex[k]);
        s.bodyTex[k] = tex;
    }
}

bool InitHair() {
    State& s = S();
    if (s.hairReady) return true;
    if (!BindHead()) return false;
    const ImportedModel* model = s.host.entityModel(s.target);
    if (!model) return false;
    std::vector<std::pair<size_t, size_t>> skin;
    int albedoImage = -1;
    for (const ImportedPart& p : model->parts)
        if (p.skin) { skin.push_back({p.first, p.count}); if (albedoImage < 0) albedoImage = p.albedo; }
    Json doc;
    MorphSet::ReadJson(s.facePath, doc, nullptr);
    const bool keep = s.hair.Ready(); // a new skin preset or a rebind keeps what was painted
    std::vector<float> d = s.hair.density, fx = s.hair.fx, fy = s.hair.fy, len = s.hair.len;
    if (!s.hair.Init(s.head.Rest(), skin, doc.Find("landmarks"))) { s.status = "Hair: the head has no skin triangles."; return false; }
    if (keep && d.size() == s.hair.density.size()) { s.hair.density = d; s.hair.fx = fx; s.hair.fy = fy; s.hair.len = len; s.hair.painted = true; }
    // the head's own albedo (after the importer's beard bake), the source of every bake
    s.albedo.clear();
    if (albedoImage >= 0 && albedoImage < (int)model->images.size())
        DecodeImage(model->images[(size_t)albedoImage].data(), model->images[(size_t)albedoImage].size(), s.albedo, s.albedoW, s.albedoH);
    s.baseAlbedo = s.albedo;
    ToneAlbedo(s.albedo, s.baseAlbedo);
    s.albedoPath = s.host.entityPath(s.target);
    if (s.tile.empty()) {
        std::vector<uint8_t> png;
        const std::wstring tilePath = ToolsDir() + L"\\..\\testproject\\WesternGame\\Assets\\characters\\hair_tiles\\short_strands_01.png";
        if (ReadBytes(tilePath, png)) DecodeImage(png.data(), png.size(), s.tile, s.tileW, s.tileH);
        if (s.tile.empty()) Log("Character: hair strand tile not found at %ls", tilePath.c_str());
    }
    s.hairReady = !s.albedo.empty();
    if (!s.hairReady) s.status = "Hair: cannot read the head's skin albedo.";
    return s.hairReady;
}

// ---------------------------------------------------------------- feature view
// Like a rig's region view: each skin vertex takes the face-maker group (Jaw, Nose, Eyes, Brows, Mouth, Ears) whose sliders move
// it most ("Overall" is left out: it moves everything). Rasterised over the head's UVs, each texel takes its triangle's nearest
// corner's group; region borders are drawn as solid rings and every triangle edge as a dark wire, so paint lands where the rig
// expects it (paint outside a ring and that region's morph stretches it). Composited after the hair bake, so 3D and 2D both show it.
COLORREF FeatureColor(const std::string& g) {
    if (g == "Brows") return RGB(230, 200, 90); // the forehead the brow sliders move
    if (g == "Eyes") return RGB(150, 214, 150);
    if (g == "Nose") return RGB(236, 226, 200);
    if (g == "Mouth") return RGB(214, 150, 214);
    if (g == "Jaw") return RGB(240, 176, 96);
    if (g == "Ears") return RGB(235, 235, 235);
    return RGB(120, 150, 210);
}
bool BuildFeatures() {
    State& s = S();
    if (!s.featRegion.empty() && s.featRegion.size() == (size_t)s.albedoW * s.albedoH) return true;
    if (!InitHair() || !s.face.Valid() || !s.head.Bound()) return false;
    const ImportedModel* model = s.host.entityModel(s.target);
    if (!model) return false;
    const size_t G = s.face.groups.size(), NV = (size_t)s.face.vertexCount;
    std::vector<float> w(G * NV, 0.0f);
    for (const MorphSlider& m : s.face.sliders) {
        const size_t g = (size_t)(std::find(s.face.groups.begin(), s.face.groups.end(), m.group) - s.face.groups.begin());
        if (g >= G || m.group == "Overall") continue;
        for (size_t k = 0; k < m.idx.size(); ++k) {
            const float* d = &m.d[k * 3];
            w[g * NV + m.idx[k]] += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
    }
    std::vector<int8_t> vr(NV, -1);
    for (size_t v = 0; v < NV; ++v) {
        float best = 0.0006f; // under 0.6 mm at full slider: no group
        for (size_t g = 0; g < G; ++g) if (w[g * NV + v] > best) { best = w[g * NV + v]; vr[v] = (int8_t)g; }
    }
    const int W = s.albedoW, H = s.albedoH;
    s.featRegion.assign((size_t)W * H, -2);
    s.featWire.assign((size_t)W * H, 0);
    const std::vector<Vertex>& rest = s.head.Rest();
    const std::vector<uint32_t>& map = s.head.FlatToGlb();
    // the eyebrow hair zone: the arch brow_cards.py and the hair painter grow brows along (morphs.json landmarks, mesh space)
    Json doc;
    MorphSet::ReadJson(s.facePath, doc, nullptr);
    const Json* lmj = doc.Find("landmarks");
    const Json* by = lmj ? lmj->Find("brow_y") : nullptr;
    const float browY = by && by->type == Json::Number ? (float)by->n : 0.829f;
    const auto inBrow = [&](float x, float y, float z) {
        if (z < 0.10f) return false;
        const float t = (std::fabs(x) - 0.011f) / 0.050f;
        if (t < -0.02f || t > 1.04f) return false;
        const float c = std::clamp(t, 0.0f, 1.0f);
        const float centre = browY + 0.002f + 0.006f * std::sin(3.14159f * std::min(1.0f, c * 1.15f)) - 0.004f * c;
        const float half = (c > 0.15f ? 0.0045f * (1 - c) + 0.0014f * c : 0.0035f + 0.007f * c) + 0.0012f;
        return std::fabs(y - centre) < half;
    };
    const auto line = [&](float ax, float ay, float bx, float by) {
        const int n = (int)std::ceil(std::max(std::fabs(bx - ax), std::fabs(by - ay))) + 1;
        for (int i = 0; i <= n; ++i) {
            const int x = (int)(ax + (bx - ax) * i / n), y = (int)(ay + (by - ay) * i / n);
            if (x >= 0 && y >= 0 && x < W && y < H) s.featWire[(size_t)y * W + x] |= 1;
        }
    };
    for (const ImportedPart& p : model->parts) {
        if (!p.skin) continue;
        for (size_t t = p.first; t + 2 < p.first + p.count && t + 2 < rest.size(); t += 3) {
            float X[3], Y[3];
            int8_t R[3];
            for (int k = 0; k < 3; ++k) {
                X[k] = rest[t + k].u * W; Y[k] = rest[t + k].v * H;
                const uint32_t g = t + k < map.size() ? map[t + k] : UINT32_MAX;
                R[k] = g < NV ? vr[g] : (int8_t)-1;
            }
            const float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
            if (std::fabs(area) < 1e-9f) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({X[0], X[1], X[2]}))), x1 = std::min(W - 1, (int)std::ceil(std::max({X[0], X[1], X[2]})));
            const int y0 = std::max(0, (int)std::floor(std::min({Y[0], Y[1], Y[2]}))), y1 = std::min(H - 1, (int)std::ceil(std::max({Y[0], Y[1], Y[2]})));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float b0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
                    const float b1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
                    const float b2 = 1 - b0 - b1;
                    if (b0 < -0.01f || b1 < -0.01f || b2 < -0.01f) continue;
                    const int c = b0 >= b1 && b0 >= b2 ? 0 : (b1 >= b2 ? 1 : 2);
                    s.featRegion[(size_t)y * W + x] = R[c];
                    const Vertex &A = rest[t], &B = rest[t + 1], &C = rest[t + 2];
                    if (inBrow(A.px * b0 + B.px * b1 + C.px * b2, A.py * b0 + B.py * b1 + C.py * b2, A.pz * b0 + B.pz * b1 + C.pz * b2)) s.featWire[(size_t)y * W + x] |= 2;
                }
            for (int k = 0; k < 3; ++k) line(X[k], Y[k], X[(k + 1) % 3], Y[(k + 1) % 3]);
        }
    }
    Log("Character: feature view built (%zu groups, %dx%d)", G, W, H);
    return true;
}
void ApplyFeatures(std::vector<uint8_t>& img) {
    State& s = S();
    if (!s.features || !BuildFeatures()) return;
    const int W = s.albedoW, H = s.albedoH, ring = std::max(2, W / 512);
    if (img.size() < (size_t)W * H * 4) return;
    std::vector<COLORREF> col(s.face.groups.size());
    for (size_t g = 0; g < col.size(); ++g) col[g] = FeatureColor(s.face.groups[g]);
    const int off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            const int r = s.featRegion[i];
            if (r == -2) continue;
            const COLORREF c = r >= 0 ? col[(size_t)r] : RGB(120, 150, 210);
            bool border = false;
            for (int d = 1; d <= ring && !border; ++d)
                for (const auto& o : off) {
                    const int xx = x + o[0] * d, yy = y + o[1] * d;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    const int q = s.featRegion[(size_t)yy * W + xx];
                    if (q != -2 && q != r && (r >= 0 || q >= 0)) { border = true; break; }
                }
            const bool brow = (s.featWire[i] & 2) != 0;
            bool browEdge = false; // the eyebrow zone's own ring: drawn on top of everything, 2 texels wider
            for (int d = 1; d <= ring + 2 && !browEdge; ++d)
                for (const auto& o : off) {
                    const int xx = x + o[0] * d, yy = y + o[1] * d;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    if (((s.featWire[(size_t)yy * W + xx] & 2) != 0) != brow && s.featRegion[(size_t)yy * W + xx] != -2) { browEdge = true; break; }
                }
            if (browEdge || brow) {
                uint8_t* q = &img[i * 4];
                const float a2 = browEdge ? 1.0f : 0.25f; // inside: light, so the painted brow stays readable
                const float k2[3] = {255, 40, 200};
                for (int k = 0; k < 3; ++k) q[k] = (uint8_t)std::clamp(q[k] + (k2[k] - q[k]) * a2, 0.0f, 255.0f);
                continue;
            }
            const COLORREF bc = border && r < 0 ? RGB(250, 250, 250) : c; // the outer side of a ring: the region's colour from inside
            uint8_t* p = &img[i * 4];
            const float a = border ? 1.0f : (r >= 0 ? 0.45f : 0.3f);
            const float rgb[3] = {(float)GetRValue(bc), (float)GetGValue(bc), (float)GetBValue(bc)};
            for (int k = 0; k < 3; ++k) {
                float v = p[k] + (rgb[k] - p[k]) * a;
                if ((s.featWire[i] & 1) && !border) v *= 0.55f;
                p[k] = (uint8_t)std::clamp(v, 0.0f, 255.0f);
            }
        }
}

// ---------------------------------------------------------------- skin paint: shine surface, eye texture, maps viewer
const char* kMapNames[] = {"Final albedo", "Hair density", "Hair flow", "Skin tint", "Dark spots", "Shine"};
constexpr int kMapCount = 6;

// The painted shine goes into a copy of the skin surface map (red = oil shine, low red = glossier), swapped in for the head's own.
void BakeSurface() {
    State& s = S();
    s.surfDirty = false;
    if (s.target < 0 || !s.host.skinSurface || !s.host.skinSurfaceImage || !s.host.swapSurface) return;
    const TextureId cur = s.host.skinSurface(s.target);
    if (cur == kNoTexture) return;
    if (s.baseSurface.empty() && !s.host.skinSurfaceImage(s.target, &s.baseSurface, &s.surfW, &s.surfH)) { s.baseSurface.clear(); return; }
    if (!s.sp.shinePainted) { // nothing painted: the head's own map is back
        if (s.surfTex != kNoTexture) {
            if (cur == s.surfTex && s.surfOrig != kNoTexture) s.host.swapSurface(s.target, cur, s.surfOrig);
            s.host.destroyTexture(s.surfTex);
            s.surfTex = kNoTexture;
        }
        return;
    }
    std::vector<uint8_t> out;
    s.sp.BuildSurface(s.baseSurface, s.surfW, s.surfH, out);
    const TextureId tex = s.host.createTexture(out.data(), s.surfW, s.surfH);
    if (tex == kNoTexture) return;
    if (s.surfTex == kNoTexture || cur != s.surfTex) s.surfOrig = cur;
    s.host.swapSurface(s.target, cur, tex);
    if (s.surfTex != kNoTexture && s.surfTex != tex) s.host.destroyTexture(s.surfTex);
    s.surfTex = tex;
}

bool InitEye() {
    State& s = S();
    if (!s.eyeBase.empty()) return true;
    if (!BindHead()) return false;
    const ImportedModel* model = s.host.entityModel(s.target);
    if (!model) return false;
    for (const ImportedPart& p : model->parts)
        if (p.eye && p.albedo >= 0 && p.albedo < (int)model->images.size()) {
            if (DecodeImage(model->images[(size_t)p.albedo].data(), model->images[(size_t)p.albedo].size(), s.eyeBase, s.eyeBaseW, s.eyeBaseH) && s.eyeBaseW >= 8) {
                s.eyeFirst = p.first;
                s.eyeFound = true;
                return true;
            }
        }
    s.eyeBase.clear();
    s.status = "Eyes: the head has no eye texture.";
    return false;
}

// The eye texture from the sliders (1024 x 1024, rendered on the CPU), swapped in for the eye parts' albedo. All defaults: the original back.
void BakeEye() {
    State& s = S();
    s.eyeDirty = false;
    s.eyeLastBake = NowMs();
    if (!InitEye() || !s.host.partAlbedo) { Log("Character: eye bake: no eye texture in the head model"); return; }
    const TextureId cur = s.host.partAlbedo(s.target, s.eyeFirst);
    if (cur == kNoTexture) { Log("Character: eye bake: no drawn albedo for the eye part at vertex %zu", s.eyeFirst); return; }
    if (s.eye.IsDefault()) {
        if (s.eyeTex != kNoTexture) {
            if (cur == s.eyeTex && s.eyeOrig != kNoTexture) s.host.swapAlbedo(s.target, cur, s.eyeOrig);
            s.host.destroyTexture(s.eyeTex);
            s.eyeTex = kNoTexture;
        }
        return;
    }
    std::vector<uint8_t> out;
    RenderEye(s.eyeBase, s.eyeBaseW, s.eyeBaseH, s.eye, 1024, out);
    const TextureId tex = s.host.createTexture(out.data(), 1024, 1024);
    if (tex == kNoTexture) return;
    if (s.eyeTex == kNoTexture || cur != s.eyeTex) s.eyeOrig = cur;
    s.host.swapAlbedo(s.target, cur, tex);
    if (s.eyeTex != kNoTexture && s.eyeTex != tex) s.host.destroyTexture(s.eyeTex);
    s.eyeTex = tex;
}

// What the 2D view draws for the chosen map, at the albedo's size, with the feature overlay on top (so rig regions show on every map).
void BuildMapImg() {
    State& s = S();
    s.mapImg.clear();
    if (s.mapView <= 0 || s.albedoW <= 0) return;
    const int W = s.albedoW, H = s.albedoH, N = HairPainter::N;
    const std::vector<uint8_t>* under = s.albedo.size() >= (size_t)W * H * 4 ? &s.albedo : nullptr;
    if (s.mapView == 1 || s.mapView == 2) {
        s.mapImg.assign((size_t)W * H * 4, 255);
        if (!s.hair.Ready()) return;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t m = (size_t)std::min(N - 1, (int)((long long)y * N / H)) * N + (size_t)std::min(N - 1, (int)((long long)x * N / W));
                uint8_t* o = &s.mapImg[((size_t)y * W + (size_t)x) * 4];
                if (s.mapView == 1) { const uint8_t v = (uint8_t)(std::clamp(s.hair.density[m], 0.0f, 1.0f) * 255.0f); o[0] = (uint8_t)(under ? (*under)[((size_t)y * W + x) * 4] * 0.25f + v * 0.75f : v); o[1] = (uint8_t)(v * 0.75f + 10); o[2] = (uint8_t)(v * 0.6f + 10); }
                else { o[0] = (uint8_t)((s.hair.fx[m] * 0.5f + 0.5f) * 255.0f * s.hair.density[m]); o[1] = (uint8_t)((s.hair.fy[m] * 0.5f + 0.5f) * 255.0f * s.hair.density[m]); o[2] = (uint8_t)(s.hair.len[m] * 200.0f * s.hair.density[m]); }
            }
    } else {
        s.sp.MapImage(s.mapView - 3, W, H, under, s.mapImg);
    }
    ApplyFeatures(s.mapImg);
}

// ---------------------------------------------------------------- brow maker
// The brow sliders: one table drives the panel rows, dragging, saving and loading. shape: rebuilds the default loop.
struct BrowSlider { const char* key; const wchar_t* label; float BrowMaker::*field; float lo, hi; bool shape; const wchar_t* fmt; };
const BrowSlider kBrowSliders[] = {
    {"density", L"Density", &BrowMaker::density, 0.3f, 2.5f, false, L"%.2f"},
    {"length", L"Hair length", &BrowMaker::length, 0.5f, 1.8f, false, L"%.2f"},
    {"thickness", L"Thickness", &BrowMaker::thick, 0.4f, 2.2f, true, L"%.2f"},
    {"arch", L"Arch height", &BrowMaker::arch, 0.0f, 2.5f, true, L"%.2f"},
    {"peak", L"Arch peak (in / out)", &BrowMaker::peak, -1.0f, 1.0f, true, L"%+.2f"},
    {"tail_drop", L"Tail drop mm", &BrowMaker::tailDrop, -3.0f, 6.0f, true, L"%+.1f"},
    {"taper", L"Tail taper", &BrowMaker::taper, 0.0f, 2.0f, true, L"%.2f"},
    {"gap", L"Inner gap mm", &BrowMaker::gap, -4.0f, 6.0f, true, L"%+.1f"},
    {"span", L"Span", &BrowMaker::span, 0.7f, 1.25f, true, L"%.2f"},
    {"upright", L"Flat / brushed up", &BrowMaker::upright, -1.0f, 1.0f, false, L"%+.2f"},
    {"messy", L"Messiness", &BrowMaker::messy, 0.0f, 1.0f, false, L"%.2f"},
    {"soft", L"Edge softness", &BrowMaker::soft, 0.3f, 2.5f, false, L"%.2f"},
    {"dark", L"Darkness", &BrowMaker::dark, 0.5f, 1.8f, false, L"%.2f"},
    {"warm", L"Ash / auburn", &BrowMaker::warm, -1.0f, 1.0f, false, L"%+.2f"},
};
constexpr int kBrowSliderCount = (int)(sizeof(kBrowSliders) / sizeof(kBrowSliders[0]));
bool IsBrowSlider(int a) { return a >= A_BrowDensity && a < A_BrowDensity + kBrowSliderCount; }

// The head's brow-card part (the importer's brow_cards.json: 10 cards x 36 vertices): where it starts and which hair part it is.
bool BrowPart() {
    State& s = S();
    const ImportedModel* model = s.host.entityModel(s.target);
    if (!model) return false;
    size_t ordinal = 0;
    for (const ImportedPart& p : model->parts) {
        if (!p.hair) continue;
        if (p.brow) {
            if (p.count != (size_t)2 * BrowMaker::kCards * BrowMaker::kVertsPerCard) { s.browStatus = "the head's brow cards are not the 10-card layout"; return false; }
            s.browFirst = p.first; s.browOrdinal = ordinal;
            return true;
        }
        ++ordinal;
    }
    s.browStatus = "the head has no brow cards (Assets\\hair\\wyatt\\brow_cards.json)";
    return false;
}
// Re-applies a baked brow texture when the head's mesh was made again (a skin preset reload) and the swap was lost.
void SyncBrowTexture() {
    State& s = S();
    if (s.browTex == kNoTexture || s.target < 0 || !s.host.partAlbedo) return;
    const TextureId cur = s.host.partAlbedo(s.target, SIZE_MAX - s.browOrdinal);
    if (cur != kNoTexture && cur != s.browTex) s.host.swapAlbedo(s.target, cur, s.browTex);
}
void BakeBrows(bool preview = false) {
    State& s = S();
    if (!InitHair() || !BrowPart()) { s.status = "Brows: " + s.browStatus; return; }
    Json doc;
    MorphSet::ReadJson(s.facePath, doc, nullptr);
    if (const Json* lm = doc.Find("landmarks")) if (const Json* by = lm->Find("brow_y"); by && by->type == Json::Number) s.brow.browY = (float)by->n;
    if (s.brow.loop.size() < 6 || !s.brow.custom) s.brow.DefaultLoop(); // the default loop always follows the style sliders
    const std::vector<Vertex>& rest = s.head.Rest();
    // only the skin around the subject's left brow (the bake works there and mirrors): a few hundred triangles instead of the
    // whole head, so the thousands of rays a bake casts stay cheap
    std::vector<Vertex> tris;
    if (const ImportedModel* model = s.host.entityModel(s.target))
        for (const ImportedPart& part : model->parts) {
            if (!part.skin) continue;
            for (size_t k = part.first; k + 2 < part.first + part.count && k + 2 < rest.size(); k += 3) {
                const Vertex &A = rest[k], &B = rest[k + 1], &C = rest[k + 2];
                const float mx = std::max({A.px, B.px, C.px}), my = std::max({A.py, B.py, C.py}), ny = std::min({A.py, B.py, C.py}), mz = std::max({A.pz, B.pz, C.pz});
                if (mx < -0.004f || mx > 0.11f || my < s.brow.browY - 0.04f || ny > s.brow.browY + 0.05f || mz < 0.04f) continue;
                tris.push_back(A); tris.push_back(B); tris.push_back(C);
            }
        }
    const BrowMaker::Cast cast = [&](const float o[3], const float d[3], float p[3], float n[3]) { // nearest hit, normal facing the ray
        float best = 1e30f;
        bool hit = false;
        for (size_t k = 0; k + 2 < tris.size(); k += 3) {
            const Vertex &A = tris[k], &B = tris[k + 1], &C = tris[k + 2];
            const float e1[3] = {B.px - A.px, B.py - A.py, B.pz - A.pz}, e2[3] = {C.px - A.px, C.py - A.py, C.pz - A.pz};
            const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::fabs(det) < 1e-14f) continue;
            const float inv = 1.0f / det, tv[3] = {o[0] - A.px, o[1] - A.py, o[2] - A.pz};
            const float bu = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (bu < 0 || bu > 1) continue;
            const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
            const float bv = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
            if (bv < 0 || bu + bv > 1) continue;
            const float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
            if (t <= 0 || t >= best) continue;
            best = t; hit = true;
            for (int c = 0; c < 3; ++c) p[c] = o[c] + d[c] * t;
            float nn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (l < 1e-12f) continue;
            const float sg = (nn[0] * d[0] + nn[1] * d[1] + nn[2] * d[2]) > 0 ? -1.0f : 1.0f;
            for (int c = 0; c < 3; ++c) n[c] = nn[c] / l * sg;
        }
        return hit;
    };
    const double t0 = NowMs();
    std::string why;
    if (!s.brow.Bake(cast, why, preview)) { s.status = "Brows: " + why; return; }
    const double t1 = NowMs();
    const Vertex importedCorner = rest[s.browFirst]; // before it is replaced (debug log below)
    s.head.SetRest(s.browFirst, s.brow.cards);
    { // debug: TBD_CHARBROWDUMP=<file.png> writes the baked texture; the first card's corners go to the log
        wchar_t dump[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"TBD_CHARBROWDUMP", dump, MAX_PATH) > 0) WritePngRgba(dump, s.brow.tex.data(), s.brow.texW, s.brow.texH);
        const Vertex& c0 = s.brow.cards[0];
        const Vertex& o0 = importedCorner;
        size_t cover = 0;
        for (size_t i = 3; i < s.brow.tex.size(); i += 4) cover += s.brow.tex[i] > 100;
        Log("Character: brow card 0 corner %.4f %.4f %.4f (the imported one %.4f %.4f %.4f), texture coverage %.2f%%", c0.px, c0.py, c0.pz, o0.px, o0.py, o0.pz,
            100.0 * cover / std::max<size_t>(1, s.brow.tex.size() / 4));
    }
    if (GetEnvironmentVariableA("TBD_CHARBROWSOLID", nullptr, 0) > 0) for (size_t i = 0; i < s.brow.tex.size(); i += 4) { s.brow.tex[i] = 255; s.brow.tex[i + 1] = 0; s.brow.tex[i + 2] = 255; s.brow.tex[i + 3] = 255; } // debug: the cards as solid magenta
    const TextureId tex = s.host.createTexture(s.brow.tex.data(), s.brow.texW, s.brow.texH);
    // the texture on the brow cards now: the one this creator put there last, else the head's own (asked once)
    TextureId cur = s.browTex;
    if (cur == kNoTexture && s.host.partAlbedo) cur = s.host.partAlbedo(s.target, SIZE_MAX - s.browOrdinal);
    if (cur != kNoTexture) s.host.swapAlbedo(s.target, cur, tex);
    if (s.browTex != kNoTexture && s.browTex != tex) s.host.destroyTexture(s.browTex);
    s.browTex = tex;
    s.browBaked = true;
    s.faceDirty = true; // the head goes back up with the new cards
    char b[200];
    snprintf(b, sizeof(b), "Brows %s: %d hairs into 10 cards, %.0f ms (+ texture %dx%d)%s", preview ? "preview" : "baked", s.brow.hairCount, t1 - t0,
             s.brow.texW, s.brow.texH, cur == kNoTexture ? " - card texture not found, cards only" : "");
    s.status = b;
    if (!preview) Log("Character: %s", b);
    s.browFull = preview; // a preview is followed by the full bake once the drag ends
}

void BakeHair() {
    State& s = S();
    const bool strokeEnded = s.bakeNow && !s.stroking;
    s.hairDirty = s.bakeNow = false;
    if (strokeEnded && s.clayMaskMode) ClayFromMask(); // the Hair Maker's clay follows the painted scalp
    if (!InitHair()) return;
    const double t0 = NowMs();
    std::vector<uint8_t> out, painted;
    const std::vector<uint8_t>* src = &s.albedo;
    if (s.sp.tintPainted || s.sp.spotsPainted || s.sp.gainPainted) { painted = s.albedo; s.sp.Composite(painted, s.albedoW, s.albedoH); src = &painted; } // paint under the strands
    s.hair.Bake(*src, s.albedoW, s.albedoH, s.tile, s.tileW, s.tileH, out);
    ApplyFeatures(out);
    BuildMapImg();
    if (s.surfDirty) BakeSurface();
    const double t1 = NowMs();
    wchar_t dump[MAX_PATH] = {}; // TBD_CHARHAIRDUMP=<file.png>: the baked albedo, to check the bake without the cards over it
    if (GetEnvironmentVariableW(L"TBD_CHARHAIRDUMP", dump, MAX_PATH) > 0) WritePngRgba(dump, out.data(), s.albedoW, s.albedoH);
    const TextureId tex = s.host.createTexture(out.data(), s.albedoW, s.albedoH);
    s.shown = out;
    ++s.albedoStamp;
    s.bgraDirty = true;
    if (s.wnd2d) InvalidateRect(s.wnd2d, nullptr, FALSE);
    const TextureId cur = s.host.skinAlbedo(s.target);
    if (s.bakedTex == kNoTexture || cur != s.bakedTex) s.origTex = cur;
    s.host.swapAlbedo(s.target, cur, tex);
    if (s.bakedTex != kNoTexture && s.bakedTex != tex) s.host.destroyTexture(s.bakedTex);
    s.bakedTex = tex;
    s.bakeMs = t1 - t0;
    s.texMs = NowMs() - t1;
    char b[160];
    snprintf(b, sizeof(b), "Hair baked %dx%d: composite %.1f ms, texture upload %.1f ms", s.albedoW, s.albedoH, s.bakeMs, s.texMs);
    s.status = b;
    Log("Character: %s", b);
}

void ClearHair() {
    State& s = S();
    s.hair.Clear();
    if (s.bakedTex != kNoTexture && s.target >= 0) {
        const TextureId cur = s.host.skinAlbedo(s.target);
        if (cur == s.bakedTex && s.origTex != kNoTexture) s.host.swapAlbedo(s.target, cur, s.origTex);
        s.host.destroyTexture(s.bakedTex);
    }
    s.bakedTex = kNoTexture;
    s.shown.clear();
    s.bgraDirty = true;
    if (s.wnd2d) InvalidateRect(s.wnd2d, nullptr, FALSE);
    s.status = "Hair cleared: the head's own albedo is back.";
}

// One brush dab on the active tool: the hair tools paint the hair maps, the skin layer tools (SkinPaint) paint tint, spots or shine.
void DoDab(const HairHit& h, float du, float dv) {
    State& s = S();
    if (SkinPaint::IsLayerTool(s.hairTool)) {
        s.sp.Dab(s.hairTool, h, s.hairRadius, s.hairStrength, s.hairHardness, s.tintType, s.spotKind);
        if (s.hairTool == SkinPaint::ShinePaint || s.hairTool == SkinPaint::ShineErase) s.surfDirty = true;
    } else
        s.hair.Dab((HairPainter::Tool)s.hairTool, h, s.hairRadius, s.hairStrength, du, dv, s.hairHardness);
}

// The same dab on the other side: the hit point mirrored across x = 0, found again on the drawn head along its mirrored normal.
// Comb's direction comes from the mirrored stroke's own UV steps (the two sides' UVs are not mirror images).
void MirrorDab(const HairHit& h, bool first) {
    State& s = S();
    const float o[3] = {-h.p[0] - h.n[0] * 0.02f, h.p[1] + h.n[1] * 0.02f, h.p[2] + h.n[2] * 0.02f}; // mirrored point, lifted off along the mirrored normal
    const float d[3] = {h.n[0], -h.n[1], -h.n[2]}; // minus the mirrored normal (-n0, n1, n2): pointing in
    const HairHit m = s.hair.Raycast(o, d, s.headNow);
    if (!m.hit || std::fabs(m.p[0] + h.p[0]) > 0.01f) { s.lastMirror.hit = false; return; } // missed, or on the centre line
    if (std::fabs(h.p[0]) < s.hairRadius * 0.0005f) { s.lastMirror.hit = false; return; } // the brush already covers the centre
    float du = 0, dv = 0;
    if (!first && s.lastMirror.hit) { du = m.u - s.lastMirror.u; dv = m.v - s.lastMirror.v; if (std::sqrt(du * du + dv * dv) > 0.05f) du = dv = 0; }
    DoDab(m, du, dv);
    s.lastMirror = m;
}

// One brush sample on a mesh-space ray.
void HairDab(const float o[3], const float d[3], bool first) {
    State& s = S();
    const HairHit h = s.hair.Raycast(o, d, s.headNow);
    if (!h.hit) { s.lastHit.hit = false; return; }
    float du = 0, dv = 0;
    if (!first && s.lastHit.hit) {
        du = h.u - s.lastHit.u; dv = h.v - s.lastHit.v;
        if (std::sqrt(du * du + dv * dv) > 0.05f) du = dv = 0; // jumped across a UV seam
    }
    DoDab(h, du, dv);
    s.lastHit = h;
    if (s.symmetry) MirrorDab(h, first);
    s.hairDirty = true;
}

// TBD_CHARHAIRTEST=0: defaults only. 1: defaults, then thicker brows and a comb to a side part, by scripted strokes (mesh-space
// rays aimed at the surface: the same Raycast and Dab as a mouse stroke).
void HairTest(int level) {
    State& s = S();
    if (!InitHair()) return;
    const double t0 = NowMs();
    s.hair.FillDefaults();
    if (level >= 1) {
        Json doc;
        MorphSet::ReadJson(s.facePath, doc, nullptr);
        const Json* lm = doc.Find("landmarks");
        const auto num = [&](const char* k, float def) { const Json* j = lm ? lm->Find(k) : nullptr; return j && j->type == Json::Number ? (float)j->n : def; };
        const float browY = num("brow_y", 0.829f), top = num("head_top_y", 0.93f), front = num("face_front_z", 0.176f), back = num("head_back_z", -0.045f);
        const auto stroke = [&](int tool, float radius, float strength, const std::vector<std::array<float, 6>>& rays) {
            s.hairTool = tool; s.hairRadius = radius; s.hairStrength = strength;
            bool first = true;
            for (const auto& r : rays) { HairDab(&r[0], &r[3], first); first = false; }
        };
        for (const float side : {1.0f, -1.0f}) { // brows: paint along and just above the brow line, twice
            std::vector<std::array<float, 6>> rays;
            for (int k = 0; k <= 24; ++k) {
                const float t = (float)k / 24, x = side * (0.012f + 0.048f * t);
                rays.push_back({x, browY + 0.005f + 0.006f * std::sin(3.14159f * t) - 0.004f * t, front + 0.3f, 0, 0, -1});
            }
            stroke(HairPainter::Paint, 5.0f, 0.9f, rays);
            stroke(HairPainter::Paint, 5.0f, 0.9f, rays);
        }
        const float part = 0.028f; // a side part on the subject's left
        for (float z = front - 0.03f; z > back + 0.06f; z -= 0.008f) {
            std::vector<std::array<float, 6>> left, right, line;
            for (int k = 0; k <= 16; ++k) left.push_back({part + 0.004f + 0.07f * k / 16, top + 0.3f, z, 0, -1, 0});
            for (int k = 0; k <= 22; ++k) right.push_back({part - 0.004f - 0.10f * k / 22, top + 0.3f, z, 0, -1, 0});
            stroke(HairPainter::Comb, 12.0f, 0.85f, left);
            stroke(HairPainter::Comb, 12.0f, 0.85f, right);
            line.push_back({part, top + 0.3f, z, 0, -1, 0});
            stroke(HairPainter::Erase, 2.5f, 0.5f, line);
        }
        s.hairTool = HairPainter::None;
    }
    char hex[16] = {}; // TBD_CHARHAIRCOLOR=#rrggbb for the test
    if (GetEnvironmentVariableA("TBD_CHARHAIRCOLOR", hex, sizeof(hex)) > 0 && hex[0] == '#') {
        const unsigned v = (unsigned)strtoul(hex + 1, nullptr, 16);
        s.hair.color[0] = ((v >> 16) & 255) / 255.0f; s.hair.color[1] = ((v >> 8) & 255) / 255.0f; s.hair.color[2] = (v & 255) / 255.0f;
    }
    Log("Character: hair test %d painted in %.1f ms", level, NowMs() - t0);
    BakeHair();
}

// ---------------------------------------------------------------- applying shapes
void ApplyFace() {
    State& s = S();
    s.faceDirty = false;
    if (!BindHead()) return;
    const double t0 = NowMs();
    if (HeadNeutral() && !s.browBaked) {
        s.host.restoreMesh(s.target);
        s.headNow = s.head.Rest();
        s.deformMs = 0;
        s.uploadMs = NowMs() - t0;
        s.status = s.targetName + ": face as authored.";
        SyncHairTexture();
        return;
    }
    std::vector<Vertex> out;
    const bool neck = s.bodyLoaded && s.bodyHead.Valid();
    s.head.Deform(s.values, out, neck ? &s.bodyHead : nullptr, neck ? &s.bodyValues : nullptr);
    ApplyEyePlacement(out);
    const double t1 = NowMs();
    s.host.setMesh(s.target, out);
    s.deformMs = t1 - t0;
    s.uploadMs = NowMs() - t1;
    s.headNow = std::move(out);
    SyncHairTexture();
    SyncBrowTexture();
    Report(s.targetName, s.face.MaxDisplacementMm(s.values));
}

void ApplyBody() {
    State& s = S();
    s.bodyDirty = false;
    if (!s.bodyLoaded) return;
    double deform = 0, upload = 0;
    float mm = 0;
    std::string names;
    for (BodyPart& p : s.parts) {
        const int i = FindEntity(p.slot);
        if (i < 0) continue;
        if (p.target != i || p.name != s.host.entityName(i) || !p.morph.Bound()) {
            std::string why;
            if (!BindMorph(p.morph, p.set, i, why)) { s.status = "Cannot morph " + s.host.entityName(i) + ": " + why; p.target = -1; return; }
            p.target = i;
            p.name = s.host.entityName(i);
        }
        const double t0 = NowMs();
        if (Neutral(s.bodyValues)) { s.host.restoreMesh(i); upload += NowMs() - t0; continue; }
        std::vector<Vertex> out;
        p.morph.Deform(s.bodyValues, out);
        const double t1 = NowMs();
        s.host.setMesh(i, out);
        deform += t1 - t0;
        upload += NowMs() - t1;
        mm = std::max(mm, p.set.MaxDisplacementMm(s.bodyValues));
        names += (names.empty() ? "" : " + ") + p.name;
    }
    if (s.bodyHead.Valid()) s.faceDirty = true; // the neck lives on the head mesh, with the face
    if (s.heightScale != s.heightShown) {
        for (const char* stem : {"body_head", "body_torso", "body_legs"})
            if (const int i = FindEntity(stem); i >= 0) s.host.setEntityScale(i, s.heightScale);
        s.heightShown = s.heightScale;
    }
    s.deformMs = deform;
    s.uploadMs = upload;
    Report(names.empty() ? std::string("body") : names, mm);
}

// The torso and legs beside the head (same folder, same #native frame), when the scene has only the head.
void EnsureBody() {
    State& s = S();
    const int head = FindEntity("body_head");
    if (head < 0) return;
    const std::string hp = s.host.entityPath(head);
    const size_t slash = hp.find_last_of("\\/");
    const std::string dir = hp.substr(0, slash + 1), tag = hp.find("#native") != std::string::npos ? "#native" : "";
    for (const char* stem : {"body_torso", "body_legs"})
        if (FindEntity(stem) < 0 && Exists(Utf8ToWide(dir + stem + ".glb"))) {
            const int i = s.host.addEntity(head, std::string("Body ") + (stem[5] == 't' ? "Torso" : "Legs"), dir + stem + ".glb" + tag);
            Log("Character: added %s to the scene (entity %d)", stem, i);
        }
}

// Bounding box of the character in the head entity's mesh space, then the camera on it.
void FrameCamera() {
    State& s = S();
    const int head = FindEntity("body_head");
    if (head < 0) return;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const auto grow = [&](int i) {
        std::vector<Vertex> v;
        if (i < 0 || !s.host.entityMesh(i, &v)) return;
        for (const Vertex& x : v) {
            lo[0] = std::min(lo[0], x.px); lo[1] = std::min(lo[1], x.py); lo[2] = std::min(lo[2], x.pz);
            hi[0] = std::max(hi[0], x.px); hi[1] = std::max(hi[1], x.py); hi[2] = std::max(hi[2], x.pz);
        }
    };
    grow(head);
    if (s.tab == 0) { grow(FindEntity("body_torso")); grow(FindEntity("body_legs")); }
    if (s.tab == 2) { // the shirt and the waistband: from the head down to the thighs
        if (lo[0] > hi[0]) return;
        float mid[3] = {(lo[0] + hi[0]) * 0.5f, hi[1] - 0.42f, (lo[2] + hi[2]) * 0.5f}, world[3];
        s.host.toWorld(head, mid, world);
        s.host.frame(world, 1.55f * s.heightScale);
        return;
    }
    if (lo[0] > hi[0]) return;
    const float h = hi[1] - lo[1];
    float mid[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f}, world[3];
    if (s.tab == 1 || s.tab == 3) { // the face, not the neck; the orbit point on the face's front, so zooming in reaches the skin, not the inside
        mid[1] = lo[1] + h * 0.62f;
        mid[2] = hi[2] - (hi[2] - lo[2]) * 0.08f;
    }
    s.host.toWorld(head, mid, world);
    char zoom[16] = {}; // TBD_CHARZOOM=<x>: camera distance times x (close-ups for checking pores)
    const float zk = GetEnvironmentVariableA("TBD_CHARZOOM", zoom, sizeof(zoom)) > 0 ? std::clamp((float)atof(zoom), 0.1f, 4.0f) : 1.0f;
    s.host.frame(world, (s.tab == 0 ? h * 1.25f : h * 1.45f) * s.heightScale * zk);
}

void ApplySkin(int k) {
    State& s = S();
    if (k < 0 || k >= (int)s.skins.size()) return;
    const double t0 = NowMs();
    int changed = 0;
    s.skin = k;
    s.skinInfo.clear();
    Json doc;
    std::vector<uint8_t> bytes;
    const std::wstring info = s.skins[(size_t)k] == "Regular" ? RegularDir() + L"regular.skin.json"
                                                             : ToolsDir() + L"\\skin_tex\\out\\presets\\" + Utf8ToWide(s.skins[(size_t)k]) + L".skin.json";
    if (ReadBytes(info, bytes) &&
        JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc)) {
        std::string line;
        for (const auto& [key, v] : doc.o) {
            if (v.type != Json::Number || key.find("seed") != std::string::npos || key == "scar_count") continue;
            char b[48];
            snprintf(b, sizeof(b), "%s %.2f   ", key.c_str(), v.n);
            line += b;
            if (line.size() > 52) { s.skinInfo.push_back(line); line.clear(); }
        }
        if (!line.empty()) s.skinInfo.push_back(line);
    }
    // only the colour changes: the meshes, morphs and painted hair stay; the head re-bakes from its recoloured albedo
    float tone = 0.355f, ref[3], to[3];
    for (const auto& [key, v] : doc.o) if (key == "tone" && v.type == Json::Number) tone = (float)v.n;
    if (s.skins[(size_t)k] == "Regular") tone = 0.355f;
    ToneRamp(0.355f, ref);
    ToneRamp(tone, to);
    for (int c = 0; c < 3; ++c) s.toneGain[c] = to[c] / ref[c];
    if (InitHair()) { ToneAlbedo(s.albedo, s.baseAlbedo); BakeHair(); ++changed; }
    ToneBody();
    changed += (s.bodyTex[0] != kNoTexture) + (s.bodyTex[1] != kNoTexture);
    Json* skinBlock = nullptr;
    for (auto& kv : s.extra.o) if (kv.first == "skin") skinBlock = &kv.second;
    if (!skinBlock) { s.extra.type = Json::Object; s.extra.o.push_back({"skin", Json()}); skinBlock = &s.extra.o.back().second; skinBlock->type = Json::Object; }
    bool set = false;
    for (auto& kv : skinBlock->o) if (kv.first == "preset") { kv.second.type = Json::String; kv.second.s = s.skins[(size_t)k]; set = true; }
    if (!set) { Json v; v.type = Json::String; v.s = s.skins[(size_t)k]; skinBlock->o.push_back({"preset", v}); }
    char b[160];
    snprintf(b, sizeof(b), "Skin tone %s on %d parts, edits kept (%.0f ms).", s.skins[(size_t)k].c_str(), changed, NowMs() - t0);
    s.status = b;
    Log("Character: %s", s.status.c_str());
}

// ---------------------------------------------------------------- presets
Json JNum(double v) { Json j; j.type = Json::Number; j.n = v; return j; }
Json JStr(const std::string& v) { Json j; j.type = Json::String; j.s = v; return j; }
Json JObj() { Json j; j.type = Json::Object; return j; }

std::string HexColor(const float c[3]) {
    char b[16];
    snprintf(b, sizeof(b), "#%02x%02x%02x", (int)std::lround(c[0] * 255), (int)std::lround(c[1] * 255), (int)std::lround(c[2] * 255));
    return b;
}

bool LoadPreset(const std::wstring& path) {
    State& s = S();
    if (!LoadFace()) return false;
    CharacterPreset p;
    std::string err, warn;
    if (!p.Load(path, s.face, &err, &warn)) { s.status = "Could not load the preset: " + err; return false; }
    s.values = p.face;
    s.age = std::clamp(p.age, 0.0f, 1.0f);
    s.presetSeed = p.seed;
    s.presetName = p.name;
    s.extra = JObj();
    std::string skinName, groomName;
    const std::wstring folder = path.substr(0, path.find_last_of(L"\\/") + 1);
    for (const auto& kv : p.extra.o) {
        if (kv.first == "body") { // character_preset_body.schema.json: sliders, height_scale, seed
            PollBody();
            if (s.bodyLoaded) {
                std::fill(s.bodyValues.begin(), s.bodyValues.end(), 0.0f);
                if (const Json* vals = kv.second.Find("sliders"))
                    for (const auto& [n, v] : vals->o)
                        if (const int i = s.parts[0].set.Find(n); i >= 0) s.bodyValues[(size_t)i] = std::clamp((float)v.Num(), -1.0f, 1.0f);
                if (const Json* h = kv.second.Find("height_scale")) s.heightScale = std::clamp((float)h->Num(1.0), s.heightMin, s.heightMax);
                s.bodyPresetSeed = kv.second.Find("seed") ? (long long)kv.second.Find("seed")->Num() : -1;
                s.bodyDirty = true;
            } else s.extra.o.push_back(kv); // kept for when the body morphs exist
            continue;
        }
        if (kv.first == "brows") { // the brow maker's block: re-baked from the boundary and sliders (deterministic, same seed)
            const Json& b = kv.second;
            for (const BrowSlider& sl : kBrowSliders) if (const Json* v = b.Find(sl.key)) s.brow.*sl.field = std::clamp((float)v->Num(), sl.lo, sl.hi);
            if (const Json* v = b.Find("seed")) s.brow.seed = (uint32_t)v->Num(7);
            s.brow.custom = b.Find("custom") && b.Find("custom")->Num() > 0.5;
            s.brow.loop.clear();
            if (const Json* l = b.Find("loop"))
                for (const Json& q : l->a) if (q.a.size() == 2) s.brow.loop.push_back({(float)q.a[0].Num(), (float)q.a[1].Num()});
            BakeBrows();
            continue; // written back fresh on save
        }
        if (kv.first == "skin")
            if (const Json* pr = kv.second.Find("preset")) skinName = pr->s;
        if (kv.first == "hair")
            if (const Json* g = kv.second.Find("groom"); g && !g->s.empty()) groomName = g->s;
        if (kv.first == "hair")
            if (const Json* paint = kv.second.Find("paint")) { // {density, flow, color}, files next to the preset
                const Json* d = paint->Find("density");
                const Json* f = paint->Find("flow");
                if (const Json* c = paint->Find("color"); c && c->s.size() == 7) {
                    const unsigned v = (unsigned)strtoul(c->s.c_str() + 1, nullptr, 16);
                    s.hair.color[0] = ((v >> 16) & 255) / 255.0f; s.hair.color[1] = ((v >> 8) & 255) / 255.0f; s.hair.color[2] = (v & 255) / 255.0f;
                }
                if (d && f && InitHair()) {
                    if (s.hair.Load(folder + Utf8ToWide(d->s), folder + Utf8ToWide(f->s))) s.bakeNow = true;
                    else Log("Character: preset hair maps not readable (%s, %s)", d->s.c_str(), f->s.c_str());
                }
            }
        s.extra.o.push_back(kv);
    }
    ScanLooks();
    for (size_t k = 0; k < s.skins.size(); ++k)
        if (s.skins[k] == skinName && (int)k != s.skin) ApplySkin((int)k);
    if (!groomName.empty() && groomName != s.groom) ApplyGroom(groomName);
    s.faceDirty = true;
    if (!warn.empty()) Log("Character: %s", warn.c_str());
    return true;
}

bool SavePreset(const std::wstring& path) {
    State& s = S();
    CharacterPreset out;
    out.name = s.presetName;
    out.seed = s.presetSeed;
    out.age = s.age;
    out.face = s.values;
    out.extra = s.extra;
    out.extra.type = Json::Object;
    const auto put = [](Json& obj, const std::string& key, const Json& v) {
        for (auto& kv : obj.o) if (kv.first == key) { kv.second = v; return; }
        obj.o.push_back({key, v});
    };
    if (s.bodyLoaded) {
        Json body = JObj(), sliders = JObj(), meshes = JObj(), sums = JObj();
        meshes.o = {{"torso", JStr(s.parts[0].set.mesh)}, {"legs", JStr(s.parts[1].set.mesh)}};
        sums.o = {{"torso", JStr(s.parts[0].set.restChecksum)}, {"legs", JStr(s.parts[1].set.restChecksum)}};
        if (s.bodyHead.Valid()) { meshes.o.push_back({"head", JStr(s.bodyHead.mesh)}); sums.o.push_back({"head", JStr(s.bodyHead.restChecksum)}); }
        body.o.push_back({"base_meshes", meshes});
        body.o.push_back({"rest_checksums", sums});
        if (s.bodyPresetSeed >= 0) body.o.push_back({"seed", JNum((double)s.bodyPresetSeed)});
        body.o.push_back({"height_scale", JNum(std::round(s.heightScale * 10000.0) / 10000.0)});
        for (size_t i = 0; i < s.bodyValues.size(); ++i)
            if (s.bodyValues[i] != 0.0f) sliders.o.push_back({s.parts[0].set.sliders[i].name, JNum(std::round(s.bodyValues[i] * 10000.0) / 10000.0)});
        body.o.push_back({"sliders", sliders});
        put(out.extra, "body", body);
    }
    if (s.hair.painted && s.hair.Ready()) { // the maps next to the preset, named after it
        const std::wstring folder = path.substr(0, path.find_last_of(L"\\/") + 1);
        std::wstring stem = path.substr(folder.size());
        stem = stem.substr(0, stem.find_last_of(L'.'));
        const std::wstring dn = stem + L"_hair_density.png", fn = stem + L"_hair_flow.png";
        if (!s.hair.Save(folder + dn, folder + fn)) return false;
        Json hair = JObj();
        for (const auto& kv : out.extra.o) if (kv.first == "hair") hair = kv.second;
        Json paint = JObj();
        paint.o = {{"density", JStr(Narrow(dn))}, {"flow", JStr(Narrow(fn))}, {"color", JStr(HexColor(s.hair.color))}};
        put(hair, "paint", paint);
        put(out.extra, "hair", hair);
    }
    if (s.browBaked && s.browFull) BakeBrows(); // never save a preview
    if (s.browBaked) { // the brow maker: its boundary and sliders, and the baked texture tied to this character
        const std::wstring folder = path.substr(0, path.find_last_of(L"\\/") + 1);
        std::wstring stem = path.substr(folder.size());
        stem = stem.substr(0, stem.find_last_of(L'.'));
        const std::wstring tn = stem + L"_brows.png";
        WritePngRgba((folder + tn).c_str(), s.brow.tex.data(), s.brow.texW, s.brow.texH);
        Json brows = JObj(), loop;
        loop.type = Json::Array;
        for (const auto& q : s.brow.loop) {
            Json pt;
            pt.type = Json::Array;
            pt.a = {JNum(std::round(q[0] * 1e5) / 1e5), JNum(std::round(q[1] * 1e5) / 1e5)};
            loop.a.push_back(pt);
        }
        for (const BrowSlider& b : kBrowSliders) brows.o.push_back({b.key, JNum(std::round(s.brow.*b.field * 1000.0) / 1000.0)});
        brows.o.insert(brows.o.end(), {{"seed", JNum((double)s.brow.seed)},
                   {"custom", JNum(s.brow.custom ? 1 : 0)}, {"texture", JStr(Narrow(tn))}, {"loop", loop}});
        put(out.extra, "brows", brows);
    }
    return out.Save(path, s.face);
}

std::wstring FileDialog(bool save) {
    State& s = S();
    wchar_t buf[MAX_PATH] = L"character.json";
    if (!save) buf[0] = 0;
    const std::wstring dir = ToolsDir() + L"\\face_maker\\out";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s.host.parent;
    ofn.lpstrFilter = L"Character preset (*.json)\0*.json\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return L"";
    return buf;
}

void SetAge(float a) {
    State& s = S();
    a = std::clamp(a, 0.0f, 1.0f);
    for (const auto& [i, k] : s.face.ageOffsets) s.values[(size_t)i] = std::clamp(s.values[(size_t)i] + k * (a - s.age), -1.0f, 1.0f);
    s.age = a;
    s.faceDirty = true;
}

void PhotoEnter();
void PhotoLeave();
void SelectTab(int tab) {
    State& s = S();
    const int prev = s.tab;
    if (prev == 3 && tab != 3) PhotoLeave();
    s.tab = tab;
    if (tab == 0) { PollBody(); EnsureBody(); }
    if (tab == 3 && prev != 3) PhotoEnter();
    FrameCamera();
    Redraw();
}

void PickHairColor() {
    State& s = S();
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = s.host.parent;
    cc.rgbResult = RGB((int)(s.hair.color[0] * 255), (int)(s.hair.color[1] * 255), (int)(s.hair.color[2] * 255));
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    s.hair.color[0] = GetRValue(cc.rgbResult) / 255.0f; s.hair.color[1] = GetGValue(cc.rgbResult) / 255.0f; s.hair.color[2] = GetBValue(cc.rgbResult) / 255.0f;
    s.bakeNow = s.hair.painted;
}

// ---------------------------------------------------------------- clothes maker (CLOTHES tab)
struct ClothSlider { const wchar_t* group; const wchar_t* label; float clothes::ShirtRecipe::*field; float lo, hi, show; const wchar_t* fmt; };
const ClothSlider kClothSliders[] = {
    {L"FIT", L"Chest ease", &clothes::ShirtRecipe::chestEase, 0.04f, 0.16f, 100, L"%.1f cm"},
    {L"FIT", L"Waist ease", &clothes::ShirtRecipe::waistEase, 0.02f, 0.12f, 100, L"%.1f cm"},
    {L"FIT", L"Sleeve length", &clothes::ShirtRecipe::sleeveLength, 0.45f, 0.80f, 100, L"%.0f %%"},
    {L"FIT", L"Roll width", &clothes::ShirtRecipe::rollWidth, 0.030f, 0.070f, 100, L"%.1f cm"},
    {L"FIT", L"Tail in the jeans", &clothes::ShirtRecipe::tuckDepth, 0.040f, 0.100f, 100, L"%.1f cm"},
    {L"FIT", L"Tail pulled down", &clothes::ShirtRecipe::tuckPull, 0.0f, 0.05f, 100, L"%.1f cm"},
    {L"FIT", L"Back length", &clothes::ShirtRecipe::backLength, -0.02f, 0.04f, 100, L"%.1f cm"},
    {L"COLLAR", L"Stand height", &clothes::ShirtRecipe::collarStand, 0.018f, 0.045f, 100, L"%.1f cm"},
    {L"COLLAR", L"Leaf length", &clothes::ShirtRecipe::collarLeaf, 0.030f, 0.070f, 100, L"%.1f cm"},
    {L"COLLAR", L"Spread", &clothes::ShirtRecipe::collarSpread, 0.0f, 1.0f, 1, L"%.2f"},
    {L"CLOTH", L"Fabric weight", &clothes::ShirtRecipe::fabricWeight, 0.5f, 2.0f, 1, L"%.2f"},
    {L"CLOTH", L"Stretch stiffness", &clothes::ShirtRecipe::stretch, 0.5f, 2.0f, 1, L"%.2f"},
    {L"CLOTH", L"Bend stiffness", &clothes::ShirtRecipe::bend, 0.3f, 3.0f, 1, L"%.2f"},
    {L"FABRIC", L"Stripe spacing", &clothes::ShirtRecipe::stripeSpacing, 0.003f, 0.015f, 1000, L"%.1f mm"},
    {L"FABRIC", L"Stripe width", &clothes::ShirtRecipe::stripeWidth, 0.0004f, 0.0020f, 1000, L"%.2f mm"},
    {L"FABRIC", L"Wear / fade", &clothes::ShirtRecipe::wear, 0.0f, 1.0f, 1, L"%.2f"},
    {L"FABRIC", L"Dirt", &clothes::ShirtRecipe::dirt, 0.0f, 1.0f, 1, L"%.2f"},
};
constexpr int kClothSliderCount = (int)(sizeof(kClothSliders) / sizeof(kClothSliders[0]));
bool IsClothSlider(int a) { return a >= A_Cloth && a < A_Cloth + kClothSliderCount; }

std::string StripTag(const std::string& p) { const size_t h = p.find('#'); return h == std::string::npos ? p : p.substr(0, h); }
std::string LowerStr(std::string v) { for (char& c : v) c = (char)std::tolower((unsigned char)c); return v; }
std::wstring ClothOutDir() {
    std::wstring root = ToolsDir();
    const size_t cut = root.find_last_of(L"\\/");
    root = cut == std::wstring::npos ? root : root.substr(0, cut);
    return root + L"\\Assets\\clothes\\" + Utf8ToWide(S().shirt.name);
}

void StartClothBuild() {
    State& s = S();
    if (s.clothBusy) { s.clothAgain = true; return; } // one more build when this one lands
    const int head = FindEntity("body_head");
    if (head < 0) { s.clothStatus = "No body_head in the scene: the shirt is fitted to that character's body_torso.glb."; Redraw(); return; }
    const std::string hp = StripTag(s.host.entityPath(head));
    clothes::BuildInput in;
    in.bodyGlb = hp.substr(0, hp.find_last_of("\\/") + 1) + "body_torso.glb";
    in.legsGlb = hp.substr(0, hp.find_last_of("\\/") + 1) + "body_legs.glb";
    in.headGlb = hp;
    in.script = ToolsDir() + L"\\clothes_sim\\build_garment.py";
    for (int i = 0; i < s.host.entityCount(); ++i) {
        const std::string p = StripTag(s.host.entityPath(i));
        if (LowerStr(p).find("jeans") != std::string::npos || LowerStr(s.host.entityName(i)) == "jeans") { in.jeansGlb = p; break; }
    }
    in.outDir = ClothOutDir();
    if (s.clothJob.joinable()) s.clothJob.join();
    s.clothBusy = true;
    s.clothDone = false;
    s.clothStatus = "Building the shirt: pattern, cloth simulation and bake in Blender (1-2 minutes)...";
    const clothes::ShirtRecipe recipe = s.shirt;
    s.clothJob = std::thread([recipe, in] {
        State& st = S();
        st.clothRes = clothes::BuildShirtCloth(recipe, in); // sewn + simulated in Blender; clothes::BuildShirt is the legacy builder
        st.clothDone = true;
    });
    Redraw();
}

// UI thread: the finished build goes on the scene's shirt entity (its old file is remembered for Restore).
void ClothPoll() {
    State& s = S();
    if (s.clothBusy && !s.clothDone) { // the Blender build reports its stage
        const std::string p = clothes::ClothProgress();
        if (!p.empty() && p != s.clothStatus) { s.clothStatus = p; Redraw(); }
        return;
    }
    if (!s.clothBusy || !s.clothDone) return;
    if (s.clothJob.joinable()) s.clothJob.join();
    s.clothBusy = false;
    const clothes::BuildResult& r = s.clothRes;
    if (!r.ok) {
        s.clothStatus = "Build failed: " + r.error;
        Log("Character: clothes build failed: %s", r.error.c_str());
    } else {
        const std::string path = r.glbPath + "#native";
        int ent = -1;
        for (int i = 0; i < s.host.entityCount(); ++i) if (s.host.entityPath(i) == path) { ent = i; break; }
        if (ent < 0) for (int i = 0; i < s.host.entityCount(); ++i) if (LowerStr(s.host.entityName(i)) == "shirt") { ent = i; break; }
        if (ent < 0) {
            ent = s.host.addEntity(FindEntity("body_head"), "Shirt", path);
            s.clothOrigPath.clear();
        } else if (s.host.entityPath(ent) == path) s.host.reloadEntity(ent);
        else { s.clothOrigPath = s.host.entityPath(ent); s.host.setEntityPath(ent, path); }
        s.clothEntity = ent;
        char b[160];
        snprintf(b, sizeof(b), "Built in %.0f ms: ", r.ms);
        s.clothStatus = b + r.summary;
        Log("Character: clothes build %.0f ms -> %s (%s)", r.ms, r.glbPath.c_str(), r.summary.c_str());
    }
    if (s.clothAgain) { s.clothAgain = false; StartClothBuild(); }
    Redraw();
}

void PickClothColor(float* c) {
    State& s = S();
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = s.host.parent;
    const auto g = [](float x) { return (int)std::lround(std::pow(std::clamp(x, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f); };
    cc.rgbResult = RGB(g(c[0]), g(c[1]), g(c[2]));
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    const auto l = [](int v) { return std::pow(v / 255.0f, 2.2f); };
    c[0] = l(GetRValue(cc.rgbResult)); c[1] = l(GetGValue(cc.rgbResult)); c[2] = l(GetBValue(cc.rgbResult));
    if (s.clothAuto) StartClothBuild();
}

std::wstring RecipeDialog(bool save) {
    State& s = S();
    wchar_t buf[MAX_PATH] = L"recipe.json";
    if (!save) buf[0] = 0;
    const std::wstring dir = ClothOutDir();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s.host.parent;
    ofn.lpstrFilter = L"Garment recipe (*.json)\0*.json\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return L"";
    return buf;
}

// ---------------------------------------------------------------- PHOTO FIT tab (photo_fit.h; plan in docs/photo_fit_plan.md)
photo::Photo* ActivePhoto() {
    State& s = S();
    return s.photoActive >= 0 && s.photoActive < (int)s.photos.size() ? &s.photos[(size_t)s.photoActive] : nullptr;
}
void PhotoRedraw() {
    State& s = S();
    s.layerKey = 0;
    if (s.tab == 3) s.status = s.photoStatus;
    if (s.wnd2d) InvalidateRect(s.wnd2d, nullptr, FALSE);
    Redraw();
}
std::wstring PhotoSessionPath() {
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) == 0) return L"";
    const std::wstring d = std::wstring(buf) + L"\\tbd_engine";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\photo_fit_session.json";
}
void SavePhotoSession() {
    State& s = S();
    const std::wstring p = PhotoSessionPath();
    if (!p.empty()) WriteAllText(p, photo::ToJson(s.photos, s.photoActive, s.pins));
}

// A photo from disk: decoded by the imaging component, turned upright by its EXIF orientation, shrunk so its long side is at most 2048.
bool LoadPhotoFile(const std::wstring& path, photo::Photo& out) {
    using Microsoft::WRL::ComPtr;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> fac;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
            SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 &&
            SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            UINT orient = 1;
            ComPtr<IWICMetadataQueryReader> q;
            if (SUCCEEDED(frame->GetMetadataQueryReader(&q))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v)) && v.vt == VT_UI2) orient = v.uiVal;
                PropVariantClear(&v);
            }
            std::vector<uint8_t> src((size_t)w * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)src.size(), src.data()))) {
                const int f = (int)(std::max(w, h) + 2047) / 2048; // box shrink factor
                const int sw = (int)w / f, sh = (int)h / f;
                std::vector<uint8_t> small((size_t)sw * sh * 4);
                for (int y = 0; y < sh; ++y)
                    for (int x = 0; x < sw; ++x) {
                        int acc[4] = {0, 0, 0, 0};
                        for (int dy = 0; dy < f; ++dy)
                            for (int dx = 0; dx < f; ++dx) {
                                const uint8_t* p = &src[((size_t)(y * f + dy) * w + (size_t)(x * f + dx)) * 4];
                                for (int c = 0; c < 4; ++c) acc[c] += p[c];
                            }
                        for (int c = 0; c < 4; ++c) small[((size_t)y * sw + x) * 4 + (size_t)c] = (uint8_t)(acc[c] / (f * f));
                    }
                const int ow = orient >= 5 && orient <= 8 ? sh : sw, oh = orient >= 5 && orient <= 8 ? sw : sh;
                out.rgba.assign((size_t)ow * oh * 4, 255);
                for (int y = 0; y < oh; ++y)
                    for (int x = 0; x < ow; ++x) {
                        int sx = x, sy = y;
                        switch (orient) {
                        case 2: sx = sw - 1 - x; break;
                        case 3: sx = sw - 1 - x; sy = sh - 1 - y; break;
                        case 4: sy = sh - 1 - y; break;
                        case 5: sx = y; sy = x; break;
                        case 6: sx = y; sy = sh - 1 - x; break;
                        case 7: sx = sw - 1 - y; sy = sh - 1 - x; break;
                        case 8: sx = sw - 1 - y; sy = x; break;
                        default: break;
                        }
                        std::memcpy(&out.rgba[((size_t)y * ow + x) * 4], &small[((size_t)sy * sw + sx) * 4], 4);
                    }
                out.w = ow; out.h = oh; out.path = path;
                ok = true;
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// The head without its hair cards, as triangles (what the photo view draws), and where the head turns about.
void BuildPhotoTris() {
    State& s = S();
    s.photoTris.clear();
    size_t end = s.headNow.size();
    std::vector<std::pair<size_t, size_t>> eyes; // the eyeballs are hidden here: their dense mesh buries the lids and the pins
    if (const ImportedModel* m = s.target >= 0 ? s.host.entityModel(s.target) : nullptr)
        for (const ImportedPart& p : m->parts) {
            if (p.hair) end = std::min(end, p.first);
            if (p.eye) eyes.push_back({p.first, p.first + p.count});
        }
    end -= end % 3;
    s.photoSrc.clear();
    for (size_t v = 0; v < end; v += 3) {
        bool skip = false;
        for (const auto& r : eyes) if (v >= r.first && v < r.second) { skip = true; break; }
        if (skip) continue;
        s.photoTris.insert(s.photoTris.end(), s.headNow.begin() + (std::ptrdiff_t)v, s.headNow.begin() + (std::ptrdiff_t)v + 3);
        for (size_t k = 0; k < 3; ++k) s.photoSrc.push_back((uint32_t)(v + k));
    }
    if (s.pins.ok)
        for (int k = 0; k < 3; ++k) {
            s.faceCentre[k] = 0;
            for (int i = 0; i < photo::kPins; ++i) s.faceCentre[k] += s.pins.p[i][k] / (float)photo::kPins;
        }
}

void SolveActive() {
    State& s = S();
    photo::Photo* ph = ActivePhoto();
    if (!ph) return;
    if (!s.pins.ok) { s.photoStatus = "The head's pin points are not ready (the face tracker could not find the face)."; return; }
    if (photo::SolveCamera(*ph, s.pins, s.headNow, s.faceCentre)) {
        char b[160];
        snprintf(b, sizeof b, "Camera solved from %d pins: average miss %.1f px (head turned %.0f deg).", ph->Marked(), ph->cam.rms, ph->cam.yaw * 57.2958f);
        s.photoStatus = b;
    } else s.photoStatus = ph->Marked() < 4 ? "Mark at least 4 pins to place the head on the photo (" + std::to_string(ph->Marked()) + " so far)." : "Camera could not be solved.";
}

void LoadPhotoSession() {
    State& s = S();
    s.photoLoaded = true;
    std::string text;
    const std::wstring p = PhotoSessionPath();
    if (p.empty() || !ReadAllText(p, text)) return;
    Json doc;
    if (!JsonParser(text.data(), text.size()).Parse(doc) || doc.type != Json::Object) return;
    if (const Json* cu = doc.Find("custom"); cu && s.pins.ok)
        for (const Json& c : cu->a) {
            const Json* q = c.Find("p");
            const Json* nm = c.Find("name");
            if (!q || q->a.size() < 3) continue;
            const float pt[3] = {(float)q->a[0].Num(), (float)q->a[1].Num(), (float)q->a[2].Num()};
            s.pins.AddCustom(nm ? Utf8ToWide(nm->s) : L"Custom pin", pt, s.head.Rest());
        }
    const Json* arr = doc.Find("photos");
    for (size_t i = 0; arr && i < arr->a.size(); ++i) {
        const Json& j = arr->a[i];
        photo::Photo ph;
        const Json* path = j.Find("path");
        if (!path || !LoadPhotoFile(Utf8ToWide(path->s), ph)) continue;
        if (const Json* l = j.Find("label")) ph.label = l->s;
        if (const Json* f = j.Find("fov")) ph.cam.fov = std::clamp((float)f->Num(28), 8.0f, 70.0f);
        if (const Json* f = j.Find("size")) ph.size = std::clamp((float)f->Num(1), 0.2f, 4.0f);
        if (const Json* f = j.Find("offx")) ph.offx = std::clamp((float)f->Num(0), -1.0f, 1.0f);
        if (const Json* f = j.Find("offy")) ph.offy = std::clamp((float)f->Num(0), -1.0f, 1.0f);
        if (const Json* m = j.Find("marks"))
            for (int k = 0; k < photo::kMaxPins && (size_t)k < m->a.size(); ++k) {
                const Json& e = m->a[(size_t)k];
                if (e.a.size() < 3) continue;
                ph.mark[k].state = (uint8_t)std::clamp(e.a[0].Int(), 0, 2);
                ph.mark[k].x = (float)e.a[1].Num();
                ph.mark[k].y = (float)e.a[2].Num();
            }
        s.photos.push_back(std::move(ph));
    }
    if (const Json* a = doc.Find("active")) s.photoActive = std::clamp(a->Int(0), -1, (int)s.photos.size() - 1);
    if (s.photoActive < 0 && !s.photos.empty()) s.photoActive = 0;
}

void FitShapeNow();
void BuildSkinNow(bool project);
void PhotoEnter() {
    State& s = S();
    s.viewBeforePhoto = s.view;
    s.view = 1;
    s.pZoom = 1; s.pCx = s.pCy = 0.5f;
    if (!BindHead()) s.photoStatus = s.status;
    else {
        if (!s.pinsTried) {
            s.pinsTried = true;
            std::vector<float> lm;
            if (s.host.faceLandmarks && s.host.faceLandmarks(s.target, &lm) && lm.size() >= 468 * 3) s.pins.Attach(lm.data(), s.head.Rest());
            else s.photoStatus = "The face tracker found no face on the head: pins cannot be placed on it.";
        }
        BuildPhotoTris();
        if (InitHair() && s.shown.empty()) s.bakeNow = true; // the side-by-side head shows the albedo: bake it once
    }
    if (!s.photoLoaded) LoadPhotoSession();
    for (photo::Photo& ph : s.photos)
        if (!ph.cam.ok && ph.Marked() >= 4) photo::SolveCamera(ph, s.pins, s.headNow, s.faceCentre);
    if (s.photoStatus.empty()) s.photoStatus = "Add a photo, pick a pin in the list, then click its place on the photo.";
    if (char au[8] = {}; GetEnvironmentVariableA("TBD_PHOTOAUTO", au, sizeof(au)) > 0) { // test hook: s = fit the shape, k = read the skin colours, p = project the photo
        if (strchr(au, 's')) FitShapeNow();
        if (strchr(au, 'k')) BuildSkinNow(false);
        if (strchr(au, 'p')) BuildSkinNow(true);
    }
    if (char sbs[8] = {}; GetEnvironmentVariableA("TBD_PHOTOSBS", sbs, sizeof(sbs)) > 0) s.photoSbs = sbs[0] == '1';
    if (char sw[16] = {}; GetEnvironmentVariableA("TBD_SKINWARM", sw, sizeof(sw)) > 0) s.skinWarm = (float)atof(sw); // test hooks for the skin colour sliders
    if (char se[16] = {}; GetEnvironmentVariableA("TBD_SKINEXP", se, sizeof(se)) > 0) s.skinExp = (float)atof(se);
    if (s.host.relayout) s.host.relayout();
    PhotoRedraw();
}
void PhotoLeave() {
    State& s = S();
    s.view = s.viewBeforePhoto;
    if (s.host.relayout) s.host.relayout();
}

std::vector<std::wstring> PhotoDialog() {
    State& s = S();
    static wchar_t buf[8192];
    buf[0] = 0;
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s.host.parent;
    ofn.lpstrFilter = L"Photos\0*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 8192;
    ofn.lpstrTitle = L"Add photos of the person (several angles: select them together)";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn)) return out;
    const std::wstring first = buf;
    const wchar_t* next = buf + first.size() + 1;
    if (*next == 0) { out.push_back(first); return out; }
    for (; *next; next += wcslen(next) + 1) out.push_back(first + L"\\" + next);
    return out;
}

void AddPhotos(const std::vector<std::wstring>& paths) {
    State& s = S();
    int added = 0;
    for (const std::wstring& p : paths) {
        photo::Photo ph;
        if (!LoadPhotoFile(p, ph)) { s.photoStatus = "Could not read " + Narrow(p); continue; }
        s.photos.push_back(std::move(ph));
        s.photoActive = (int)s.photos.size() - 1;
        ++added;
    }
    if (added) {
        s.pZoom = 1; s.pCx = s.pCy = 0.5f; s.photoPin = 0;
        s.photoStatus = "Added " + std::to_string(added) + " photo(s). Pick a pin, then click it on the photo.";
        SavePhotoSession();
    }
    PhotoRedraw();
}


// The face sliders that bring the head's pins onto the marks of every photo (photo::FitShape). The pins' response to each slider is read from the
// head's own deform once per pin set; the solve itself is a few small linear systems.
void FitShapeNow() {
    State& s = S();
    if (!BindHead() || !s.face.Valid()) { s.photoStatus = "The face sliders are not loaded: " + s.faceError; return; }
    if (!s.pins.ok) { s.photoStatus = "The head's pin points are missing."; return; }
    int ready = 0;
    for (const photo::Photo& ph : s.photos) ready += ph.Marked() >= 4;
    if (!ready) { s.photoStatus = "Mark at least 4 pins on a photo first."; return; }
    const int nS = (int)s.face.sliders.size(), n = s.pins.count;
    s.values.resize((size_t)nS, 0.0f);
    const double t0 = NowMs();
    uint64_t key = 1469598103934665603ull ^ (uint64_t)s.target;
    for (int k = 0; k < n; ++k) key = (key ^ (uint64_t)(s.pins.vtx[k] + 1)) * 1099511628211ull;
    key = (key ^ (uint64_t)nS) * 1099511628211ull;
    const std::vector<Vertex>& rest = s.head.Rest();
    if (key != s.shapeKey || s.shapeD.size() != (size_t)n * 3 * (size_t)nS) {
        s.shapeD.assign((size_t)n * 3 * (size_t)nS, 0.0f);
        std::vector<float> unit((size_t)nS, 0.0f);
        std::vector<Vertex> out;
        for (int i = 0; i < nS; ++i) {
            unit[(size_t)i] = 1.0f;
            s.head.Deform(unit, out, nullptr, nullptr);
            unit[(size_t)i] = 0.0f;
            for (int k = 0; k < n; ++k) {
                const size_t v = (size_t)s.pins.vtx[k];
                if (v >= out.size() || v >= rest.size()) continue;
                s.shapeD[(size_t)(k * 3 + 0) * nS + i] = out[v].px - rest[v].px;
                s.shapeD[(size_t)(k * 3 + 1) * nS + i] = out[v].py - rest[v].py;
                s.shapeD[(size_t)(k * 3 + 2) * nS + i] = out[v].pz - rest[v].pz;
            }
        }
        s.shapeKey = key;
    }
    const double t1 = NowMs();
    std::vector<float> X0((size_t)n * 3), lo((size_t)nS), hi((size_t)nS), v = s.values;
    for (int k = 0; k < n; ++k) s.pins.Now(s.headNow, k, &X0[(size_t)k * 3]);
    for (int i = 0; i < nS; ++i) { lo[(size_t)i] = s.face.sliders[(size_t)i].lo; hi[(size_t)i] = s.face.sliders[(size_t)i].hi; }
    const photo::ShapeResult r = photo::FitShape(s.photos, s.pins, s.shapeD, nS, v, lo, hi, s.faceCentre, X0.data());
    if (!r.ok) { s.photoStatus = "The shape could not be fitted (the cameras could not be solved)."; return; }
    s.values = v;
    s.presetSeed = -1;
    ApplyFace();
    BuildPhotoTris();
    for (photo::Photo& ph : s.photos) if (ph.Marked() >= 4) photo::SolveCamera(ph, s.pins, s.headNow, s.faceCentre);
    char b[200];
    snprintf(b, sizeof b, "Face fitted to %d photo(s): average miss %.1f px -> %.1f px (%.0f + %.0f ms). Undo restores it.", r.photos, r.before, r.after, t1 - t0, NowMs() - t1);
    s.photoStatus = b;
}

float UvPerMetre2D();
// Skin colour from the photos (photo_skin.h): the gain layer over the head's albedo, then a bake.
void BuildSkinNow(bool project) {
    State& s = S();
    if (!InitHair()) { s.photoStatus = "The head is not ready yet (open the HEAD tab once)."; return; }
    if (!s.pins.ok) { s.photoStatus = "The head's pin points are missing."; return; }
    std::vector<Vertex> skinTris;
    if (const ImportedModel* m = s.host.entityModel(s.target))
        for (const ImportedPart& p : m->parts)
            if (p.skin && p.first + p.count <= s.headNow.size()) skinTris.insert(skinTris.end(), s.headNow.begin() + (std::ptrdiff_t)p.first, s.headNow.begin() + (std::ptrdiff_t)(p.first + p.count));
    float zp[5][3];
    float a[3], b[3];
    s.pins.Now(s.headNow, 15, a); s.pins.Now(s.headNow, 16, b);                                   // forehead: above the brow ends
    zp[0][0] = (a[0] + b[0]) * 0.5f; zp[0][1] = (a[1] + b[1]) * 0.5f + 0.035f; zp[0][2] = (a[2] + b[2]) * 0.5f;
    s.pins.Now(s.headNow, 1, a); s.pins.Now(s.headNow, 8, b);                                     // right cheek: between eye corner and mouth corner
    for (int k = 0; k < 3; ++k) zp[1][k] = (a[k] + b[k]) * 0.5f;
    s.pins.Now(s.headNow, 3, a); s.pins.Now(s.headNow, 9, b);
    for (int k = 0; k < 3; ++k) zp[2][k] = (a[k] + b[k]) * 0.5f;
    s.pins.Now(s.headNow, 4, zp[3]);                                                              // nose tip
    s.pins.Now(s.headNow, 19, zp[4]); zp[4][1] += 0.02f;                                          // chin
    std::vector<uint8_t> gain;
    const double t0 = NowMs();
    s.skinRep = photo::BuildSkinGain(s.photos, skinTris, s.photoTris, s.faceCentre, s.albedo, s.albedoW, s.albedoH, project, s.skinStrength, s.skinWarm, s.skinExp, UvPerMetre2D(), SkinPaint::N, zp, 5, gain);
    if (!s.skinRep.ok) { s.photoStatus = s.skinRep.message; return; }
    s.sp.SetGain(gain);
    s.bakeNow = true;
    char b2[64];
    snprintf(b2, sizeof b2, " (%.0f ms)", NowMs() - t0);
    s.photoStatus = s.skinRep.message + b2;
    Log("Character: photo skin: %s", s.photoStatus.c_str());
}

void PhotoDo(int a) {
    State& s = S();
    photo::Photo* ph = ActivePhoto();
    static const char* labels[] = {"front", "3/4 left", "3/4 right", "profile left", "profile right", "other"};
    if (a == A_TabPhoto) { SelectTab(3); return; }
    if (a == A_PhotoAdd) { AddPhotos(PhotoDialog()); return; }
    if (a == A_PhotoRemove && ph) {
        s.photos.erase(s.photos.begin() + s.photoActive);
        s.photoActive = s.photos.empty() ? -1 : std::min(s.photoActive, (int)s.photos.size() - 1);
        SavePhotoSession();
    } else if (a == A_PhotoLabel && ph) {
        int k = 0;
        for (int i = 0; i < 6; ++i) if (ph->label == labels[i]) k = i;
        ph->label = labels[(k + 1) % 6];
        SavePhotoSession();
    } else if ((a == A_PhotoHide || a == A_PhotoClear) && ph) {
        photo::Mark& m = ph->mark[s.photoPin];
        m.state = a == A_PhotoClear ? 0 : (m.state == 2 ? 0 : 2);
        SolveActive();
        SavePhotoSession();
    } else if (a == A_PhotoSkinRead || a == A_PhotoSkinProject) BuildSkinNow(a == A_PhotoSkinProject);
    else if (a == A_PhotoSkinClear) { s.sp.ClearGain(); s.skinRep = photo::SkinReport(); s.bakeNow = true; s.photoStatus = "Photo skin colour removed."; }
    else if (a == A_PhotoShape) FitShapeNow();
    else if (a == A_PhotoShapeReset) {
        std::fill(s.values.begin(), s.values.end(), 0.0f);
        s.presetSeed = -1;
        ApplyFace();
        BuildPhotoTris();
        for (photo::Photo& p2 : s.photos) if (p2.Marked() >= 4) photo::SolveCamera(p2, s.pins, s.headNow, s.faceCentre);
        s.photoStatus = "Face sliders back to neutral.";
    }
    else if (a == A_PhotoAuto) s.photoAutoNext = !s.photoAutoNext;
    else if (a == A_PhotoOverlay) s.photoSbs = false;
    else if (a == A_PhotoSbs) s.photoSbs = true;
    else if (a == A_PhotoFit) { s.pZoom = 1; s.pCx = s.pCy = 0.5f; s.viewYaw = 0; }
    else if (a == A_PhotoPlaceReset && ph) { ph->size = 1; ph->offx = ph->offy = 0; SolveActive(); SavePhotoSession(); }
    else if (a == A_PhotoCustom) {
        if (s.customPlacing) { s.customPlacing = false; s.photoStatus = "Custom pin cancelled."; }
        else if (!ph) s.photoStatus = "Add a photo first.";
        else if (s.pins.count >= photo::kMaxPins) s.photoStatus = "No room for more custom pins.";
        else { s.customPlacing = true; s.photoSbs = true; s.photoStatus = "Click the head on the RIGHT where your pin goes."; }
    }
    else if (a == A_PhotoCustomDel) {
        if (s.photoPin >= photo::kPins) {
            photo::RemoveCustomPin(s.pins, s.photos, s.photoPin);
            s.photoPin = std::min(s.photoPin, s.pins.count - 1);
            SolveActive();
            SavePhotoSession();
        } else s.photoStatus = "Only your own pins can be removed.";
    }
    else if (a >= A_PhotoPin && a < A_PhotoPin + photo::kMaxPins) { if (a - A_PhotoPin < s.pins.count) s.photoPin = a - A_PhotoPin; }
    else if (a >= A_PhotoSel && a < A_PhotoSel + 16 && a - A_PhotoSel < (int)s.photos.size()) { s.photoActive = a - A_PhotoSel; s.pZoom = 1; s.pCx = s.pCy = 0.5f; SavePhotoSession(); }
    PhotoRedraw();
}

// The camera the rendered head is drawn with: the photo's, turned by the rotation slider when the head is shown beside the photo.
// Before a photo has 4 pins the camera is not solved: the head is shown from the front, about 17 cm wide over 60% of the photo, so it can be seen
// and its pins found; marking pins replaces this with the solved camera.
photo::Cam DefaultCam(const photo::Photo& ph) {
    photo::Cam c;
    c.fov = ph.cam.fov;
    c.ok = true;
    c.dist = photo::Focal(c, ph.w, ph.h) * 0.17f / (0.60f * (float)ph.w);
    return c;
}
photo::Cam HeadCam(const photo::Photo& ph) {
    photo::Cam c = ph.cam.ok ? ph.cam : DefaultCam(ph);
    if (S().photoSbs) c.yaw += S().viewYaw;
    return c;
}

// Pane geometry shared by painting and the mouse: the photo pane is the left half in side-by-side, the whole canvas otherwise.
struct PhotoPane { int paneW, paneH, top; photo::View vw; };
bool PhotoPaneOf(HWND hwnd, PhotoPane* o) {
    State& s = S();
    const photo::Photo* ph = ActivePhoto();
    if (!ph) return false;
    RECT cr;
    GetClientRect(hwnd, &cr);
    o->top = s.st.S(24);
    o->paneW = s.photoSbs ? (int)cr.right / 2 : (int)cr.right;
    o->paneH = std::max(8, (int)cr.bottom - o->top);
    o->vw = photo::FitView(o->paneW, o->paneH, ph->w, ph->h, s.pZoom, s.pCx, s.pCy);
    return true;
}

uint64_t LayerKey(const photo::Photo& ph, const PhotoPane& pn, bool shaded) {
    State& s = S();
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
    const photo::Cam hc = HeadCam(ph);
    const float v[10] = {hc.yaw, hc.pitch, hc.roll, hc.tx, hc.ty, hc.dist, hc.fov, pn.vw.scale, pn.vw.ox, pn.vw.oy};
    mix(v, sizeof v);
    const int d[5] = {pn.paneW, pn.paneH, shaded, ph.cam.ok, s.albedoStamp};
    mix(d, sizeof d);
    for (int i = 0; i < s.pins.count; i += 4) { float p[3]; s.pins.Now(s.headNow, i, p); mix(p, sizeof p); }
    const size_t n = s.photoTris.size();
    mix(&n, sizeof n);
    return h | 1;
}

void PaintPhoto(HWND hwnd, HDC dc, uint8_t* px, int W, int H) {
    State& s = S();
    PhotoPane pn;
    if (!PhotoPaneOf(hwnd, &pn)) {
        Text(dc, W / 2, H / 2 - s.st.S(8), L"PHOTO FIT: press Add photo (left panel), pick a pin, click its place on the photo.", kMuted, s.st.small, TA_CENTER);
        Text(dc, W / 2, H / 2 + s.st.S(10), L"Several angles of the same person work best; any pins you can see are enough.", kMuted, s.st.small, TA_CENTER);
        return;
    }
    photo::Photo& ph = *ActivePhoto();
    const bool sbs = s.photoSbs;
    const uint64_t key = LayerKey(ph, pn, sbs);
    if (key != s.layerKey) {
        photo::Photo rp;
        rp.w = ph.w; rp.h = ph.h; rp.cam = HeadCam(ph);
        const bool haveTex = sbs && s.shown.size() >= (size_t)s.albedoW * s.albedoH * 4 && s.albedoW > 0;
        photo::RenderHead(rp, s.photoTris, s.faceCentre, pn.vw, pn.paneW, pn.paneH, sbs, s.layer, haveTex ? s.shown.data() : nullptr, s.albedoW, s.albedoH);
        s.layerKey = key;
    }
    GdiFlush();
    const float inv = 1.0f / pn.vw.scale;
    const int wa = (int)(std::clamp(s.wireAlpha, 0.0f, 1.0f) * 256.0f);
    for (int y = 0; y < pn.paneH; ++y) {
        uint8_t* row = px + ((size_t)(y + pn.top) * W) * 4;
        for (int x = 0; x < pn.paneW; ++x) {
            float ixf, iyf;
            ph.FrameToImg(((float)x - pn.vw.ox) * inv, ((float)y - pn.vw.oy) * inv, &ixf, &iyf);
            const int ix = (int)std::floor(ixf), iy = (int)std::floor(iyf);
            int b = 22, g = 24, r = 24;
            if (ix >= 0 && iy >= 0 && ix < ph.w && iy < ph.h) { const uint8_t* q = &ph.rgba[((size_t)iy * ph.w + ix) * 4]; r = q[0]; g = q[1]; b = q[2]; }
            const uint8_t* l = &s.layer.bgra[((size_t)y * pn.paneW + x) * 4];
            const bool haveLayer = s.layer.w == pn.paneW && s.layer.h == pn.paneH;
            if (!sbs && haveLayer && l[3]) {
                const int a = l[3] * wa >> 8;
                b += ((int)l[0] - b) * a >> 8; g += ((int)l[1] - g) * a >> 8; r += ((int)l[2] - r) * a >> 8;
            }
            uint8_t* o = row + (size_t)x * 4;
            o[0] = (uint8_t)b; o[1] = (uint8_t)g; o[2] = (uint8_t)r; o[3] = 255;
            if (sbs && x + pn.paneW < W) { // the rendered head, same view
                uint8_t* o2 = row + (size_t)(x + pn.paneW) * 4;
                if (haveLayer && l[3]) { o2[0] = l[0]; o2[1] = l[1]; o2[2] = l[2]; } else { o2[0] = 26; o2[1] = 24; o2[2] = 22; }
                o2[3] = 255;
            }
        }
    }
    // pins: red = where the head's pin is now (needs the camera), cyan = what you marked, a line between them
    const auto toPane = [&](float ix, float iy, int paneX, int* ox, int* oy) { *ox = paneX + (int)(pn.vw.ox + ix * pn.vw.scale); *oy = pn.top + (int)(pn.vw.oy + iy * pn.vw.scale); };
    const auto dot = [&](int cx, int cy, int rad, COLORREF fillc, COLORREF edge) {
        HBRUSH br = CreateSolidBrush(fillc);
        HPEN pen = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
        Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
    };
    const int rad = std::max(3, s.st.S(4));
    for (int i = 0; i < s.pins.count; ++i) {
        const bool active = i == s.photoPin;
        int rx = 0, ry = 0, mx = 0, my = 0;
        bool haveRed = false;
        float o[3] = {};
        if (s.pins.ok) {
            float p[3];
            s.pins.Now(s.headNow, i, p);
            photo::Project(ph.cam.ok ? ph.cam : DefaultCam(ph), ph.w, ph.h, s.faceCentre, p, o);
            toPane(o[0], o[1], 0, &rx, &ry);
            haveRed = true;
        }
        const photo::Mark& m = ph.mark[i];
        if (m.state == 1) {
            float mfx, mfy;
            ph.MarkPx(i, &mfx, &mfy);
            toPane(mfx, mfy, 0, &mx, &my);
            if (haveRed && ph.cam.ok) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 235, 120));
                HGDIOBJ op = SelectObject(dc, pen);
                MoveToEx(dc, rx, ry, nullptr);
                LineTo(dc, mx, my);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
        }
        if (haveRed && m.state != 2) {
            if (ph.cam.ok || !sbs) dot(rx, ry, rad, RGB(230, 50, 40), RGB(255, 255, 255));
            if (sbs) {
                float p2[3], o2[3];
                s.pins.Now(s.headNow, i, p2);
                photo::Project(HeadCam(ph), ph.w, ph.h, s.faceCentre, p2, o2);
                if (std::fabs(s.viewYaw) < 1.45f) { // a pin turned to the far side is hidden by the head: skip it
                    float n2[3];
                    photo::Rotate(HeadCam(ph), s.faceCentre, p2, n2);
                    if (std::fabs(s.viewYaw) < 0.9f || n2[2] > -0.0f) { int sx, sy; toPane(o2[0], o2[1], pn.paneW, &sx, &sy); dot(sx, sy, rad, active ? RGB(255, 220, 60) : RGB(230, 50, 40), RGB(255, 255, 255)); }
                }
            }
        }
        if (m.state == 1) {
            dot(mx, my, rad, active ? RGB(255, 220, 60) : RGB(70, 200, 230), RGB(20, 20, 20));
            wchar_t nb[8];
            swprintf_s(nb, L"%d", i + 1);
            Text(dc, mx + rad + 3, my - rad - 3, nb, active ? RGB(255, 220, 60) : RGB(200, 235, 245), s.st.small);
        }
    }
    wchar_t info[220];
    swprintf_s(info, L"PHOTO %d/%d  %hs  -  %d of %d pins marked   wheel: zoom   right drag: pan   %ls", s.photoActive + 1, (int)s.photos.size(), ph.label.c_str(),
               ph.Marked(), s.pins.count, s.photoSbs ? L"left: photo, right: the head from that camera" : L"red = head pin, cyan = your mark");
    Fill(dc, 0, 0, W, pn.top, RGB(24, 22, 20));
    Text(dc, s.st.S(10), s.st.S(5), info, kMuted, s.st.small);
}


// Hover preview: a close-up of the head from the front with a dot on the pin under the mouse in the list, and an arrow pointing at it.
void PaintPinPreview(HDC dc, uint8_t* px, int W, int H) {
    State& s = S();
    if (s.hoverPin < 0 || !s.pins.ok || s.photoTris.empty()) return;
    const int N = std::min({s.st.S(260), W - s.st.S(20), H - s.st.S(50)});
    if (N < 80) return;
    if (s.prevPin != s.hoverPin || s.prevLayer.w != N) {
        photo::Photo tmp;
        tmp.w = N; tmp.h = N; tmp.cam.fov = 28; tmp.cam.ok = true;
        float p[3];
        s.pins.Now(s.headNow, s.hoverPin, p);
        tmp.cam.tx = -(p[0] - s.faceCentre[0]);
        tmp.cam.ty = -(p[1] - s.faceCentre[1]);
        tmp.cam.dist = photo::Focal(tmp.cam, N, N) * 0.12f / (float)N + (p[2] - s.faceCentre[2]); // 12 cm across the picture
        photo::RenderHead(tmp, s.photoTris, s.faceCentre, photo::View{1.0f, 0.0f, 0.0f}, N, N, true, s.prevLayer);
        s.prevPin = s.hoverPin;
    }
    GdiFlush();
    const int x0 = W - N - s.st.S(10), y0 = s.st.S(34);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const uint8_t* l = &s.prevLayer.bgra[((size_t)y * N + x) * 4];
            uint8_t* o = px + ((size_t)(y0 + y) * W + (size_t)(x0 + x)) * 4;
            if (l[3]) { o[0] = l[0]; o[1] = l[1]; o[2] = l[2]; } else { o[0] = 34; o[1] = 30; o[2] = 28; }
            o[3] = 255;
        }
    const int cx = x0 + N / 2, cy = y0 + N / 2, r = s.st.S(5);
    HPEN frame = CreatePen(PS_SOLID, 2, RGB(176, 112, 48));
    HGDIOBJ op = SelectObject(dc, frame), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x0, y0, x0 + N, y0 + N);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(frame);
    // arrow: from up-left of the dot to just short of it
    const float len = (float)s.st.S(78), ax = cx - len * 0.7071f, ay = cy - len * 0.7071f, gap = (float)(r + s.st.S(5));
    const float ex = cx - gap * 0.7071f, ey = cy - gap * 0.7071f;
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 225, 80));
    op = SelectObject(dc, pen);
    MoveToEx(dc, (int)ax, (int)ay, nullptr);
    LineTo(dc, (int)ex, (int)ey);
    const float hl = (float)s.st.S(13); // arrow head: two short strokes back from the tip
    const float ang[2] = {0.7854f - 0.45f, 0.7854f + 0.45f};
    for (float a : ang) {
        MoveToEx(dc, (int)ex, (int)ey, nullptr);
        LineTo(dc, (int)(ex - hl * std::cos(a)), (int)(ey - hl * std::sin(a)));
    }
    SelectObject(dc, op);
    DeleteObject(pen);
    HBRUSH br = CreateSolidBrush(RGB(230, 50, 40));
    HPEN ring = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    ob = SelectObject(dc, br);
    op = SelectObject(dc, ring);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(ring);
    wchar_t t[96];
    swprintf_s(t, L"%d  %ls", s.hoverPin + 1, s.pins.Name(s.hoverPin));
    Fill(dc, x0, y0 + N, x0 + N, y0 + N + s.st.S(20), RGB(24, 22, 20));
    Text(dc, x0 + s.st.S(6), y0 + N + s.st.S(3), t, kText, s.st.small);
}

// Mouse on the canvas while the tab is open. Returns true when handled.
bool PhotoMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* res) {
    State& s = S();
    *res = 0;
    PhotoPane pn;
    photo::Photo* ph = ActivePhoto();
    if (!ph || !PhotoPaneOf(hwnd, &pn)) return msg == WM_LBUTTONDOWN || msg == WM_MOUSEWHEEL;
    POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg == WM_MOUSEWHEEL) {
        ScreenToClient(hwnd, &p);
        const int px = p.x >= pn.paneW && s.photoSbs ? p.x - pn.paneW : p.x, py = p.y - pn.top;
        const float u = ((float)px - pn.vw.ox) / ((float)ph->w * pn.vw.scale), v = ((float)py - pn.vw.oy) / ((float)ph->h * pn.vw.scale);
        s.pZoom = std::clamp(s.pZoom * std::pow(1.25f, (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA), 1.0f, 24.0f);
        const photo::View nv = photo::FitView(pn.paneW, pn.paneH, ph->w, ph->h, s.pZoom, 0.5f, 0.5f);
        s.pCx = u - ((float)px - 0.5f * (float)pn.paneW) / ((float)ph->w * nv.scale);
        s.pCy = v - ((float)py - 0.5f * (float)pn.paneH) / ((float)ph->h * nv.scale);
        if (s.pZoom <= 1.0f) s.pCx = s.pCy = 0.5f;
        PhotoRedraw();
        return true;
    }
    const auto imgPoint = [&](float* u, float* v) { // the pane point, as 0..1 of the photo (through its placement)
        float ix, iy;
        ph->FrameToImg(((float)p.x - pn.vw.ox) / pn.vw.scale, ((float)(p.y - pn.top) - pn.vw.oy) / pn.vw.scale, &ix, &iy);
        *u = ix / (float)ph->w; *v = iy / (float)ph->h;
    };
    switch (msg) {
    case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        SetFocus(hwnd); s.pan2d = true; s.panFrom = p; SetCapture(hwnd);
        return true;
    case WM_MBUTTONUP: case WM_RBUTTONUP:
        if (s.pan2d) { s.pan2d = false; ReleaseCapture(); }
        return true;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        if (p.x >= pn.paneW && s.photoSbs) { // the rendered head: only a custom pin is placed here
            size_t tri = 0;
            float bw[3];
            photo::Photo pickPh;
            pickPh.w = ph->w; pickPh.h = ph->h; pickPh.cam = HeadCam(*ph);
            if (s.customPlacing && photo::PickHead(pickPh, s.photoTris, s.faceCentre, pn.vw, (float)(p.x - pn.paneW), (float)(p.y - pn.top), &tri, bw)) {
                float q[3] = {0, 0, 0};
                int nearV = 0;
                for (int k = 0; k < 3; ++k) {
                    const Vertex& v = s.photoTris[tri + (size_t)k];
                    q[0] += bw[k] * v.px; q[1] += bw[k] * v.py; q[2] += bw[k] * v.pz;
                    if (bw[k] > bw[nearV]) nearV = k;
                }
                const std::vector<Vertex>& rest = s.head.Rest();
                const size_t src = s.photoSrc[tri + (size_t)nearV];
                if (src < rest.size() && src < s.headNow.size()) { // back to the rest mesh, so the pin follows the morphs
                    q[0] -= s.headNow[src].px - rest[src].px; q[1] -= s.headNow[src].py - rest[src].py; q[2] -= s.headNow[src].pz - rest[src].pz;
                }
                const int idx = s.pins.AddCustom(L"Custom " + std::to_wstring(s.pins.count - photo::kPins + 1), q, rest);
                if (idx >= 0) { s.photoPin = idx; s.photoStatus = "Custom pin added. Now click where it is on the photo."; SavePhotoSession(); }
                s.customPlacing = false;
                PhotoRedraw();
            }
            return true;
        }
        float u, v;
        imgPoint(&u, &v);
        if (u < 0 || v < 0 || u > 1 || v > 1) return true;
        int nearPin = -1;
        float nd = (float)s.st.S(10);
        for (int i = 0; i < s.pins.count; ++i) {
            if (ph->mark[i].state != 1) continue;
            float mfx, mfy;
            ph->MarkPx(i, &mfx, &mfy);
            const float dx = pn.vw.ox + mfx * pn.vw.scale - (float)p.x, dy = (float)pn.top + pn.vw.oy + mfy * pn.vw.scale - (float)p.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < nd) { nd = d; nearPin = i; }
        }
        s.photoNext = -1;
        if (nearPin >= 0) { s.photoPin = nearPin; s.dragPin = nearPin; }
        else {
            ph->mark[s.photoPin] = {u, v, 1};
            s.dragPin = s.photoPin;
            if (s.photoAutoNext)
                for (int k = 1; k < s.pins.count; ++k) {
                    const int j = (s.photoPin + k) % s.pins.count;
                    if (ph->mark[j].state == 0) { s.photoNext = j; break; }
                }
        }
        SetCapture(hwnd);
        SolveActive();
        PhotoRedraw();
        return true;
    }
    case WM_MOUSEMOVE:
        if (s.dragPin >= 0) {
            float u, v;
            imgPoint(&u, &v);
            ph->mark[s.dragPin].x = std::clamp(u, 0.0f, 1.0f);
            ph->mark[s.dragPin].y = std::clamp(v, 0.0f, 1.0f);
            ph->mark[s.dragPin].state = 1;
            SolveActive();
            PhotoRedraw();
        } else if (s.pan2d) {
            s.pCx = std::clamp(s.pCx - (float)(p.x - s.panFrom.x) / ((float)ph->w * pn.vw.scale), 0.0f, 1.0f);
            s.pCy = std::clamp(s.pCy - (float)(p.y - s.panFrom.y) / ((float)ph->h * pn.vw.scale), 0.0f, 1.0f);
            s.panFrom = p;
            PhotoRedraw();
        }
        return true;
    case WM_LBUTTONUP:
        if (s.dragPin >= 0) {
            s.dragPin = -1;
            ReleaseCapture();
            if (s.photoNext >= 0) { s.photoPin = s.photoNext; s.photoNext = -1; }
            SavePhotoSession();
            PhotoRedraw();
        }
        return true;
    case WM_CAPTURECHANGED:
        s.dragPin = -1;
        s.pan2d = false;
        return true;
    default: return false;
    }
}

void Do(int a) {
    State& s = S();
    LoadFace();
    if (a >= A_TabPhoto && a < A_PhotoSel + 16) { PhotoDo(a); return; }
    switch (a) {
    case A_TabBody: SelectTab(0); return;
    case A_TabHead: SelectTab(1); return;
    case A_TabClothes: SelectTab(2); return;
    case A_ClothBuild: StartClothBuild(); return;
    case A_ClothAuto: s.clothAuto = !s.clothAuto; break;
    case A_ClothRolled: s.shirt.rolled = !s.shirt.rolled; if (s.clothAuto) StartClothBuild(); break;
    case A_ClothTucked: s.shirt.tucked = !s.shirt.tucked; if (s.clothAuto) StartClothBuild(); break;
    case A_ClothPockets: s.shirt.pockets = !s.shirt.pockets; if (s.clothAuto) StartClothBuild(); break;
    case A_ClothBtnDown: if (s.shirt.openButtons > 0) { --s.shirt.openButtons; if (s.clothAuto) StartClothBuild(); } break;
    case A_ClothBtnUp: if (s.shirt.openButtons < 3) { ++s.shirt.openButtons; if (s.clothAuto) StartClothBuild(); } break;
    case A_ClothSeedDown: if (s.shirt.seed > 0) { --s.shirt.seed; if (s.clothAuto) StartClothBuild(); } break;
    case A_ClothSeedUp: ++s.shirt.seed; if (s.clothAuto) StartClothBuild(); break;
    case A_ClothRestore:
        if (s.clothEntity >= 0 && s.clothEntity < s.host.entityCount() && !s.clothOrigPath.empty()) {
            s.host.setEntityPath(s.clothEntity, s.clothOrigPath);
            s.clothStatus = "The scene's own shirt is back: " + StripTag(s.clothOrigPath);
            s.clothOrigPath.clear();
        } else s.clothStatus = "Nothing to restore: the made shirt did not replace a scene shirt.";
        break;
    case A_ClothSave: { const std::wstring p = RecipeDialog(true); if (!p.empty()) s.clothStatus = s.shirt.Save(p) ? "Saved the recipe to " + Narrow(p) : "Could not write the recipe."; break; }
    case A_ClothLoad: {
        const std::wstring p = RecipeDialog(false);
        std::string err;
        if (!p.empty()) { if (s.shirt.Load(p, &err)) { s.clothStatus = "Loaded " + Narrow(p); StartClothBuild(); } else s.clothStatus = "Recipe: " + err; }
        break;
    }
    case A_Random:
        if (s.face.Valid()) {
            s.values = s.face.Randomize(s.seed, s.age);
            s.presetSeed = s.seed;
            s.presetName = "random " + std::to_string(s.seed);
            ++s.seed;
            s.faceDirty = true;
        }
        break;
    case A_SeedDown: if (s.seed > 0) --s.seed; break;
    case A_SeedUp: ++s.seed; break;
    case A_BodyRandom:
        if (s.bodyLoaded) { // body_maker's rule: slider normals, then height from the next normal, then the limiter over all parts
            const std::vector<double> z = MorphSet::Normals(s.bodySeed, s.bodyValues.size() + 1);
            for (size_t i = 0; i < s.bodyValues.size(); ++i) s.bodyValues[i] = (float)(z[i] * s.parts[0].set.sliders[i].sigma);
            s.heightScale = std::clamp(1.0f + s.heightSigma * (float)z.back(), s.heightMin, s.heightMax);
            LimitBody(s.bodyValues);
            s.bodyPresetSeed = s.bodySeed++;
            s.bodyDirty = true;
        }
        break;
    case A_BodySeedDown: if (s.bodySeed > 0) --s.bodySeed; break;
    case A_BodySeedUp: ++s.bodySeed; break;
    case A_Reset:
        if (s.tab == 2) { s.shirt = clothes::ShirtRecipe(); if (s.clothAuto) StartClothBuild(); }
        else if (s.tab == 1) { std::fill(s.values.begin(), s.values.end(), 0.0f); s.age = 0; s.presetSeed = -1; s.faceDirty = true; ClearHair(); }
        else { std::fill(s.bodyValues.begin(), s.bodyValues.end(), 0.0f); s.heightScale = 1.0f; s.bodyPresetSeed = -1; s.bodyDirty = s.bodyLoaded; }
        break;
    case A_New:
        std::fill(s.values.begin(), s.values.end(), 0.0f);
        std::fill(s.bodyValues.begin(), s.bodyValues.end(), 0.0f);
        s.age = 0;
        s.heightScale = 1.0f;
        s.presetSeed = s.bodyPresetSeed = -1;
        s.presetName = "custom";
        s.extra = Json();
        s.faceDirty = true;
        s.bodyDirty = s.bodyLoaded;
        ClearHair();
        if (!s.skins.empty() && s.skin != 0) ApplySkin(0); // New is always the generic Regular human
        break;
    case A_Save: {
        if (s.tab == 2) { Do(A_ClothSave); return; }
        const std::wstring p = FileDialog(true);
        if (!p.empty()) s.status = SavePreset(p) ? "Saved " + Narrow(p) : "Could not write the preset.";
        break;
    }
    case A_Load: {
        if (s.tab == 2) { Do(A_ClothLoad); return; }
        const std::wstring p = FileDialog(false);
        if (!p.empty()) LoadPreset(p);
        break;
    }
    case A_HairFill: if (InitHair()) { s.hair.FillDefaults(); s.bakeNow = true; } break;
    case A_HairClear: ClearHair(); if (s.features) BakeHair(); break;
    case A_BrowDraw:
        s.browDraw = !s.browDraw;
        if (s.browDraw) { s.hairTool = HairPainter::None; InitHair(); }
        s.status = s.browDraw ? "Brow boundary: drag a loop around where the brow roots grow (either side; it is mirrored)." : "Brow boundary tool off.";
        break;
    case A_BrowDefault: { BrowMaker fresh; fresh.browY = s.brow.browY; fresh.seed = s.brow.seed; s.brow = fresh; s.brow.DefaultLoop(); BakeBrows(); break; }
    case A_BrowBake: BakeBrows(); break;
    case A_BrowGuides: s.browGuides = !s.browGuides; s.browGuideAt = -1e9; break;
    case A_Features:
        s.features = !s.features;
        if (s.features && !BuildFeatures()) { s.features = false; s.status = "Feature view: needs the head bound to the face morphs."; break; }
        BakeHair();
        s.status = s.features ? "Feature view on: paint inside a region's ring; the rig moves that region." : "Feature view off.";
        break;
    case A_HairBake: s.bakeNow = true; break;
    case A_InnerCorner: {
        if (!InitHair()) break;
        Json doc;
        MorphSet::ReadJson(s.facePath, doc, nullptr);
        const Json* lm = doc.Find("landmarks");
        const Json* ec = lm ? lm->Find("eye_c") : nullptr;
        float c[2][3] = {{0.02897f, 0.82122f, 0.13114f}, {-0.02897f, 0.82122f, 0.13114f}};
        if (ec && ec->a.size() >= 2) for (int e = 0; e < 2; ++e) if (ec->a[(size_t)e].a.size() >= 3) for (int j = 0; j < 3; ++j) c[e][j] = (float)ec->a[(size_t)e].a[(size_t)j].Num(c[e][j]);
        const double t0 = NowMs();
        s.sp.FillInnerCorners(c, 0.0f, 0.8f, [&](float u, float v, float* p, float* n) { return s.hair.SurfaceAt(u, v, p, n); });
        s.bakeNow = true;
        char b[120];
        snprintf(b, sizeof(b), "Inner-corner redness painted on both eyes (%.0f ms). Refine it with the Tint tool.", NowMs() - t0);
        s.status = b;
        break;
    }
    case A_PaintClear: s.sp.Clear(); s.surfDirty = true; s.bakeNow = true; s.status = "Skin paint cleared (tint, spots and shine)."; break;
    case A_EyeZoom: {
        if (!BindHead()) break;
        Json doc;
        MorphSet::ReadJson(s.facePath, doc, nullptr);
        const Json* lm = doc.Find("landmarks");
        const Json* ec = lm ? lm->Find("eye_c") : nullptr;
        float mid[3] = {0.0f, 0.82122f, 0.14f};
        if (ec && ec->a.size() >= 2 && ec->a[0].a.size() >= 3 && ec->a[1].a.size() >= 3) for (int j = 0; j < 3; ++j) mid[j] = (float)(ec->a[0].a[(size_t)j].Num(mid[j]) + ec->a[1].a[(size_t)j].Num(mid[j])) * 0.5f;
        mid[2] += 0.012f; // the face in front of the balls, so the camera orbits the lids, not the middle of the head
        float w[3];
        s.host.toWorld(s.target, mid, w);
        char zb[24] = {};
        float dist = 0.13f, offX = 0.0f; // TBD_CHAREYEZOOM=<metres>, TBD_CHAREYEX=<metres>: a closer look at one eye, for captures
        if (GetEnvironmentVariableA("TBD_CHAREYEZOOM", zb, sizeof(zb)) > 0) dist = (float)atof(zb);
        if (GetEnvironmentVariableA("TBD_CHAREYEX", zb, sizeof(zb)) > 0) offX = (float)atof(zb);
        w[0] += offX;
        s.host.frame(w, dist * s.heightScale);
        InitEye();
        s.status = "Camera on the eyes. Adjust the EYES sliders; the picture follows.";
        break;
    }
    case A_EyeReset: { s.eyePlace[0] = 0.3f; s.eyePlace[1] = 1.8f; s.eyePlace[2] = 0.0f; s.eyePlace[3] = 1.02f; s.faceDirty = true; s.eye = EyeLook{}; BakeEye(); break; } // back to the brown default
    case A_EyeIrisColor: {
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = s.host.parent;
        cc.rgbResult = RGB((int)(s.eye.irisColor[0] * 255), (int)(s.eye.irisColor[1] * 255), (int)(s.eye.irisColor[2] * 255));
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc)) break;
        s.eye.irisColor[0] = GetRValue(cc.rgbResult) / 255.0f; s.eye.irisColor[1] = GetGValue(cc.rgbResult) / 255.0f; s.eye.irisColor[2] = GetBValue(cc.rgbResult) / 255.0f;
        if (s.eye.irisAmount < 0.05f) s.eye.irisAmount = 0.85f;
        BakeEye();
        break;
    }
    case A_HairColor: PickHairColor(); break;
    case A_HairSymmetry: s.symmetry = !s.symmetry; s.status = s.symmetry ? "Symmetry on: each dab is mirrored left / right." : "Symmetry off: one side only (what is painted stays)."; break;
    case A_View3D: case A_View2D: case A_ViewBoth:
        s.view = a - A_View3D;
        if (s.view) InitHair(); // the 2D view shows the head's albedo
        if (s.host.relayout) s.host.relayout();
        break;
    default:
        if (a >= A_ClothColor && a < A_ClothColor + 4) {
            float* cols[4] = {s.shirt.base, s.shirt.stripeA, s.shirt.stripeB, s.shirt.button};
            PickClothColor(cols[a - A_ClothColor]);
        }
        else if (a >= A_Fold && a - A_Fold < (int)s.foldKeys.size()) {
            const std::wstring& key = s.foldKeys[(size_t)(a - A_Fold)];
            const auto it = s.foldOpen.find(key);
            const bool was = it == s.foldOpen.end() ? key.back() == L'+' : it->second;
            s.foldOpen[key] = !was;
        }
        else if (a >= A_LayerTool && a < A_LayerTool + 6) {
            const int t = SkinPaint::TintPaint + (a - A_LayerTool);
            s.hairTool = s.hairTool == t ? HairPainter::None : t;
            if (s.hairTool != HairPainter::None) InitHair();
        }
        else if (a >= A_TintType && a < A_TintType + SkinPaint::kTintTypes) {
            s.tintType = a - A_TintType;
            if (s.hairTool != SkinPaint::TintPaint && s.hairTool != SkinPaint::TintErase) { s.hairTool = SkinPaint::TintPaint; InitHair(); }
        }
        else if (a >= A_SpotKind && a < A_SpotKind + SkinPaint::kSpotKinds) {
            s.spotKind = a - A_SpotKind;
            if (s.hairTool != SkinPaint::SpotPaint && s.hairTool != SkinPaint::SpotErase) { s.hairTool = SkinPaint::SpotPaint; InitHair(); }
        }
        else if (a >= A_MapView && a < A_MapView + kMapCount) {
            s.mapView = a - A_MapView;
            InitHair();
            if (s.view == 0) { s.view = 2; if (s.host.relayout) s.host.relayout(); } // the 2D view shows the map: open it beside the 3D view
            BuildMapImg();
            s.bgraDirty = true;
            if (s.wnd2d) InvalidateRect(s.wnd2d, nullptr, FALSE);
        }
        else if (a >= A_Skin && a < A_Face) ApplySkin(a - A_Skin);
        else if (a >= A_Groom - 1 && a < A_Groom + (int)s.grooms.size()) ApplyGroom(a == A_Groom - 1 ? std::string("none") : s.grooms[(size_t)(a - A_Groom)]);
        else if (a > A_ClayTool && a < A_ClayTool + HairClay::ToolCount) {
            const int t = a - A_ClayTool;
            if (s.clay.Empty()) ClayLoad();
            s.clayTool = s.clayTool == t ? HairClay::None : t;
            s.hairTool = HairPainter::None;
            s.status = s.clayTool == HairClay::None ? "Clay tool off." : std::string("Clay: drag on the clay (") + ClayToolName(s.clayTool) + "). Radius, strength, hardness and symmetry are the brush's.";
        }
        else if (a == A_ClayLoad) ClayLoad();
        else if (a == A_ClayPaint || a == A_ClayErase) {
            const int t = a == A_ClayPaint ? HairPainter::Paint : HairPainter::Erase;
            s.hairTool = s.hairTool == t && s.clayMaskMode ? HairPainter::None : t;
            s.clayMaskMode = true;
            s.clayTool = HairClay::None;
            s.clayLoopDraw = false;
            InitHair();
            s.status = s.hairTool == HairPainter::None ? "Scalp painting off." : "Paint the scalp in 3D or in the 2D view (3D + 2D above). The clay follows after every stroke.";
        }
        else if (a == A_ClayFill) {
            if (InitHair()) { s.hair.FillDefaults(); s.clayMaskMode = true; s.bakeNow = true; s.status = "Default scalp painted; the clay follows it."; }
        }
        else if (a == A_ClayHairline) {
            s.clayLoopDraw = !s.clayLoopDraw;
            if (s.clayLoopDraw) { s.clayTool = HairClay::None; s.hairTool = HairPainter::None; s.browDraw = false; s.clayLoop.clear(); InitHair(); }
            s.status = s.clayLoopDraw ? "Hairline: drag a closed loop around the hair on the scalp; let go near where you started." : "Hairline drawing off.";
        }
        else if (a == A_ClayUndo) s.status = s.clay.Undo() ? "Clay: undone." : "Clay: nothing to undo.";
        else if (a == A_ClayGenerate) ClayGenerate();
        else if (a == A_ClayHair) s.clayShowHair = !s.clayShowHair;
        else if (a == A_ClayCombView) s.clayCombView = !s.clayCombView;
        else if (a > A_HairTool && a <= (int)A_HairTool + (int)HairPainter::Smooth) {
            const int t = a - A_HairTool;
            s.hairTool = s.hairTool == t ? HairPainter::None : t;
            if (s.hairTool != HairPainter::None) InitHair();
        }
        break;
    }
    Redraw();
}


// ---------------------------------------------------------------- undo (src/undo.h): one step per finished action
// Begin() before an action, Commit(label) after it: the creator's values (face, body, age, height, skin tone) and, when the hair
// is set up, the changed rectangle of the hair maps go into one step. Nothing changed: no step.
struct Params {
    std::vector<float> values, bodyValues;
    float age = 0, height = 1, tone[3] = {1, 1, 1};
    int skin = -1;
    bool operator==(const Params& o) const {
        return values == o.values && bodyValues == o.bodyValues && age == o.age && height == o.height && skin == o.skin &&
               tone[0] == o.tone[0] && tone[1] == o.tone[1] && tone[2] == o.tone[2];
    }
};
struct HairRegion { int x = 0, y = 0, w = 0, h = 0; std::vector<float> d, fx, fy, len; };
struct UndoState {
    bool begun = false;
    Params before;
    bool hairBefore = false;
    std::vector<float> d, fx, fy, len; // the hair maps at Begin (reused buffers)
};
UndoState& U() { static UndoState u; return u; }

Params CaptureParams() {
    const State& s = S();
    Params p;
    p.values = s.values; p.bodyValues = s.bodyValues; p.age = s.age; p.height = s.heightScale; p.skin = s.skin;
    for (int c = 0; c < 3; ++c) p.tone[c] = s.toneGain[c];
    return p;
}
void ApplyParams(const Params& p) {
    State& s = S();
    const bool toneChanged = p.tone[0] != s.toneGain[0] || p.tone[1] != s.toneGain[1] || p.tone[2] != s.toneGain[2];
    if (p.values.size() == s.values.size()) s.values = p.values;
    if (p.bodyValues.size() == s.bodyValues.size()) s.bodyValues = p.bodyValues;
    s.age = p.age; s.heightScale = p.height; s.skin = p.skin;
    for (int c = 0; c < 3; ++c) s.toneGain[c] = p.tone[c];
    s.faceDirty = true;
    s.bodyDirty = s.bodyLoaded;
    if (toneChanged) {
        if (s.hairReady) { ToneAlbedo(s.albedo, s.baseAlbedo); s.bakeNow = true; }
        ToneBody();
    }
    tbd::undo::AbsorbSceneChange(); // the height scale moves the entities' scale: this step owns it
    Redraw();
}
HairRegion Cut(const std::vector<float>& d, const std::vector<float>& fx, const std::vector<float>& fy, const std::vector<float>& len, int x, int y, int w, int h) {
    HairRegion r{x, y, w, h};
    const size_t n = (size_t)w * h;
    r.d.resize(n); r.fx.resize(n); r.fy.resize(n); r.len.resize(n);
    for (int j = 0; j < h; ++j) {
        const size_t o = (size_t)(y + j) * HairPainter::N + (size_t)x, b = (size_t)j * w;
        std::copy_n(&d[o], w, &r.d[b]); std::copy_n(&fx[o], w, &r.fx[b]); std::copy_n(&fy[o], w, &r.fy[b]); std::copy_n(&len[o], w, &r.len[b]);
    }
    return r;
}
void Paste(const HairRegion& r) {
    State& s = S();
    if (!s.hair.Ready()) return;
    for (int j = 0; j < r.h; ++j) {
        const size_t o = (size_t)(r.y + j) * HairPainter::N + (size_t)r.x, b = (size_t)j * r.w;
        std::copy_n(&r.d[b], r.w, &s.hair.density[o]); std::copy_n(&r.fx[b], r.w, &s.hair.fx[o]);
        std::copy_n(&r.fy[b], r.w, &s.hair.fy[o]); std::copy_n(&r.len[b], r.w, &s.hair.len[o]);
    }
    s.hair.painted = true;
    s.bakeNow = true;
    Redraw();
}

void Begin() {
    State& s = S();
    UndoState& u = U();
    u.begun = true;
    u.before = CaptureParams();
    u.hairBefore = s.hairReady && s.hair.Ready();
    if (u.hairBefore) { u.d = s.hair.density; u.fx = s.hair.fx; u.fy = s.hair.fy; u.len = s.hair.len; }
}
void Commit(const std::string& label) {
    State& s = S();
    UndoState& u = U();
    if (!u.begun) return;
    u.begun = false;
    const Params after = CaptureParams();
    const bool params = !(after == u.before);
    std::shared_ptr<HairRegion> hb, ha;
    if (u.hairBefore && s.hair.Ready() && u.d.size() == s.hair.density.size()) {
        constexpr int N = HairPainter::N;
        int x0 = N, y0 = N, x1 = -1, y1 = -1;
        for (int y = 0; y < N; ++y) {
            const size_t row = (size_t)y * N;
            for (int x = 0; x < N; ++x) {
                const size_t i = row + (size_t)x;
                if (u.d[i] != s.hair.density[i] || u.fx[i] != s.hair.fx[i] || u.fy[i] != s.hair.fy[i] || u.len[i] != s.hair.len[i]) {
                    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
            }
        }
        if (x1 >= 0) {
            hb = std::make_shared<HairRegion>(Cut(u.d, u.fx, u.fy, u.len, x0, y0, x1 - x0 + 1, y1 - y0 + 1));
            ha = std::make_shared<HairRegion>(Cut(s.hair.density, s.hair.fx, s.hair.fy, s.hair.len, x0, y0, x1 - x0 + 1, y1 - y0 + 1));
        }
    }
    if (!params && !hb) return;
    auto pb = std::make_shared<Params>(u.before), pa = std::make_shared<Params>(after);
    tbd::undo::Push(label,
        [pb, hb, params] { if (params) ApplyParams(*pb); if (hb) Paste(*hb); },
        [pa, ha, params] { if (params) ApplyParams(*pa); if (ha) Paste(*ha); });
    if (params) tbd::undo::AbsorbSceneChange();
}
std::string ActionLabel(int a) {
    const State& s = S();
    if (a >= A_Face && a < A_Body && a - A_Face < (int)s.face.sliders.size()) return s.face.sliders[(size_t)(a - A_Face)].label;
    if (a >= A_Body && a < A_Fold && a - A_Body < (int)s.parts[0].set.sliders.size()) {
        const MorphSlider& sl = s.parts[0].set.sliders[(size_t)(a - A_Body)];
        return sl.label.empty() ? sl.name : sl.label;
    }
    if (a >= A_Skin && a < A_Face && a - A_Skin < (int)s.skins.size()) return "Skin tone " + s.skins[(size_t)(a - A_Skin)];
    if (a == A_Groom - 1) return "Hair style: none";
    if (a >= A_Groom && a - A_Groom < (int)s.grooms.size()) return "Hair style " + s.grooms[(size_t)(a - A_Groom)];
    switch (a) {
    case A_Age: return "Age";
    case A_Height: return "Height";
    case A_Random: return "Randomize face";
    case A_BodyRandom: return "Randomize body";
    case A_Reset: return "Reset";
    case A_New: return "New character";
    case A_Load: return "Load preset";
    case A_HairFill: return "Fill hair defaults";
    case A_HairClear: return "Clear hair";
    default: return "Character edit";
    }
}
const char* ToolName(int t) { if (SkinPaint::IsLayerTool(t)) return "Skin paint"; return t == HairPainter::Paint ? "Paint hair" : t == HairPainter::Erase ? "Erase hair" : t == HairPainter::Comb ? "Comb hair" : "Smooth hair"; }

// ---------------------------------------------------------------- drawing
int TopH() { return S().st.S(180); }

void Button(HDC dc, RECT r, const std::wstring& label, int action, bool on = false, bool big = false) {
    State& s = S();
    Fill(dc, r.left, r.top, r.right, r.bottom, on ? kAccent : RGB(52, 46, 40));
    Text(dc, (r.left + r.right) / 2, r.top + (r.bottom - r.top) / 2 - s.st.S(big ? 9 : 7), label, kText, big ? s.st.font : s.st.small, TA_CENTER);
    if (action) s.hits.push_back({r, action});
}

RECT Track(int x, int w) {
    const Style& st = S().st;
    return {x + st.S(122), 0, x + w - st.S(44), 0};
}

void SliderRow(HDC dc, int x, int y, int w, const std::wstring& label, float v, float lo, float hi, float tickLo, float tickHi, int action, const wchar_t* fmt = L"%+.2f") {
    State& s = S();
    const int rowH = s.st.S(20);
    Text(dc, x, y + s.st.S(2), label, kMuted, s.st.small);
    RECT tr = Track(x, w);
    tr.top = y + s.st.S(5);
    tr.bottom = y + rowH - s.st.S(5);
    const int mid = (tr.top + tr.bottom) / 2;
    Fill(dc, tr.left, mid - 1, tr.right, mid + 1, kPanel);
    const auto X = [&](float a) { return tr.left + (int)((a - lo) / (hi - lo) * (float)(tr.right - tr.left)); };
    if (tickHi > tickLo) {
        Fill(dc, X(tickLo), mid - s.st.S(4), X(tickLo) + 1, mid + s.st.S(4), kMuted);
        Fill(dc, X(tickHi), mid - s.st.S(4), X(tickHi) + 1, mid + s.st.S(4), kMuted);
    }
    const int kx = X(std::clamp(v, lo, hi));
    Fill(dc, kx - s.st.S(3), tr.top, kx + s.st.S(3), tr.bottom, kAccent);
    wchar_t b[24];
    swprintf_s(b, fmt, v);
    Text(dc, x + w, y + s.st.S(2), b, kText, s.st.small, TA_RIGHT);
    RECT hit = {tr.left - s.st.S(6), y, tr.right + s.st.S(6), y + rowH};
    s.hits.push_back({hit, action});
}

// The scrolling body of one tab, drawn from y; returns the y after it.
int DrawBody(HDC dc, int m, int W, int y) {
    State& s = S();
    const int row = s.st.S(20);
    const COLORREF kOff = RGB(200, 120, 90);
    // Drop-downs: a clickable bar with an arrow. key ends in '+' when the default is open.
    const auto fold = [&](const std::wstring& title, bool defOpen, bool big) {
        const std::wstring key = std::to_wstring(s.tab) + L"|" + title + (defOpen ? L"+" : L"-");
        const auto it = s.foldOpen.find(key);
        const bool isOpen = it == s.foldOpen.end() ? defOpen : it->second;
        const int h = big ? s.st.S(26) : s.st.S(20), x = big ? m : m + s.st.S(4);
        if (big) y += s.st.S(6);
        Fill(dc, x, y, m + W, y + h, big ? RGB(44, 39, 34) : RGB(36, 32, 29));
        Text(dc, x + s.st.S(6), y + (big ? s.st.S(4) : s.st.S(3)), std::wstring(isOpen ? L"\u25BE  " : L"\u25B8  ") + title, big ? kAccent : kText, big ? s.st.font : s.st.small);
        s.hits.push_back({{x, y, m + W, y + h}, A_Fold + (int)s.foldKeys.size()});
        s.foldKeys.push_back(key);
        y += h + s.st.S(big ? 6 : 2);
        return isOpen;
    };
    const auto header = [&](const wchar_t* t, bool defOpen = true) { return fold(t, defOpen, true); };
    const auto note = [&](const std::wstring& t, COLORREF c) { Text(dc, m, y, t, c, s.st.small); y += s.st.S(17); };
    const auto seedRow = [&](int random, int down, int up, uint32_t seed) {
        Button(dc, {m, y, m + s.st.S(90), y + s.st.S(22)}, L"Randomize", random);
        Button(dc, {m + s.st.S(96), y, m + s.st.S(118), y + s.st.S(22)}, L"<", down);
        wchar_t sd[32];
        swprintf_s(sd, L"seed %u", seed);
        Text(dc, m + s.st.S(154), y + s.st.S(4), sd, kText, s.st.small, TA_CENTER);
        Button(dc, {m + s.st.S(190), y, m + s.st.S(212), y + s.st.S(22)}, L">", up);
        y += s.st.S(28);
    };
    if (s.tab == 3) { // PHOTO FIT: photos, the pin list, the view (photo_fit.h)
        const COLORREF kOk = RGB(140, 200, 120), kHide = RGB(220, 170, 90);
        photo::Photo* ph = ActivePhoto();
        if (header(L"PHOTOS")) {
            const int bw = (W - 2 * s.st.S(6)) / 3;
            Button(dc, {m, y, m + bw, y + s.st.S(24)}, L"Add photos", A_PhotoAdd);
            Button(dc, {m + bw + s.st.S(6), y, m + 2 * bw + s.st.S(6), y + s.st.S(24)}, L"Remove", A_PhotoRemove);
            Button(dc, {m + 2 * (bw + s.st.S(6)), y, m + W, y + s.st.S(24)}, ph ? Utf8ToWide(ph->label) : L"Angle", A_PhotoLabel);
            y += s.st.S(30);
            for (size_t i = 0; i < s.photos.size() && i < 16; ++i) {
                const photo::Photo& p = s.photos[i];
                const size_t cut = p.path.find_last_of(L"\\/");
                wchar_t t[96];
                swprintf_s(t, L"%d  %ls   (%d pins)", (int)i + 1, p.path.substr(cut == std::wstring::npos ? 0 : cut + 1).substr(0, 28).c_str(), p.Marked());
                Button(dc, {m, y, m + W, y + s.st.S(20)}, t, A_PhotoSel + (int)i, (int)i == s.photoActive);
                y += s.st.S(23);
            }
            if (ph) {
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Photo size", ph->size, 0.25f, 4.0f, 0, 0, A_PhotoSize, L"%.2fx");
                y += row;
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Move left / right", ph->offx * 100.0f, -50.0f, 50.0f, 0, 0, A_PhotoOffX, L"%+.0f %%");
                y += row;
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Move up / down", ph->offy * 100.0f, -50.0f, 50.0f, 0, 0, A_PhotoOffY, L"%+.0f %%");
                y += row;
                Button(dc, {m, y, m + W, y + s.st.S(22)}, L"Reset size and position", A_PhotoPlaceReset);
                y += s.st.S(28);
                note(L"Size the photo against the head before marking pins; marks follow it.", kMuted);
            }
            if (s.photos.empty()) note(L"No photos yet. Select several angles at once.", kMuted);
            else note(L"The angle label is only a note; the camera comes from the pins.", kMuted);
        }
        if (header(L"PINS: PICK ONE, THEN CLICK THE PHOTO")) {
            if (!s.pins.ok) note(L"The head's pin points are missing (no face found on the head).", kOff);
            const int bw = (W - 2 * s.st.S(6)) / 3;
            Button(dc, {m, y, m + bw, y + s.st.S(22)}, L"Not visible", A_PhotoHide, ph && ph->mark[s.photoPin].state == 2);
            Button(dc, {m + bw + s.st.S(6), y, m + 2 * bw + s.st.S(6), y + s.st.S(22)}, L"Clear pin", A_PhotoClear);
            Button(dc, {m + 2 * (bw + s.st.S(6)), y, m + W, y + s.st.S(22)}, s.photoAutoNext ? L"Auto-next ON" : L"Auto-next OFF", A_PhotoAuto, s.photoAutoNext);
            y += s.st.S(28);
            Button(dc, {m, y, m + (W - s.st.S(6)) / 2, y + s.st.S(22)}, s.customPlacing ? L"Click the head (right)..." : L"+ Custom pin", A_PhotoCustom, s.customPlacing);
            Button(dc, {m + (W - s.st.S(6)) / 2 + s.st.S(6), y, m + W, y + s.st.S(22)}, L"Remove custom pin", A_PhotoCustomDel);
            y += s.st.S(28);
            note(s.pins.Hint(s.photoPin), kMuted);
            for (int i = 0; i < s.pins.count; ++i) {
                const uint8_t st = ph ? ph->mark[i].state : 0;
                const RECT rr = {m, y, m + W, y + s.st.S(19)};
                if (i == s.photoPin) Fill(dc, rr.left, rr.top, rr.right, rr.bottom, RGB(58, 50, 42));
                Fill(dc, m + s.st.S(4), y + s.st.S(5), m + s.st.S(13), y + s.st.S(14), st == 1 ? kOk : st == 2 ? kHide : RGB(70, 66, 60));
                wchar_t t[80];
                swprintf_s(t, L"%d  %ls", i + 1, s.pins.Name(i));
                Text(dc, m + s.st.S(20), y + s.st.S(2), t, i == s.photoPin ? kText : kMuted, s.st.small);
                if (ph && ph->cam.ok && ph->cam.err[i] >= 0) {
                    wchar_t e[24];
                    swprintf_s(e, L"%.1f px", ph->cam.err[i]);
                    Text(dc, m + W - s.st.S(4), y + s.st.S(2), e, ph->cam.err[i] > 8.0f ? kOff : kOk, s.st.small, TA_RIGHT);
                } else if (st == 2) Text(dc, m + W - s.st.S(4), y + s.st.S(2), L"hidden", kHide, s.st.small, TA_RIGHT);
                s.hits.push_back({rr, A_PhotoPin + i});
                y += s.st.S(20);
            }
            y += s.st.S(4);
            note(L"Mark only what you can see exactly; skip the rest.", kMuted);
        }
        if (header(L"SHAPE: FIT THE FACE TO THE PINS")) {
            Button(dc, {m, y, m + W, y + s.st.S(26)}, L"Fit face shape to the pins", A_PhotoShape, false, true);
            y += s.st.S(32);
            Button(dc, {m, y, m + W, y + s.st.S(22)}, L"Back to the neutral face", A_PhotoShapeReset);
            y += s.st.S(28);
            note(L"Moves the face sliders (HEAD tab) so the head's pins land on", kMuted);
            note(L"your marks, over every photo. Needs 4+ pins on a photo.", kMuted);
            note(L"Depth (nose length, brow) stays neutral without side photos.", kMuted);
        }
        if (header(L"SKIN COLOUR FROM THE PHOTOS")) {
            const int bw = (W - s.st.S(6)) / 2;
            Button(dc, {m, y, m + bw, y + s.st.S(26)}, L"Read colours", A_PhotoSkinRead, false, true);
            Button(dc, {m + bw + s.st.S(6), y, m + W, y + s.st.S(26)}, L"Project photo", A_PhotoSkinProject, false, true);
            y += s.st.S(32);
            SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Strength", s.skinStrength, 0.0f, 1.0f, 0, 0, A_PhotoSkinStr, L"%.2f");
            y += row;
            SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Warmth", s.skinWarm, -1.0f, 1.0f, 0, 0, A_PhotoSkinWarm, L"%+.2f");
            y += row;
            SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Brightness (stops)", s.skinExp, -2.0f, 2.0f, 0, 0, A_PhotoSkinExp, L"%+.1f");
            y += row;
            note(L"Press Read / Project again after changing a slider.", kMuted);
            Button(dc, {m, y, m + W, y + s.st.S(22)}, L"Remove photo skin colour", A_PhotoSkinClear, s.sp.gainPainted);
            y += s.st.S(28);
            if (s.skinRep.ok) {
                const wchar_t* zn[5] = {L"Forehead", L"Right cheek", L"Left cheek", L"Nose", L"Chin"};
                const int sw = (W - 4 * s.st.S(4)) / 5;
                for (int k = 0; k < 5; ++k) {
                    const int x0 = m + k * (sw + s.st.S(4));
                    if (s.skinRep.zoneOk[k]) Fill(dc, x0, y, x0 + sw, y + s.st.S(18), RGB((int)s.skinRep.zoneRgb[k][0], (int)s.skinRep.zoneRgb[k][1], (int)s.skinRep.zoneRgb[k][2]));
                    else Fill(dc, x0, y, x0 + sw, y + s.st.S(18), RGB(50, 46, 42));
                    Text(dc, x0 + sw / 2, y + s.st.S(19), zn[k], kMuted, s.st.small, TA_CENTER);
                }
                y += s.st.S(40);
                if (s.skinRep.unevenLight) note(L"Warning: very uneven light in a photo: colours may be off.", kHide);
            }
            note(L"Read colours: the person's skin tone and its slow changes.", kMuted);
            note(L"Project photo: also blotches, redness patches and moles.", kMuted);
            note(L"The photo's light is removed first; the base skin's pores stay.", kMuted);
        }
        if (header(L"VIEW")) {
            const int bw = (W - s.st.S(6)) / 2;
            Button(dc, {m, y, m + bw, y + s.st.S(24)}, L"Overlay on photo", A_PhotoOverlay, !s.photoSbs);
            Button(dc, {m + bw + s.st.S(6), y, m + W, y + s.st.S(24)}, L"Side by side", A_PhotoSbs, s.photoSbs);
            y += s.st.S(30);
            Button(dc, {m, y, m + W, y + s.st.S(22)}, L"Fit view (reset zoom)", A_PhotoFit);
            y += s.st.S(28);
            SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Wire strength", s.wireAlpha, 0.0f, 1.0f, 0, 0, A_PhotoWire, L"%.2f");
            y += row;
            SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Head rotation", s.viewYaw * 57.2958f, -90.0f, 90.0f, 0, 0, A_PhotoYaw, L"%+.0f deg");
            y += row;
            if (ph) { SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), L"Lens (field of view)", ph->cam.fov, 10.0f, 60.0f, 0, 0, A_PhotoFov, L"%.0f deg"); y += row; }
            note(L"Lens: a phone photo is about 60-70 deg wide; portraits ~25.", kMuted);
        }
        {
            const std::wstring st = Utf8ToWide(s.photoStatus);
            for (size_t at = 0; at < st.size(); at += 58) note(st.substr(at, 58), ph && ph->cam.ok ? kOk : kMuted);
        }
        return y + s.st.S(12);
    }
    if (s.tab == 2) { // CLOTHES: the Clothes Maker (clothes_maker.h)
        const COLORREF kOk = RGB(140, 200, 120);
        if (header(L"GARMENT")) {
            const int bw = (W - 2 * s.st.S(6)) / 3;
            Button(dc, {m, y, m + bw, y + s.st.S(24)}, L"Button-up shirt", 0, true);
            Fill(dc, m + bw + s.st.S(6), y, m + 2 * bw + s.st.S(6), y + s.st.S(24), RGB(40, 36, 32));
            Text(dc, m + bw + s.st.S(6) + bw / 2, y + s.st.S(5), L"Jeans (next)", kMuted, s.st.small, TA_CENTER);
            Fill(dc, m + 2 * (bw + s.st.S(6)), y, m + W, y + s.st.S(24), RGB(40, 36, 32));
            Text(dc, m + 2 * (bw + s.st.S(6)) + bw / 2, y + s.st.S(5), L"Coat (next)", kMuted, s.st.small, TA_CENTER);
            y += s.st.S(30);
            Button(dc, {m, y, m + W - s.st.S(96), y + s.st.S(30)}, s.clothBusy ? L"BUILDING..." : L"BUILD SHIRT", A_ClothBuild, s.clothBusy, true);
            Button(dc, {m + W - s.st.S(90), y, m + W, y + s.st.S(30)}, s.clothAuto ? L"Auto ON" : L"Auto OFF", A_ClothAuto, s.clothAuto);
            y += s.st.S(36);
            const std::wstring st = Utf8ToWide(s.clothStatus);
            for (size_t at = 0; at < st.size(); at += 60) note(st.substr(at, 60), s.clothRes.ok && !s.clothBusy ? kOk : kMuted);
            if (!s.clothOrigPath.empty()) { Button(dc, {m, y, m + W, y + s.st.S(22)}, L"Restore the scene's own shirt", A_ClothRestore); y += s.st.S(28); }
        }
        const auto toggles = [&](int act0, const wchar_t* on0, const wchar_t* off0, bool v0, int act1, const wchar_t* on1, const wchar_t* off1, bool v1) {
            const int bw = (W - s.st.S(6)) / 2;
            Button(dc, {m, y, m + bw, y + s.st.S(22)}, v0 ? on0 : off0, act0, v0);
            Button(dc, {m + bw + s.st.S(6), y, m + W, y + s.st.S(22)}, v1 ? on1 : off1, act1, v1);
            y += s.st.S(28);
        };
        const auto sliders = [&](const wchar_t* group) {
            for (int k = 0; k < kClothSliderCount; ++k) {
                const ClothSlider& c = kClothSliders[k];
                if (wcscmp(c.group, group) != 0) continue;
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), c.label, s.shirt.*c.field * c.show, c.lo * c.show, c.hi * c.show, 0, 0, A_Cloth + k, c.fmt);
                y += row;
            }
        };
        if (header(L"FIT")) {
            toggles(A_ClothRolled, L"Sleeves rolled", L"Sleeves down", s.shirt.rolled, A_ClothTucked, L"Tucked in jeans", L"Worn out", s.shirt.tucked);
            sliders(L"FIT");
            note(L"Sleeve length: 0 shoulder, 100 wrist (the elbow is ~50).", kMuted);
        }
        if (header(L"COLLAR & FRONT")) {
            sliders(L"COLLAR");
            Button(dc, {m, y, m + s.st.S(22), y + s.st.S(22)}, L"<", A_ClothBtnDown);
            wchar_t b[48];
            swprintf_s(b, L"%d button%s undone", s.shirt.openButtons, s.shirt.openButtons == 1 ? L"" : L"s");
            Text(dc, m + s.st.S(30), y + s.st.S(4), b, kText, s.st.small);
            Button(dc, {m + s.st.S(150), y, m + s.st.S(172), y + s.st.S(22)}, L">", A_ClothBtnUp);
            Button(dc, {m + s.st.S(180), y, m + W, y + s.st.S(22)}, s.shirt.pockets ? L"Chest pockets ON" : L"Chest pockets OFF", A_ClothPockets, s.shirt.pockets);
            y += s.st.S(28);
        }
        if (header(L"WRINKLES")) {
            sliders(L"WRINKLES");
            Button(dc, {m, y, m + s.st.S(22), y + s.st.S(22)}, L"<", A_ClothSeedDown);
            wchar_t b[32];
            swprintf_s(b, L"fold seed %u", s.shirt.seed);
            Text(dc, m + s.st.S(30), y + s.st.S(4), b, kText, s.st.small);
            Button(dc, {m + s.st.S(150), y, m + s.st.S(172), y + s.st.S(22)}, L">", A_ClothSeedUp);
            y += s.st.S(28);
            note(L"Folds gather up from the waistband, bunch above the rolled", kMuted);
            note(L"cuffs and crease the inner elbow; the jeans set the waist.", kMuted);
        }
        if (header(L"FABRIC")) {
            const wchar_t* names[4] = {L"Cloth", L"Light stripe", L"Dark stripe", L"Buttons"};
            float* cols[4] = {s.shirt.base, s.shirt.stripeA, s.shirt.stripeB, s.shirt.button};
            const int bw = (W - 3 * s.st.S(6)) / 4;
            const auto g = [](float v) { return (int)std::lround(std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f); };
            for (int k = 0; k < 4; ++k) {
                const int x0 = m + k * (bw + s.st.S(6));
                Fill(dc, x0, y, x0 + bw, y + s.st.S(18), RGB(g(cols[k][0]), g(cols[k][1]), g(cols[k][2])));
                s.hits.push_back({{x0, y, x0 + bw, y + s.st.S(34)}, A_ClothColor + k});
                Text(dc, x0 + bw / 2, y + s.st.S(19), names[k], kMuted, s.st.small, TA_CENTER);
            }
            y += s.st.S(40);
            sliders(L"FABRIC");
        }
        if (header(L"FILES", false)) {
            note(L"Save / Load at the top = this recipe (JSON).", kMuted);
            note(L"Each build writes Assets\\clothes\\" + Utf8ToWide(s.shirt.name) + L"\\:", kMuted);
            note(L"  " + Utf8ToWide(s.shirt.name) + L".glb (skinned), albedo.png, normal.png,", kMuted);
            note(L"  recipe.json. The scene's Shirt entity points at the GLB.", kMuted);
        }
        return y + s.st.S(12);
    }
    if (s.tab == 1) {
        if (s.features) { // legend for the overlay (turned on from the Features button at the top)
            Fill(dc, m, y, m + W, y + s.st.S(22), RGB(44, 39, 34));
            Text(dc, m + s.st.S(6), y + s.st.S(4), L"FEATURE OVERLAY  (Features button, top)", kAccent, s.st.small);
            y += s.st.S(26);
            if (s.face.Valid()) {
                const int cw = (W - 2 * s.st.S(6)) / 3;
                int k = 0;
                for (const std::string& g : s.face.groups) {
                    if (g == "Overall") continue;
                    const int cx = m + (k % 3) * (cw + s.st.S(6)), cy = y + (k / 3) * s.st.S(20);
                    Fill(dc, cx, cy + s.st.S(3), cx + s.st.S(12), cy + s.st.S(15), FeatureColor(g));
                    Text(dc, cx + s.st.S(18), cy + s.st.S(2), Utf8ToWide(g), kText, s.st.small);
                    ++k;
                }
                y += ((k + 2) / 3) * s.st.S(20);
            }
            Fill(dc, m, y + s.st.S(3), m + s.st.S(12), y + s.st.S(15), RGB(255, 40, 200));
            Text(dc, m + s.st.S(18), y + s.st.S(2), L"Eyebrow zone: paint brows inside this ring", kText, s.st.small);
            y += s.st.S(22);
            note(L"Colour = which face sliders move that skin; dark lines = mesh.", kMuted);
            y += s.st.S(6);
        }
        if (!header(L"FACE SHAPE")) {}
        else if (!s.face.Valid()) note(Utf8ToWide(s.faceError), kOff);
        else {
            seedRow(A_Random, A_SeedDown, A_SeedUp, s.seed);
            SliderRow(dc, m, y, W, L"Age", s.age, 0.0f, 1.0f, 0, 0, A_Age);
            y += row;
            for (const std::string& g : s.face.groups) {
                if (!fold(Utf8ToWide(g), false, false)) continue;
                for (size_t i = 0; i < s.face.sliders.size(); ++i)
                    if (s.face.sliders[i].group == g) {
                        SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), Utf8ToWide(s.face.sliders[i].label), s.values[i], -1.0f, 1.0f, s.face.sliders[i].lo, s.face.sliders[i].hi, A_Face + (int)i);
                        y += row;
                    }
            }
        }
        if (header(L"BROWS (DRAW, BAKE TO 10 CARDS)")) {
            const int bw4 = (W - 3 * s.st.S(6)) / 4;
            const wchar_t* bl[4] = {s.browDraw ? L"Drawing..." : L"Draw", L"Default", L"Bake", L"Guides"};
            for (int k = 0; k < 4; ++k)
                Button(dc, {m + k * (bw4 + s.st.S(6)), y, m + k * (bw4 + s.st.S(6)) + bw4, y + s.st.S(24)}, bl[k], A_BrowDraw + k,
                       (k == 0 && s.browDraw) || (k == 3 && s.browGuides));
            y += s.st.S(30);
            if (s.browGuides) { // the guide colours, as drawn on the head
                const struct { COLORREF c; const wchar_t* t; } key[5] = {{RGB(255, 50, 200), L"boundary"}, {RGB(60, 140, 255), L"arch"},
                    {RGB(70, 230, 90), L"inner"}, {RGB(255, 220, 40), L"peak"}, {RGB(255, 70, 60), L"tail"}};
                int kx = m;
                for (const auto& k : key) {
                    Fill(dc, kx, y + s.st.S(4), kx + s.st.S(10), y + s.st.S(14), k.c);
                    Text(dc, kx + s.st.S(13), y + s.st.S(1), k.t, kText, s.st.small);
                    kx += s.st.S(13) + (int)wcslen(k.t) * s.st.S(6) + s.st.S(10);
                }
                y += s.st.S(20);
            }
            for (int k = 0; k < kBrowSliderCount; ++k) {
                const BrowSlider& b = kBrowSliders[k];
                if (k == 2) note(s.brow.custom ? L"Shape (drawn boundary: these wait for Default shape)" : L"Shape", s.brow.custom ? kOff : kMuted);
                if (k == 9) note(L"Hairs", kMuted);
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), b.label, s.brow.*b.field, b.lo, b.hi, 0, 0, A_BrowDensity + k, b.fmt);
                y += row;
            }
            note(s.browDraw ? L"Drag a loop on the head: roots grow inside it." : L"Draw the root boundary, or keep the default shape.", kMuted);
            note(L"Sliders and drawing re-bake when you let go.", kMuted);
            if (s.browBaked) {
                wchar_t b[96];
                swprintf_s(b, L"Baked: %d hairs, %ls shape", s.brow.hairCount, s.brow.custom ? L"drawn" : L"default");
                note(b, kText);
            }
        }
        if (header(L"HAIR & BROWS")) {
            const wchar_t* tools[4] = {L"Paint", L"Erase", L"Comb", L"Smooth"};
            const int bw = (W - 3 * s.st.S(6)) / 4;
            for (int k = 0; k < 4; ++k)
                Button(dc, {m + k * (bw + s.st.S(6)), y, m + k * (bw + s.st.S(6)) + bw, y + s.st.S(24)}, tools[k], A_HairTool + 1 + k, s.hairTool == 1 + k);
            y += s.st.S(30);
            Button(dc, {m, y, m + W, y + s.st.S(22)}, s.symmetry ? L"Symmetry: ON (mirrors left / right)" : L"Symmetry: OFF", A_HairSymmetry, s.symmetry);
            y += s.st.S(28);
            const wchar_t* acts[3] = {L"Fill defaults", L"Clear", L"Bake"};
            const int cw = (W - 2 * s.st.S(6)) / 3;
            for (int k = 0; k < 3; ++k)
                Button(dc, {m + k * (cw + s.st.S(6)), y, m + k * (cw + s.st.S(6)) + cw, y + s.st.S(22)}, acts[k], A_HairFill + k);
            y += s.st.S(28);
            SliderRow(dc, m, y, W, L"Brush radius mm", s.hairRadius, 2.0f, 40.0f, 0, 0, A_HairRadius, L"%.0f");
            y += row;
            SliderRow(dc, m, y, W, L"Strength", s.hairStrength, 0.0f, 1.0f, 0, 0, A_HairStrength, L"%.2f");
            y += row;
            SliderRow(dc, m, y, W, L"Hardness", s.hairHardness, 0.0f, 1.0f, 0, 0, A_HairHardness, L"%.2f");
            y += row;
            Text(dc, m, y + s.st.S(4), L"Colour", kMuted, s.st.small);
            const RECT sw = {m + s.st.S(122), y + s.st.S(2), m + s.st.S(200), y + s.st.S(20)};
            Fill(dc, sw.left, sw.top, sw.right, sw.bottom, RGB((int)(s.hair.color[0] * 255), (int)(s.hair.color[1] * 255), (int)(s.hair.color[2] * 255)));
            s.hits.push_back({sw, A_HairColor});
            Text(dc, sw.right + s.st.S(8), y + s.st.S(4), Utf8ToWide(HexColor(s.hair.color)) + L" (click)", kMuted, s.st.small);
            y += s.st.S(26);
            note(s.hairTool ? L"Left-drag on the head paints; Alt+drag orbits." : L"Pick a tool, then left-drag on the head.", kMuted);
            if (s.bakedTex != kNoTexture) {
                wchar_t b[96];
                swprintf_s(b, L"Last bake: composite %.0f ms + texture %.0f ms", s.bakeMs, s.texMs);
                note(b, kMuted);
            }
            note(L"Painted hair is flat (a texture layer, no strand volume).", kOff);
            note(L"Brow hair cards load with the head (brow_cards.py).", kMuted);
        }
        if (header(L"SKIN PAINT (TINT, SPOTS, SHINE)")) {
            const wchar_t* tn[6] = {L"Tint", L"Erase tint", L"Spots", L"Erase spots", L"Shine", L"Matte"};
            const int tw = (W - 2 * s.st.S(6)) / 3;
            for (int k = 0; k < 6; ++k) {
                const int cx = m + (k % 3) * (tw + s.st.S(6)), cy = y + (k / 3) * s.st.S(28);
                Button(dc, {cx, cy, cx + tw, cy + s.st.S(24)}, tn[k], A_LayerTool + k, s.hairTool == SkinPaint::TintPaint + k);
            }
            y += 2 * s.st.S(28);
            note(L"Skin look to paint (inner corner of the eye, blush, shadow):", kMuted);
            const int cw2 = (W - s.st.S(6)) / 2;
            for (int k = 0; k < SkinPaint::kTintTypes; ++k) {
                const int cx = m + (k % 2) * (cw2 + s.st.S(6)), cy = y + (k / 2) * s.st.S(26);
                const SkinPaint::TintType& t = SkinPaint::Tint(k);
                Button(dc, {cx, cy, cx + cw2, cy + s.st.S(22)}, std::wstring(L"      ") + t.name, A_TintType + k, s.tintType == k);
                Fill(dc, cx + s.st.S(6), cy + s.st.S(4), cx + s.st.S(22), cy + s.st.S(18), RGB(t.rgb[0], t.rgb[1], t.rgb[2]));
            }
            y += (SkinPaint::kTintTypes / 2) * s.st.S(26) + s.st.S(4);
            note(L"Dark spots (paint where they may appear):", kMuted);
            const int sw3 = (W - 2 * s.st.S(6)) / 3;
            for (int k = 0; k < SkinPaint::kSpotKinds; ++k)
                Button(dc, {m + k * (sw3 + s.st.S(6)), y, m + k * (sw3 + s.st.S(6)) + sw3, y + s.st.S(22)}, SkinPaint::SpotName(k), A_SpotKind + k, s.spotKind == k);
            y += s.st.S(28);
            SliderRow(dc, m, y, W, L"Brush radius mm", s.hairRadius, 2.0f, 40.0f, 0, 0, A_HairRadius, L"%.0f");
            y += row;
            SliderRow(dc, m, y, W, L"Strength", s.hairStrength, 0.0f, 1.0f, 0, 0, A_HairStrength, L"%.2f");
            y += row;
            SliderRow(dc, m, y, W, L"Hardness", s.hairHardness, 0.0f, 1.0f, 0, 0, A_HairHardness, L"%.2f");
            y += row;
            Button(dc, {m, y, m + cw2, y + s.st.S(24)}, L"Auto inner corners", A_InnerCorner);
            Button(dc, {m + cw2 + s.st.S(6), y, m + W, y + s.st.S(24)}, L"Clear paint", A_PaintClear);
            y += s.st.S(30);
            Button(dc, {m, y, m + W, y + s.st.S(24)}, s.features ? L"Feature view: ON (paint inside the region rings)" : L"Feature view: OFF (click to show regions)", A_Features, s.features);
            y += s.st.S(30);
            Button(dc, {m, y, m + W, y + s.st.S(22)}, s.symmetry ? L"Symmetry: ON (mirrors left / right)" : L"Symmetry: OFF", A_HairSymmetry, s.symmetry);
            y += s.st.S(28);
            note(s.hairTool ? L"Left-drag on the head paints; Alt+drag orbits." : L"Pick a tool, then left-drag on the head or the 2D map.", kMuted);
            note(L"Tint keeps the skin's own light and dark; spots are natural", kMuted);
            note(L"and never pile up; Shine lowers the skin's roughness.", kMuted);
            note(L"The feature view's rings and mesh draw over all of it.", kMuted);
        }
        if (header(L"MAPS VIEWER")) {
            const int mw = (W - 2 * s.st.S(6)) / 3;
            for (int k = 0; k < kMapCount; ++k) {
                const int cx = m + (k % 3) * (mw + s.st.S(6)), cy = y + (k / 3) * s.st.S(28);
                Button(dc, {cx, cy, cx + mw, cy + s.st.S(24)}, Utf8ToWide(kMapNames[k]), A_MapView + k, s.mapView == k);
            }
            y += 2 * s.st.S(28);
            note(L"Pick a map: the 2D view shows it (and opens beside the 3D", kMuted);
            note(L"view if it was closed). The active tool paints on it, the", kMuted);
            note(L"feature view's rings and mesh show on every map.", kMuted);
        }
        if (header(L"EYES (COLOUR, VEINS, REDNESS)")) {
            Button(dc, {m, y, m + W, y + s.st.S(24)}, L"Zoom camera on the eyes", A_EyeZoom);
            y += s.st.S(30);
            const wchar_t* el[7] = {L"Iris colour amount", L"Iris brightness", L"Limbal ring", L"Redness (whites)", L"Corner redness", L"Veins", L"Yellowing"};
            const float* ev[7] = {&s.eye.irisAmount, &s.eye.irisBright, &s.eye.limbal, &s.eye.redness, &s.eye.corner, &s.eye.veins, &s.eye.yellow};
            Text(dc, m, y + s.st.S(4), L"Iris colour", kMuted, s.st.small);
            const RECT sw = {m + s.st.S(122), y + s.st.S(2), m + s.st.S(200), y + s.st.S(20)};
            Fill(dc, sw.left, sw.top, sw.right, sw.bottom, RGB((int)(s.eye.irisColor[0] * 255), (int)(s.eye.irisColor[1] * 255), (int)(s.eye.irisColor[2] * 255)));
            s.hits.push_back({sw, A_EyeIrisColor});
            Text(dc, sw.right + s.st.S(8), y + s.st.S(4), L"(click)", kMuted, s.st.small);
            y += s.st.S(26);
            for (int k = 0; k < 7; ++k) {
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), el[k], *ev[k], 0.0f, 1.0f, 0, 0, A_Eye + k, L"%.2f");
                y += row;
            }
            Button(dc, {m, y, m + W, y + s.st.S(22)}, L"Reset eyes", A_EyeReset);
            y += s.st.S(28);
            note(L"Eyeball placement (default: no gap at the corners):", kMuted);
            const wchar_t* pl[4] = {L"Depth back mm", L"Spread out mm", L"Height mm", L"Size"};
            for (int k = 0; k < 4; ++k) {
                SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), pl[k], s.eyePlace[k], k == 3 ? 0.9f : -3.0f, k == 3 ? 1.1f : 3.0f, 0, 0, A_EyePlace + k, k == 3 ? L"%.2f" : L"%+.1f");
                y += row;
            }
            note(L"Default: brown. Slide anything; Reset eyes returns to it.", kMuted);
            note(L"One eye picture serves both eyes, so corner redness and", kMuted);
            note(L"veins appear on both sides. Gloss and the wet shine are", kMuted);
            note(L"the eye shader's: cornea, tear film and highlights.", kMuted);
        }
        if (header(L"SKIN")) {
        if (s.skins.empty()) note(L"No presets in gameenginetools\\skin_tex\\out.", kOff);
        const int cols = 3, bw = (W - 2 * s.st.S(6)) / cols;
        for (size_t k = 0; k < s.skins.size(); ++k) {
            const int cx = m + (int)(k % cols) * (bw + s.st.S(6)), cy = y + (int)(k / cols) * s.st.S(28);
            Button(dc, {cx, cy, cx + bw, cy + s.st.S(22)}, Utf8ToWide(s.skins[k]), A_Skin + (int)k, (int)k == s.skin);
        }
        y += (int)((s.skins.size() + cols - 1) / cols) * s.st.S(28);
        note(L"Values (read only; repaint with skin_tex.py):", kMuted);
        if (s.skin < 0) note(L"  no preset applied: the scene's own skin", kMuted);
        for (const std::string& l : s.skinInfo) note(L"  " + Utf8ToWide(l), kText);
        }
        if (header(L"HAIR STYLE")) {
        if (s.groom.empty() && s.target >= 0) s.groom = CurrentGroom();
        if (s.grooms.empty()) note(L"No styles in Assets\\hair\\styles (hair_groom: groom.py --install, cap.py).", kOff);
        const int cols = 2, bw = (W - s.st.S(6)) / cols;
        for (int k = -1; k < (int)s.grooms.size(); ++k) {
            const int at = k + 1, cx = m + (at % cols) * (bw + s.st.S(6)), cy = y + (at / cols) * s.st.S(28);
            const std::string name = k < 0 ? std::string("none") : s.grooms[(size_t)k];
            Button(dc, {cx, cy, cx + bw, cy + s.st.S(22)}, k < 0 ? std::wstring(L"None (bald)") : Utf8ToWide(name), A_Groom + k, name == s.groom);
        }
        y += (int)((s.grooms.size() + 1 + cols - 1) / cols) * s.st.S(28);
        note(L"Cards, cap and fine hairline from Assets\\hair\\styles; colour", kMuted);
        note(L"from the character's hair\\hair_look.json. Saved in the preset.", kMuted);
        }
        if (header(L"HAIR MAKER (CLAY + COMB)")) {
        if (s.clay.Empty()) note(L"1. Paint the scalp (3D or 2D): its edge is the hairline.", kMuted);
        else note(Utf8ToWide(s.clay.status), kMuted);
        {
            const int bw = (W - 2 * s.st.S(6)) / 3;
            Button(dc, {m, y, m + bw, y + s.st.S(26)}, L"PAINT SCALP", A_ClayPaint, s.clayMaskMode && s.hairTool == HairPainter::Paint, true);
            Button(dc, {m + bw + s.st.S(6), y, m + 2 * bw + s.st.S(6), y + s.st.S(26)}, L"ERASE", A_ClayErase, s.clayMaskMode && s.hairTool == HairPainter::Erase, true);
            Button(dc, {m + 2 * (bw + s.st.S(6)), y, m + W, y + s.st.S(26)}, L"Default scalp", A_ClayFill);
            y += s.st.S(32);
        }
        note(L"2D: switch the view to 2D or 3D + 2D at the top and paint the atlas.", kMuted);
        note(L"or draw the hairline as one loop:", kMuted);
        Button(dc, {m, y, m + W, y + s.st.S(26)}, s.clayLoopDraw ? L"DRAWING HAIRLINE: drag a closed loop on the scalp" : L"1. DRAW HAIRLINE (closed loop)", A_ClayHairline, s.clayLoopDraw, true);
        y += s.st.S(32);
        SliderRow(dc, m, y, W, L"Clay thickness mm", s.clayHeight, 3.0f, 40.0f, 0, 0, A_ClayHeight, L"%.0f");
        y += s.st.S(24);
        Button(dc, {m, y, m + W, y + s.st.S(22)}, L"or load the clay of the current style (" + Utf8ToWide(s.groom.empty() ? std::string("side_part") : s.groom) + L")", A_ClayLoad);
        y += s.st.S(28);
        note(L"2. Sculpt the clay and comb it:", kMuted);
        {
            const wchar_t* names[5] = {L"Inflate", L"Deflate", L"Smooth", L"Grab", L"Comb"};
            const int cols = 3, bw = (W - 2 * s.st.S(6)) / cols;
            for (int k = 0; k < 5; ++k) {
                const int cx = m + (k % cols) * (bw + s.st.S(6)), cy = y + (k / cols) * s.st.S(28);
                Button(dc, {cx, cy, cx + bw, cy + s.st.S(22)}, names[k], A_ClayTool + 1 + k, s.clayTool == k + 1);
            }
            const int cx = m + 2 * (bw + s.st.S(6)), cy = y + s.st.S(28);
            Button(dc, {cx, cy, cx + bw, cy + s.st.S(22)}, L"Undo", A_ClayUndo);
            y += 2 * s.st.S(28);
        }
        SliderRow(dc, m, y, W, L"Brush radius mm", s.hairRadius, 2.0f, 40.0f, 0, 0, A_HairRadius, L"%.0f");
        y += s.st.S(22);
        SliderRow(dc, m, y, W, L"Strength", s.hairStrength, 0.0f, 1.0f, 0, 0, A_HairStrength, L"%.2f");
        y += s.st.S(22);
        SliderRow(dc, m, y, W, L"Hardness", s.hairHardness, 0.0f, 1.0f, 0, 0, A_HairHardness, L"%.2f");
        y += s.st.S(24);
        {
            const int bw = (W - s.st.S(6)) / 2; // symmetry lives in the frozen bar at the top
            Button(dc, {m, y, m + bw, y + s.st.S(22)}, s.clayShowHair ? L"Hair shown" : L"Hair hidden", A_ClayHair, s.clayShowHair);
            Button(dc, {m + bw + s.st.S(6), y, m + W, y + s.st.S(22)}, s.clayCombView ? L"Comb lines ON" : L"Comb lines OFF", A_ClayCombView, s.clayCombView);
            y += s.st.S(28);
        }
        if (s.clayTool == HairClay::Inflate || s.clayTool == HairClay::Deflate) { // the heat map's key
            const int bx = m, bw = W - s.st.S(70), bh2 = s.st.S(10);
            for (int k = 0; k < bw; ++k) {
                const float t = k / float(std::max(1, bw - 1)) * 4.0f;
                static const float stops[5][3] = {{0.05f, 0.1f, 0.8f}, {0.0f, 0.7f, 0.9f}, {0.1f, 0.8f, 0.15f}, {0.95f, 0.85f, 0.05f}, {0.9f, 0.08f, 0.04f}};
                const int i = std::min(3, (int)t);
                const float f = t - i;
                const auto c = [&](int ch) { return (BYTE)std::clamp((stops[i][ch] + (stops[i + 1][ch] - stops[i][ch]) * f) * 255.0f, 0.0f, 255.0f); };
                Fill(dc, bx + k, y + s.st.S(3), bx + k + 1, y + s.st.S(3) + bh2, RGB(c(0), c(1), c(2)));
            }
            Text(dc, bx + bw + s.st.S(6), y, L"0 - 30 mm", kMuted, s.st.small);
            y += s.st.S(18);
        }
        Button(dc, {m, y, m + W, y + s.st.S(26)}, s.genProc ? L"Growing hair from the clay..." : L"GENERATE HAIR FROM CLAY + COMB", A_ClayGenerate, s.genProc != nullptr, true);
        y += s.st.S(32);
        note(L"Clay = the hair's volume: hair ends where the clay ends.", kMuted);
        note(L"Comb = flow. Generate grows cards inside the clay along the", kMuted);
        note(L"comb, decimates + shrinks this clay into the cap, fits the", kMuted);
        note(L"cards to it, adds the hairline, and puts it on as 'sculpt'.", kMuted);
        }
        if (header(L"NOT WIRED YET", false)) {
        if (fold(L"Mouth / teeth", false, false)) note(L"NOT WIRED YET: mouth_gen's mouth_interior.glb is not loaded.", kOff);
        if (fold(L"Lips", false, false)) note(L"NOT WIRED YET: needs the lip maps in the renderer.", kOff);
        if (fold(L"Wrinkles", false, false)) note(L"NOT WIRED YET: needs zoned wrinkle maps in the renderer.", kOff);
        }
    } else {
        if (header(L"PARTS", false))
        for (const char* stem : {"body_head", "body_torso", "body_legs"}) {
            const int i = FindEntity(stem);
            note(Utf8ToWide(std::string(stem)) + L": " + (i >= 0 ? Utf8ToWide(s.host.entityName(i)) : std::wstring(L"not in the scene")), i >= 0 ? kText : kOff);
        }
        if (!header(L"BODY SHAPE")) {}
        else if (!s.bodyLoaded) {
            note(Utf8ToWide(s.bodyError.substr(0, 60)), kOff);
            for (size_t at = 60; at < s.bodyError.size(); at += 60) note(Utf8ToWide(s.bodyError.substr(at, 60)), kOff);
            note(L"The sliders appear here as soon as body_maker writes it.", kMuted);
        } else {
            seedRow(A_BodyRandom, A_BodySeedDown, A_BodySeedUp, s.bodySeed);
            SliderRow(dc, m, y, W, L"Height scale", s.heightScale, s.heightMin, s.heightMax, 0, 0, A_Height, L"%.3f");
            y += row;
            note(L"Height = uniform scale of the whole character.", kMuted);
            note(L"NOT AVAILABLE: limb lengths, proportional height", kOff);
            note(L"(need skeleton retargeting).", kOff);
            for (const std::string& g : s.parts[0].set.groups) {
                if (!fold(Utf8ToWide(g), false, false)) continue;
                for (size_t i = 0; i < s.parts[0].set.sliders.size(); ++i)
                    if (s.parts[0].set.sliders[i].group == g) {
                        const MorphSlider& sl = s.parts[0].set.sliders[i];
                        SliderRow(dc, m + s.st.S(8), y, W - s.st.S(8), Utf8ToWide(sl.label.empty() ? sl.name : sl.label), s.bodyValues[i], -1.0f, 1.0f, sl.lo, sl.hi, A_Body + (int)i);
                        y += row;
                    }
            }
        }
    }
    return y + s.st.S(12);
}

void Paint(HWND hwnd) {
    State& s = S();
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, std::max(1L, cr.right), std::max(1L, cr.bottom));
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, 0, 0, cr.right, cr.bottom, kBg);
    s.hits.clear();
    s.foldKeys.clear();
    const int m = s.st.S(10), W = cr.right - 2 * m - s.st.S(6);
    const int top = TopH();
    HRGN clip = CreateRectRgn(0, top, cr.right, cr.bottom);
    SelectClipRgn(dc, clip);
    int& scroll = s.scroll[s.tab];
    const int end = DrawBody(dc, m, W, top - scroll);
    const int contentH = end + scroll - top, viewH = cr.bottom - top;
    const int maxScroll = std::max(0, contentH - viewH);
    if (scroll > maxScroll) scroll = maxScroll;
    if (maxScroll > 0) {
        const int barH = std::max(s.st.S(20), viewH * viewH / std::max(1, contentH)), barY = top + (viewH - barH) * scroll / maxScroll;
        Fill(dc, cr.right - s.st.S(5), barY, cr.right - s.st.S(1), barY + barH, kMuted);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    s.hits.erase(std::remove_if(s.hits.begin(), s.hits.end(), [&](const Hit& h) { return h.r.bottom <= top; }), s.hits.end());
    const int third = (cr.right - 5 * m) / 4, bh = s.st.S(40);
    Button(dc, {m, m, m + third, m + bh}, L"BODY", A_TabBody, s.tab == 0, true);
    Button(dc, {2 * m + third, m, 2 * m + 2 * third, m + bh}, L"HEAD", A_TabHead, s.tab == 1, true);
    Button(dc, {3 * m + 2 * third, m, 3 * m + 3 * third, m + bh}, L"CLOTHES", A_TabClothes, s.tab == 2, true);
    Button(dc, {4 * m + 3 * third, m, 4 * m + 4 * third, m + bh}, L"PHOTO", A_TabPhoto, s.tab == 3, true);
    const int y2 = m + bh + s.st.S(8), q = (cr.right - 2 * m - 3 * s.st.S(6)) / 4;
    const wchar_t* labels[4] = {L"New", L"Save", L"Load", L"Reset"};
    const int acts[4] = {A_New, A_Save, A_Load, A_Reset};
    for (int k = 0; k < 4; ++k) Button(dc, {m + k * (q + s.st.S(6)), y2, m + k * (q + s.st.S(6)) + q, y2 + s.st.S(24)}, labels[k], acts[k]);
    // HEAD has a fifth button here: Symmetry, frozen at the top so it is in reach while painting or sculpting anywhere below
    const int cols = s.tab == 1 ? 5 : 4, yv = y2 + s.st.S(30), t4 = (cr.right - 2 * m - (cols - 1) * s.st.S(6)) / cols;
    const wchar_t* vl[3] = {L"3D", L"2D", L"3D + 2D"};
    if (s.tab != 3) for (int k = 0; k < 3; ++k) Button(dc, {m + k * (t4 + s.st.S(6)), yv, m + k * (t4 + s.st.S(6)) + t4, yv + s.st.S(24)}, vl[k], A_View3D + k, s.view == k);
    // the feature overlay: rig regions, eyebrow zone and mesh wires on the head (3D) and the atlas (2D)
    if (s.tab != 3) Button(dc, {m + 3 * (t4 + s.st.S(6)), yv, m + 3 * (t4 + s.st.S(6)) + t4, yv + s.st.S(24)}, s.features ? L"Features ON" : L"Features", A_Features, s.features);
    if (s.tab == 1) Button(dc, {m + 4 * (t4 + s.st.S(6)), yv, cr.right - m, yv + s.st.S(24)}, s.symmetry ? L"Symmetry ON" : L"Symmetry", A_HairSymmetry, s.symmetry);
    const std::wstring st = Utf8ToWide(s.status);
    const int y3 = yv + s.st.S(30);
    Text(dc, m, y3, st.substr(0, 58), kMuted, s.st.small);
    if (st.size() > 58) Text(dc, m, y3 + s.st.S(16), st.substr(58, 58), kMuted, s.st.small);
    if (st.size() > 116) Text(dc, m, y3 + s.st.S(32), st.substr(116, 58), kMuted, s.st.small);
    Fill(dc, 0, top - 1, cr.right, top, kAccent);
    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void DragTo(int action, int mx) {
    State& s = S();
    RECT cr;
    GetClientRect(s.wnd, &cr);
    const int m = s.st.S(10), W = cr.right - 2 * m - s.st.S(6);
    const bool indented = action >= A_Face && action != A_ClayHeight;
    const RECT tr = Track(indented ? m + s.st.S(8) : m, indented ? W - s.st.S(8) : W);
    const float t = std::clamp((float)(mx - tr.left) / (float)std::max(1L, tr.right - tr.left), 0.0f, 1.0f);
    const float v = std::round((t * 2.0f - 1.0f) * 100.0f) / 100.0f;
    if (IsClothSlider(action)) {
        const ClothSlider& c = kClothSliders[action - A_Cloth];
        s.shirt.*c.field = c.lo + t * (c.hi - c.lo);
        Redraw();
        return;
    }
    if (IsBrowSlider(action)) {
        const BrowSlider& b = kBrowSliders[action - A_BrowDensity];
        s.brow.*b.field = b.lo + t * (b.hi - b.lo);
        if (b.shape && !s.brow.custom) s.brow.DefaultLoop();
        if (NowMs() - s.browLastPreview > 120) { BakeBrows(true); s.browLastPreview = NowMs(); } // live preview while dragging
        Redraw();
        return;
    }
    if (action == A_PhotoSize || action == A_PhotoOffX || action == A_PhotoOffY) {
        if (photo::Photo* ph = ActivePhoto()) {
            if (action == A_PhotoSize) ph->size = std::round((0.25f + t * 3.75f) * 20.0f) / 20.0f;
            else if (action == A_PhotoOffX) ph->offx = std::round((t * 2.0f - 1.0f) * 50.0f) / 100.0f;
            else ph->offy = std::round((t * 2.0f - 1.0f) * 50.0f) / 100.0f;
            SolveActive();
            SavePhotoSession();
        }
        PhotoRedraw();
        return;
    }
    if (action == A_PhotoWire) { s.wireAlpha = t; PhotoRedraw(); return; }
    if (action == A_PhotoSkinWarm) { s.skinWarm = std::round((t * 2.0f - 1.0f) * 20.0f) / 20.0f; Redraw(); return; }
    if (action == A_PhotoSkinExp) { s.skinExp = std::round((t * 4.0f - 2.0f) * 10.0f) / 10.0f; Redraw(); return; }
    if (action == A_PhotoSkinStr) { s.skinStrength = std::round(t * 100.0f) / 100.0f; Redraw(); return; }
    if (action == A_PhotoYaw) { s.viewYaw = std::round((t * 2.0f - 1.0f) * 90.0f) / 57.2958f; PhotoRedraw(); return; }
    if (action == A_PhotoFov) { if (photo::Photo* ph = ActivePhoto()) { ph->cam.fov = std::round(10.0f + t * 50.0f); SolveActive(); } PhotoRedraw(); return; }
    if (action == A_Age) SetAge(t);
    else if (action == A_Height) { s.heightScale = s.heightMin + t * (s.heightMax - s.heightMin); s.bodyDirty = true; }
    else if (action == A_HairRadius) s.hairRadius = 2.0f + t * 38.0f;
    else if (action == A_ClayHeight) {
        s.clayHeight = std::round(3.0f + t * 37.0f);
        if (s.clayMaskMode) ClayFromMask();
        else if (s.clayLoop.size() >= 8 && !s.clayLoopStroke) ClayFromLoop();
    }
    else if (action == A_HairStrength) s.hairStrength = t;
    else if (action == A_HairHardness) s.hairHardness = t;
    else if (action >= A_EyePlace && action < A_EyePlace + 4) {
        const int k = action - A_EyePlace;
        s.eyePlace[k] = k == 3 ? std::round((0.9f + 0.2f * t) * 100.0f) / 100.0f : std::round((t * 2.0f - 1.0f) * 3.0f * 10.0f) / 10.0f;
        s.faceDirty = true;
    }
    else if (action >= A_Eye && action < A_Eye + 7) {
        float* f[7] = {&s.eye.irisAmount, &s.eye.irisBright, &s.eye.limbal, &s.eye.redness, &s.eye.corner, &s.eye.veins, &s.eye.yellow};
        *f[action - A_Eye] = std::round(t * 100.0f) / 100.0f;
        s.eyeDirty = true;
    }
    if (s.wnd2d && action >= A_HairRadius && action <= A_HairHardness) InvalidateRect(s.wnd2d, nullptr, FALSE);
    else if (action >= A_Body && action - A_Body < (int)s.bodyValues.size()) { s.bodyValues[(size_t)(action - A_Body)] = v; s.bodyPresetSeed = -1; s.bodyDirty = true; }
    else if (action >= A_Face && action - A_Face < (int)s.values.size()) { s.values[(size_t)(action - A_Face)] = v; s.presetSeed = -1; s.faceDirty = true; }
    Redraw();
}

int HitAt(int x, int y) {
    for (const Hit& h : S().hits)
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h.action;
    return 0;
}
bool IsSlider(int a) { if (a == A_ClayHeight || a == A_PhotoFov || a == A_PhotoWire || a == A_PhotoYaw || a == A_PhotoSkinStr || a == A_PhotoSkinWarm || a == A_PhotoSkinExp || a == A_PhotoSize || a == A_PhotoOffX || a == A_PhotoOffY) return true;
    if (a >= A_Groom - 1 && a <= A_ClayFill) return false; // hair style and Hair Maker buttons
    return IsBrowSlider(a) || (a >= A_Eye && a < A_Eye + 7) || (a >= A_EyePlace && a < A_EyePlace + 4) || a == A_Age || a == A_Height || a == A_HairRadius || a == A_HairStrength || a == A_HairHardness || (a >= A_Face && a < A_Fold); }

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State& s = S();
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: Redraw(); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        const int a = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (IsSlider(a)) { Begin(); s.drag = a; SetCapture(hwnd); DragTo(a, GET_X_LPARAM(lp)); }
        else if (a) { Begin(); Do(a); Commit(ActionLabel(a)); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const int a = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (IsClothSlider(a)) { const clothes::ShirtRecipe fresh; s.shirt.*kClothSliders[a - A_Cloth].field = fresh.*kClothSliders[a - A_Cloth].field; if (s.clothAuto) StartClothBuild(); }
        else if (IsBrowSlider(a)) { const BrowSlider& b = kBrowSliders[a - A_BrowDensity]; BrowMaker fresh; s.brow.*b.field = fresh.*b.field; if (b.shape && !s.brow.custom) s.brow.DefaultLoop(); BakeBrows(); }
        else if (a >= A_Body && a - A_Body < (int)s.bodyValues.size()) { s.bodyValues[(size_t)(a - A_Body)] = 0; s.bodyDirty = true; }
        else if (a >= A_Face && a - A_Face < (int)s.values.size()) { s.values[(size_t)(a - A_Face)] = 0; s.faceDirty = true; }
        else if (a == A_Height) { s.heightScale = 1.0f; s.bodyDirty = true; }
        else if (a == A_PhotoYaw) { s.viewYaw = 0; PhotoRedraw(); }
        Redraw();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (s.drag) DragTo(s.drag, GET_X_LPARAM(lp));
        if (s.tab == 3) { // the pin row under the mouse gets a preview on the head
            const int a = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            const int hv = a >= A_PhotoPin && a < A_PhotoPin + photo::kMaxPins ? a - A_PhotoPin : -1;
            if (!s.hoverTrack) { TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&te); s.hoverTrack = true; }
            if (hv != s.hoverPin) { s.hoverPin = hv; if (s.wnd2d) InvalidateRect(s.wnd2d, nullptr, FALSE); }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s.hoverTrack = false;
        if (s.hoverPin >= 0) { s.hoverPin = -1; if (s.wnd2d) InvalidateRect(s.wnd2d, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: if (s.drag) { const int a = s.drag; s.drag = 0; ReleaseCapture(); if (IsBrowSlider(a)) BakeBrows(); if (IsClothSlider(a) && s.clothAuto) StartClothBuild(); Commit(ActionLabel(a)); } return 0;
    case WM_CAPTURECHANGED: if (s.drag) Commit(ActionLabel(s.drag)); s.drag = 0; return 0;
    case WM_MOUSEWHEEL: s.scroll[s.tab] = std::max(0, s.scroll[s.tab] - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * s.st.S(60)); Redraw(); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 2D view: the head's albedo atlas (baked hair on it), paintable
// Wheel zooms about the cursor (1x..32x), middle or right drag pans, a brush circle shows radius, strength and hardness.
RECT Atlas2D(const RECT& cr) { // where the whole atlas lands: zoomed, the view centre on (cu, cv)
    State& s = S();
    const float side = (float)std::max(1, (int)std::min(cr.right, cr.bottom) - 2 * s.st.S(8)) * s.zoom2d;
    const float x = cr.right * 0.5f - s.cu2d * side, y = cr.bottom * 0.5f - s.cv2d * side;
    return {(LONG)std::lround(x), (LONG)std::lround(y), (LONG)std::lround(x + side), (LONG)std::lround(y + side)};
}

// The 2D brush sizes like the 3D one: the last 3D hit's UV-per-metre, else the mean over the head's triangles.
float UvPerMetre2D() {
    State& s = S();
    if (s.lastHit.hit) return s.lastHit.uvPerMetre;
    if (s.uvPerMetre2D > 0) return s.uvPerMetre2D;
    double au = 0, a3 = 0;
    for (size_t k = 0; k + 2 < s.headNow.size(); k += 3) {
        const Vertex &A = s.headNow[k], &B = s.headNow[k + 1], &C = s.headNow[k + 2];
        const float e1[3] = {B.px - A.px, B.py - A.py, B.pz - A.pz}, e2[3] = {C.px - A.px, C.py - A.py, C.pz - A.pz};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        a3 += std::sqrt(cx * cx + cy * cy + cz * cz);
        au += std::fabs((B.u - A.u) * (C.v - A.v) - (C.u - A.u) * (B.v - A.v));
    }
    s.uvPerMetre2D = a3 > 1e-12 ? (float)std::sqrt(au / a3) : 4.0f;
    return s.uvPerMetre2D;
}

void Paint2D(HWND hwnd) {
    State& s = S();
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    const int W = std::max(1L, cr.right), H = std::max(1L, cr.bottom);
    HDC dc = CreateCompatibleDC(wdc);
    BITMAPINFO bb = {};
    bb.bmiHeader.biSize = sizeof(bb.bmiHeader);
    bb.bmiHeader.biWidth = W;
    bb.bmiHeader.biHeight = -H;
    bb.bmiHeader.biPlanes = 1;
    bb.bmiHeader.biBitCount = 32;
    uint8_t* px = nullptr; // the back buffer's pixels (BGRA, top-down), for the brush overlay
    HBITMAP bmp = CreateDIBSection(wdc, &bb, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, 0, 0, cr.right, cr.bottom, RGB(24, 22, 20));
    if (s.tab == 3) {
        GdiFlush();
        PaintPhoto(hwnd, dc, px, W, H);
        PaintPinPreview(dc, px, W, H);
        BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return;
    }
    const RECT a = Atlas2D(cr);
    const std::vector<uint8_t>& img = s.mapView > 0 && !s.mapImg.empty() ? s.mapImg : (s.shown.empty() ? s.albedo : s.shown);
    const bool have = !img.empty() && s.albedoW > 0 && img.size() >= (size_t)s.albedoW * s.albedoH * 4;
    if (have) {
        if (s.bgraDirty || s.bgra.size() != img.size()) { // RGBA -> BGRA once per bake, not per repaint
            s.bgra.resize(img.size());
            for (size_t i = 0; i + 3 < img.size(); i += 4) { s.bgra[i] = img[i + 2]; s.bgra[i + 1] = img[i + 1]; s.bgra[i + 2] = img[i]; s.bgra[i + 3] = 255; }
            s.bgraDirty = false;
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = s.albedoW;
        bi.bmiHeader.biHeight = -s.albedoH; // top-down: row 0 is v = 0
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        // halftone only when the atlas is shrunk a lot; zoomed in, plain sampling (crisp texels and fast)
        SetStretchBltMode(dc, (a.right - a.left) * 2 < s.albedoW ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, a.left, a.top, a.right - a.left, a.bottom - a.top, 0, 0, s.albedoW, s.albedoH, s.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        Text(dc, cr.right / 2, cr.bottom / 2, L"No head albedo yet: open the HEAD tab on a scene with the head.", kMuted, s.st.small, TA_CENTER);
    }
    GdiFlush();
    // brush: the dab's own weight curve as a tinted gradient (strength = how opaque, hardness = the solid core), an outer ring
    // at the radius and a thin inner ring where the falloff starts
    if (have && px && s.hover2d && s.hairTool != HairPainter::None) {
        const float r = std::max(1.5f, s.hairRadius * 0.001f * UvPerMetre2D() * (float)(a.right - a.left));
        const float hard = std::clamp(s.hairHardness, 0.0f, 0.99f);
        const float mx = (float)s.mouse2d.x, my = (float)s.mouse2d.y;
        float tint[3] = {255, 255, 255};                                   // Paint: the hair colour
        if (s.hairTool == HairPainter::Paint) for (int c = 0; c < 3; ++c) tint[c] = std::clamp(s.hair.color[c] * 255.0f * 1.6f + 40.0f, 0.0f, 255.0f);
        else if (s.hairTool == HairPainter::Erase) { tint[0] = 255; tint[1] = 80; tint[2] = 70; }
        else if (s.hairTool == HairPainter::Comb) { tint[0] = 90; tint[1] = 200; tint[2] = 255; }
        else if (s.hairTool == SkinPaint::TintPaint) { for (int c = 0; c < 3; ++c) tint[c] = SkinPaint::Tint(s.tintType).rgb[c]; }
        else if (s.hairTool == SkinPaint::SpotPaint) { tint[0] = 255; tint[1] = 200; tint[2] = 90; }
        else if (s.hairTool == SkinPaint::ShinePaint) { tint[0] = 255; tint[1] = 255; tint[2] = 255; }
        else if (s.hairTool == SkinPaint::TintErase || s.hairTool == SkinPaint::SpotErase || s.hairTool == SkinPaint::ShineErase) { tint[0] = 255; tint[1] = 80; tint[2] = 70; }
        else { tint[0] = 200; tint[1] = 255; tint[2] = 140; }                // Smooth
        const int x0 = std::max(0, (int)(mx - r - 2)), x1 = std::min(W - 1, (int)(mx + r + 2));
        const int y0 = std::max(0, (int)(my - r - 2)), y1 = std::min(H - 1, (int)(my + r + 2));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float d = std::sqrt((x - mx) * (x - mx) + (y - my) * (y - my));
                const float q = d / r;
                float alpha = q < 1 ? 0.65f * s.hairStrength * HairPainter::Falloff(q, hard) : 0.0f;
                if (std::fabs(d - r) < 1.0f) alpha = 1.0f;                         // radius ring
                else if (hard > 0.01f && std::fabs(d - r * hard) < 0.6f) alpha = std::max(alpha, 0.55f); // hardness ring
                if (alpha <= 0) continue;
                uint8_t* p = px + ((size_t)y * W + x) * 4; // BGRA
                p[0] = (uint8_t)(p[0] + (tint[2] - p[0]) * alpha);
                p[1] = (uint8_t)(p[1] + (tint[1] - p[1]) * alpha);
                p[2] = (uint8_t)(p[2] + (tint[0] - p[2]) * alpha);
            }
    }
    wchar_t info[160];
    wchar_t mapName[40] = {};
    swprintf(mapName, 40, L"[%hs]  ", kMapNames[std::clamp(s.mapView, 0, kMapCount - 1)]);
    swprintf(info, 160, L"2D map %ls%.1fx   wheel: zoom   middle / right drag: pan   %ls", mapName, s.zoom2d,
             s.hairTool == HairPainter::None ? L"(pick a tool to paint here)" : L"left drag: paint");
    Fill(dc, 0, 0, cr.right, s.st.S(24), RGB(24, 22, 20));
    Text(dc, s.st.S(10), s.st.S(5), info, kMuted, s.st.small);
    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void Dab2D(HWND hwnd, int x, int y, bool first) {
    State& s = S();
    RECT cr;
    GetClientRect(hwnd, &cr);
    const RECT a = Atlas2D(cr);
    const float u = (float)(x - a.left) / (float)std::max(1L, a.right - a.left), v = (float)(y - a.top) / (float)std::max(1L, a.bottom - a.top);
    if (u < 0 || u > 1 || v < 0 || v > 1) { s.last2dU = -1; return; }
    HairHit h;
    h.hit = true; h.u = u; h.v = v; h.uvPerMetre = UvPerMetre2D();
    float du = 0, dv = 0;
    if (!first && s.last2dU >= 0) { du = u - s.last2dU; dv = v - s.last2dV; }
    DoDab(h, du, dv);
    s.last2dU = u; s.last2dV = v;
    if (s.symmetry && s.hair.SurfaceAt(u, v, h.p, h.n)) MirrorDab(h, first);
    s.hairDirty = true;
}

void Zoom2D(HWND hwnd, int x, int y, float factor) {
    State& s = S();
    RECT cr;
    GetClientRect(hwnd, &cr);
    const RECT a = Atlas2D(cr);
    const float side = (float)std::max(1L, a.right - a.left);
    const float u = (x - a.left) / side, v = (y - a.top) / side; // the atlas point under the cursor stays under it
    const float z = std::clamp(s.zoom2d * factor, 1.0f, 32.0f);
    if (z == s.zoom2d) return;
    const float nside = side * z / s.zoom2d;
    s.zoom2d = z;
    s.cu2d = u - (x - cr.right * 0.5f) / nside;
    s.cv2d = v - (y - cr.bottom * 0.5f) / nside;
    if (z <= 1.0f) { s.cu2d = s.cv2d = 0.5f; }
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc2D(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State& s = S();
    switch (msg) {
    case WM_PAINT: Paint2D(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    default: break;
    }
    if (s.tab == 3) { LRESULT r = 0; if (PhotoMouse(hwnd, msg, wp, lp, &r)) return r; }
    switch (msg) {
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        Zoom2D(hwnd, p.x, p.y, std::pow(1.25f, (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA));
        return 0;
    }
    case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        s.pan2d = true;
        s.panFrom = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd);
        return 0;
    case WM_MBUTTONUP: case WM_RBUTTONUP:
        if (s.pan2d) { s.pan2d = false; ReleaseCapture(); }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (s.tab != 1 || s.hairTool == HairPainter::None || !InitHair()) return 0;
        Begin();
        s.stroke2d = s.stroking = true;
        s.lastMirror.hit = false;
        SetCapture(hwnd);
        Dab2D(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
        s.stroke2dAt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (s.stroke2d) { // fill the gap from the last position (every 2 pixels), then the dab below lands on p itself
            const int dx = p.x - s.stroke2dAt.x, dy = p.y - s.stroke2dAt.y;
            const int steps = std::clamp((int)(std::sqrt((float)(dx * dx + dy * dy)) / 2.0f), 1, 96);
            for (int k = 1; k < steps; ++k) Dab2D(hwnd, s.stroke2dAt.x + dx * k / steps, s.stroke2dAt.y + dy * k / steps, false);
        }
        if (!s.hover2d) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t); }
        s.hover2d = true;
        s.mouse2d = p;
        if (s.pan2d) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            const RECT a = Atlas2D(cr);
            const float side = (float)std::max(1L, a.right - a.left);
            s.cu2d = std::clamp(s.cu2d - (p.x - s.panFrom.x) / side, 0.0f, 1.0f);
            s.cv2d = std::clamp(s.cv2d - (p.y - s.panFrom.y) / side, 0.0f, 1.0f);
            s.panFrom = p;
        }
        if (s.stroke2d) { Dab2D(hwnd, p.x, p.y, false); s.stroke2dAt = p; }
        InvalidateRect(hwnd, nullptr, FALSE); // the brush circle follows the mouse
        return 0;
    }
    case WM_MOUSELEAVE: s.hover2d = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
        if (s.stroke2d) { s.stroke2d = s.stroking = false; ReleaseCapture(); s.bakeNow = true; Commit(ToolName(s.hairTool)); }
        return 0;
    case WM_CAPTURECHANGED:
        if (s.stroke2d) { s.stroke2d = s.stroking = false; s.bakeNow = true; Commit(ToolName(s.hairTool)); }
        s.pan2d = false;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Open(bool on) {
    State& s = S();
    if (!s.hostSet) return;
    if (on && !s.wnd) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = s.host.instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"TBDCharacterCreator";
        RegisterClassW(&wc);
        s.st.dpi = (int)GetDpiForWindow(s.host.parent);
        if (s.st.dpi <= 0) s.st.dpi = 96;
        CreateFonts(s.st);
        s.wnd = CreateWindowExW(0, wc.lpszClassName, L"Character Creator", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, s.host.parent, nullptr, s.host.instance, nullptr);
        WNDCLASSW w2 = wc;
        w2.lpfnWndProc = WndProc2D;
        w2.hCursor = LoadCursor(nullptr, IDC_CROSS);
        w2.lpszClassName = L"TBDCharacterCreator2D";
        RegisterClassW(&w2);
        s.wnd2d = CreateWindowExW(0, w2.lpszClassName, L"Character 2D view", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, s.host.parent, nullptr, s.host.instance, nullptr);
    }
    if (on == s.open) return;
    s.open = on;
    if (on) { LoadFace(); PollBody(); ScanLooks(); BindHead(); } // no head selected: the default human (added if missing)
    else s.hairTool = HairPainter::None;
    if (s.wnd) ShowWindow(s.wnd, on ? SW_SHOWNA : SW_HIDE);
    if (s.wnd2d) ShowWindow(s.wnd2d, on && s.view ? SW_SHOWNA : SW_HIDE);
    if (s.host.relayout) s.host.relayout();
    if (on) SelectTab(s.tab);
}

} // namespace

void SetHost(const Host& h) { S().host = h; S().hostSet = true; }
bool Busy() { return S().clothBusy; }
bool HostSet() { return S().hostSet; }
float CameraScale() { return S().open ? 0.3f : 1.0f; }

void AddMenu(HMENU bar) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kMenuCommand, L"Open / Close Creator\tF7");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuBody, L"Body");
    AppendMenuW(m, MF_STRING, kMenuHead, L"Head");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"Character Creator");
}

void Toggle() { Open(!S().open); }

void Command(UINT id) {
    if (id == kMenuCommand) { Toggle(); return; }
    Open(true);
    SelectTab(id == kMenuBody ? 0 : 1);
}

void Dock(RECT& viewport, int dpi) {
    State& s = S();
    if (!s.open) return;
    const int w = std::min(MulDiv(390, dpi, 96), (int)(viewport.right - viewport.left) / 2);
    s.dock = {viewport.left, viewport.top, viewport.left + w, viewport.bottom};
    viewport.left += w;
    // 2D: the texture view takes the rest (the 3D viewport keeps 1 px so the renderer never sees a zero size); 3D + 2D: halves
    const bool floating = s.host.viewportFloating && s.host.viewportFloating();
    if (s.view == 1 || (s.view == 2 && floating)) { s.dock2d = {viewport.left, viewport.top, viewport.right - 1, viewport.bottom}; viewport.left = viewport.right - 1; }
    else if (s.view == 2) { const int half = (viewport.right - viewport.left) / 2; s.dock2d = {viewport.left, viewport.top, viewport.left + half, viewport.bottom}; viewport.left += half; }
}

void PlaceDock() {
    State& s = S();
    if (s.wnd && s.open) MoveWindow(s.wnd, s.dock.left, s.dock.top, s.dock.right - s.dock.left, s.dock.bottom - s.dock.top, TRUE);
    if (s.wnd2d) {
        const bool show = s.open && s.view != 0;
        if (show) MoveWindow(s.wnd2d, s.dock2d.left, s.dock2d.top, s.dock2d.right - s.dock2d.left, s.dock2d.bottom - s.dock2d.top, TRUE);
        ShowWindow(s.wnd2d, show ? SW_SHOWNA : SW_HIDE);
    }
}

bool ViewportMouse(HWND viewport, UINT msg, int x, int y) {
    State& s = S();
    if (s.open && s.tab == 1 && s.clayLoopDraw) { // the hairline: a drag on the scalp draws the loop; letting go closes it
        if (msg == WM_LBUTTONUP) {
            if (!s.clayLoopStroke) return false;
            s.clayLoopStroke = false;
            ReleaseCapture();
            ClayFromLoop();
            Redraw();
            return true;
        }
        if (msg != WM_LBUTTONDOWN && !(msg == WM_MOUSEMOVE && s.clayLoopStroke)) return false;
        if (!InitHair()) return true;
        float o[3], d[3];
        if (!s.host.entityRay(s.target, x, y, o, d)) return true;
        const HairHit h = s.hair.Raycast(o, d, s.headNow);
        if (msg == WM_LBUTTONDOWN) {
            if (!h.hit) return false; // off the head: the camera still orbits
            s.clayLoopStroke = true;
            s.clayLoop.clear();
            SetCapture(viewport);
        }
        if (h.hit) {
            const HairClay::V3 p = {h.p[0], h.p[1], h.p[2]};
            if (s.clayLoop.empty() || std::hypot(std::hypot(p[0] - s.clayLoop.back()[0], p[1] - s.clayLoop.back()[1]), p[2] - s.clayLoop.back()[2]) > 0.002f) s.clayLoop.push_back(p);
        }
        return true;
    }
    if (s.open && s.tab == 1 && s.clayTool != HairClay::None && !s.clay.Empty() && s.target >= 0) {
        float o[3], d[3];
        const bool ray = s.host.entityRay(s.target, x, y, o, d);
        if (msg == WM_MOUSEMOVE && !s.clayStroke) {
            s.clayHover = ray && s.clay.Raycast(o, d, s.clayHoverP, s.clayHoverN);
            return false;
        }
        if (msg == WM_LBUTTONUP) {
            if (!s.clayStroke) return false;
            s.clayStroke = false;
            ReleaseCapture();
            s.status = std::string("Clay: ") + ClayToolName(s.clayTool) + " done (Undo to step back).";
            Redraw();
            return true;
        }
        if (msg != WM_LBUTTONDOWN && !(msg == WM_MOUSEMOVE && s.clayStroke)) return false;
        if (!ray) return true;
        const float radius = s.hairRadius * 0.001f;
        if (msg == WM_LBUTTONDOWN) {
            HairClay::V3 p, n;
            if (!s.clay.Raycast(o, d, p, n)) return false; // a click off the clay still orbits the camera
            s.clay.BeginStroke();
            s.clayStroke = true;
            s.clayAt = p;
            s.clayPlaneN = {d[0], d[1], d[2]};
            s.clayMouse = {x, y};
            SetCapture(viewport);
            if (s.clayTool != HairClay::Grab && s.clayTool != HairClay::Comb) s.clay.Dab((HairClay::Tool)s.clayTool, p, radius, s.hairStrength, s.hairHardness, {0, 0, 0}, s.symmetry, s.headNow);
            return true;
        }
        if (s.clayTool == HairClay::Grab) { // the grabbed clay follows the mouse on the plane through the first hit, facing the camera
            const HairClay::V3& n = s.clayPlaneN;
            const float den = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
            if (std::fabs(den) < 1e-6f) return true;
            const float t = ((s.clayAt[0] - o[0]) * n[0] + (s.clayAt[1] - o[1]) * n[1] + (s.clayAt[2] - o[2]) * n[2]) / den;
            const HairClay::V3 q = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
            s.clay.GrabTo(s.clayAt, {q[0] - s.clayAt[0], q[1] - s.clayAt[1], q[2] - s.clayAt[2]}, radius, s.hairHardness, s.symmetry);
            return true;
        }
        // dabs every 3 pixels along the drag
        const int dx = x - s.clayMouse.x, dy = y - s.clayMouse.y;
        const int steps = std::clamp((int)(std::sqrt((float)(dx * dx + dy * dy)) / 3.0f), 1, 48);
        for (int k = 1; k <= steps; ++k) {
            float o2[3], d2[3];
            HairClay::V3 p, n;
            if (!s.host.entityRay(s.target, s.clayMouse.x + dx * k / steps, s.clayMouse.y + dy * k / steps, o2, d2) || !s.clay.Raycast(o2, d2, p, n)) continue;
            const HairClay::V3 move = {p[0] - s.clayAt[0], p[1] - s.clayAt[1], p[2] - s.clayAt[2]};
            s.clay.Dab((HairClay::Tool)s.clayTool, p, radius, s.hairStrength * (s.clayTool == HairClay::Comb ? 1.0f : 0.35f), s.hairHardness, move, s.symmetry, s.headNow);
            s.clayAt = p;
            s.clayHoverP = p;
            s.clayHoverN = n;
        }
        s.clayMouse = {x, y};
        s.clayHover = true;
        return true;
    }
    if (s.open && s.tab == 1 && s.browDraw) { // brow boundary: a drag on the head draws the loop (front view x, y; mirrored to x > 0)
        if (msg == WM_LBUTTONUP) {
            if (!s.browStroke) return false;
            s.browStroke = false;
            ReleaseCapture();
            if (s.browStrokePts.size() >= 8) { s.brow.loop = s.browStrokePts; s.brow.custom = true; BakeBrows(true); } // the full bake follows next frame
            else s.status = "Brow boundary: too short, drag a loop all the way around.";
            Redraw();
            return true;
        }
        if (msg != WM_LBUTTONDOWN && !(msg == WM_MOUSEMOVE && s.browStroke)) return false;
        if (!InitHair()) return true;
        if (msg == WM_LBUTTONDOWN) { s.browStroke = true; s.browStrokePts.clear(); SetCapture(viewport); }
        float o[3], d[3];
        if (s.host.entityRay(s.target, x, y, o, d)) {
            const HairHit h = s.hair.Raycast(o, d, s.headNow);
            if (h.hit) {
                const std::array<float, 2> q = {std::fabs(h.p[0]), h.p[1]};
                if (s.browStrokePts.empty() || std::hypot(q[0] - s.browStrokePts.back()[0], q[1] - s.browStrokePts.back()[1]) > 0.0006f) s.browStrokePts.push_back(q);
            }
        }
        return true;
    }
    if (!s.open || s.tab != 1 || s.hairTool == HairPainter::None) return false;
    if (msg == WM_LBUTTONUP) {
        if (!s.stroking) return false;
        s.stroking = false;
        ReleaseCapture();
        s.bakeNow = true; // the stroke's end always bakes
        Commit(ToolName(s.hairTool));
        return true;
    }
    if (msg == WM_MOUSEMOVE && !s.stroking) {
        float o[3], d[3];
        s.hover = InitHair() && s.host.entityRay(s.target, x, y, o, d) ? s.hair.Raycast(o, d, s.headNow) : HairHit{};
        return false;
    }
    if (msg != WM_LBUTTONDOWN && msg != WM_MOUSEMOVE) return false;
    if (!InitHair()) return msg == WM_LBUTTONDOWN;
    const bool first = msg == WM_LBUTTONDOWN;
    if (first) { Begin(); s.stroking = true; SetCapture(viewport); s.lastHit.hit = false; s.lastMirror.hit = false; s.strokeAt = {x, y}; }
    // one dab per mouse message left gaps on a quick drag: step from the last position every 2 pixels
    const int dx = x - s.strokeAt.x, dy = y - s.strokeAt.y;
    const int steps = first ? 1 : std::clamp((int)(std::sqrt((float)(dx * dx + dy * dy)) / 2.0f), 1, 96);
    for (int k = 1; k <= steps; ++k) {
        const int px = s.strokeAt.x + dx * k / steps, py = s.strokeAt.y + dy * k / steps;
        float o[3], d[3];
        if (s.host.entityRay(s.target, px, py, o, d)) HairDab(o, d, first && k == 1);
    }
    s.strokeAt = {x, y};
    s.hover = s.lastHit;
    return true;
}

// The clay (lit, a little see-through tan) and its comb lines in world space, while the HAIR MAKER clay is loaded and the tab open.
bool ClayDraw(std::vector<Vertex>& tris, std::vector<Vertex>& lines) {
    State& s = S();
    if (!s.open || s.tab != 1 || s.target < 0 || !s.host.toWorld) return false;
    const auto world = [&](const HairClay::V3& p) { float in[3] = {p[0], p[1], p[2]}, w[3]; s.host.toWorld(s.target, in, w); return HairClay::V3{w[0], w[1], w[2]}; };
    if (s.clayLoopDraw && s.clayLoop.size() >= 2) { // the hairline being drawn (or the last one), lifted off the skin a little
        for (size_t k = 0; k + 1 < s.clayLoop.size() + (s.clayLoopStroke ? 0 : 1); ++k) {
            const HairClay::V3 a = world(s.clayLoop[k]), b = world(s.clayLoop[(k + 1) % s.clayLoop.size()]);
            for (const HairClay::V3& p : {a, b}) { Vertex v{}; v.px = p[0]; v.py = p[1]; v.pz = p[2]; v.r = 1.0f; v.g = 0.45f; v.b = 0.1f; v.a = 1; lines.push_back(v); }
        }
    }
    if (s.clay.Empty() || (s.clayMaskMode && s.stroking)) return !lines.empty(); // mid-stroke the paint on the skin shows
    std::vector<HairClay::V3> wp(s.clay.pos.size()), wn(s.clay.pos.size());
    for (size_t k = 0; k < wp.size(); ++k) {
        wp[k] = world(s.clay.pos[k]);
        const HairClay::V3 q = world({s.clay.pos[k][0] + s.clay.nrm[k][0] * 0.01f, s.clay.pos[k][1] + s.clay.nrm[k][1] * 0.01f, s.clay.pos[k][2] + s.clay.nrm[k][2] * 0.01f});
        const float l = std::max(1e-9f, std::sqrt((q[0] - wp[k][0]) * (q[0] - wp[k][0]) + (q[1] - wp[k][1]) * (q[1] - wp[k][1]) + (q[2] - wp[k][2]) * (q[2] - wp[k][2])));
        wn[k] = {(q[0] - wp[k][0]) / l, (q[1] - wp[k][1]) / l, (q[2] - wp[k][2]) / l};
    }
    // With Inflate / Deflate picked the clay is a heat map of its thickness off the scalp: blue = on the skin, through cyan, green
    // and yellow, to red = 30 mm and more. Otherwise hair brown, so it reads as cartoon hair.
    const bool heat = s.clayTool == HairClay::Inflate || s.clayTool == HairClay::Deflate;
    static std::vector<float> thick;
    if (heat) s.clay.Thickness(s.headNow, thick);
    const auto heatColour = [](float mm, float& r, float& g, float& b) {
        const float t = std::clamp(mm / 30.0f, 0.0f, 1.0f) * 4.0f; // 0 blue, 1 cyan, 2 green, 3 yellow, 4 red
        static const float stops[5][3] = {{0.05f, 0.1f, 0.8f}, {0.0f, 0.7f, 0.9f}, {0.1f, 0.8f, 0.15f}, {0.95f, 0.85f, 0.05f}, {0.9f, 0.08f, 0.04f}};
        const int i = std::min(3, (int)t);
        const float f = t - i;
        r = stops[i][0] + (stops[i + 1][0] - stops[i][0]) * f;
        g = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * f;
        b = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * f;
    };
    for (uint32_t i : s.clay.idx) {
        Vertex v{};
        v.px = wp[i][0]; v.py = wp[i][1]; v.pz = wp[i][2];
        v.nx = wn[i][0]; v.ny = wn[i][1]; v.nz = wn[i][2];
        v.r = 0.34f; v.g = 0.22f; v.b = 0.13f; v.a = 1.0f; // hair brown: the clay reads as cartoon hair
        if (heat && i < thick.size()) heatColour(thick[i] * 1000.0f, v.r, v.g, v.b);
        tris.push_back(v);
    }
    const auto line = [&](const HairClay::V3& a, const HairClay::V3& b, float r, float g, float bl) {
        for (const HairClay::V3& p : {a, b}) { Vertex v{}; v.px = p[0]; v.py = p[1]; v.pz = p[2]; v.r = r; v.g = g; v.b = bl; v.a = 1; lines.push_back(v); }
    };
    if (s.clayCombView && !heat) { // off over the heat map: the cyan lines muddied its colours
        std::vector<std::pair<HairClay::V3, HairClay::V3>> comb;
        s.clay.CombLines(comb, 0.006f);
        for (const auto& [a, b] : comb) { const HairClay::V3 wa = world(a), wb = world(b); line(wa, wb, 0.2f, 0.85f, 1.0f); }
    }
    if (s.clayHover && s.clayTool != HairClay::None) { // the brush ring on the clay
        const HairClay::V3& n = s.clayHoverN;
        HairClay::V3 t = {n[1] * 0 - n[2] * 1, 0.0f, n[0] * 1 - n[1] * 0};
        float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (tl < 1e-3f) { t = {1, 0, 0}; tl = 1; }
        t = {t[0] / tl, t[1] / tl, t[2] / tl};
        const HairClay::V3 b = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        const float r = s.hairRadius * 0.001f;
        for (int k = 0; k < 32; ++k) {
            const float a0 = k / 32.0f * 6.2831853f, a1 = (k + 1) / 32.0f * 6.2831853f;
            const auto at = [&](float a) { return HairClay::V3{s.clayHoverP[0] + (t[0] * std::cos(a) + b[0] * std::sin(a)) * r + n[0] * 0.001f,
                                                                s.clayHoverP[1] + (t[1] * std::cos(a) + b[1] * std::sin(a)) * r + n[1] * 0.001f,
                                                                s.clayHoverP[2] + (t[2] * std::cos(a) + b[2] * std::sin(a)) * r + n[2] * 0.001f}; };
            line(world(at(a0)), world(at(a1)), 1.0f, 0.8f, 0.3f);
        }
    }
    return true;
}
bool HideHeadHair() {
    const State& s = S();
    return s.open && s.tab == 1 && (!s.clay.Empty() || s.clayLoopDraw || s.clayMaskMode) && !s.clayShowHair;
}

bool BrushLines(std::vector<Vertex>& out) {
    State& s = S();
    if (s.open && s.tab == 1 && (s.browDraw || s.browGuides) && s.target >= 0 && s.hairReady) {
        // the brow guides on the head, both sides: the boundary (magenta), the arch = the band's centre line (blue), and the inner start
        // (green), the peak (yellow) and the tail end (red) as crosses. Rebuilt at most every 150 ms (each point is a ray at the head).
        if (s.browStroke || NowMs() - s.browGuideAt > 150) {
            s.browGuideAt = NowMs();
            std::vector<Vertex>& L = s.browGuideLines;
            L.clear();
            const auto onSkin = [&](float x, float y, float w[3]) {
                const float o[3] = {x, y, 0.4f}, d[3] = {0, 0, -1};
                const HairHit h = s.hair.Raycast(o, d, s.headNow);
                if (!h.hit) return false;
                const float p[3] = {h.p[0] + h.n[0] * 0.0012f, h.p[1] + h.n[1] * 0.0012f, h.p[2] + h.n[2] * 0.0012f};
                s.host.toWorld(s.target, p, w);
                return true;
            };
            const auto polyline = [&](const std::vector<std::array<float, 2>>& pts, bool closed, float r, float g, float b) {
                for (const float side : {1.0f, -1.0f}) {
                    float prev[3] = {};
                    bool have = false;
                    const size_t n = pts.size() + (closed ? 1 : 0);
                    for (size_t k = 0; k < n; ++k) {
                        const auto& q = pts[k % pts.size()];
                        float w[3];
                        if (!onSkin(q[0] * side, q[1], w)) { have = false; continue; }
                        if (have) { L.push_back({prev[0], prev[1], prev[2], 0, 0, 0, r, g, b, 1.0f}); L.push_back({w[0], w[1], w[2], 0, 0, 0, r, g, b, 1.0f}); }
                        prev[0] = w[0]; prev[1] = w[1]; prev[2] = w[2];
                        have = true;
                    }
                }
            };
            if (s.brow.loop.size() < 6 && !s.browStroke) { Json doc; MorphSet::ReadJson(s.facePath, doc, nullptr);
                if (const Json* lm = doc.Find("landmarks")) if (const Json* by = lm->Find("brow_y"); by && by->type == Json::Number) s.brow.browY = (float)by->n;
                s.brow.DefaultLoop(); }
            const auto& loop = s.browStroke ? s.browStrokePts : s.brow.loop;
            if (loop.size() >= 2) polyline(loop, !s.browStroke, 1.0f, 0.2f, 0.8f);
            if (!s.browStroke && s.brow.loop.size() >= 6) {
                float X0, X1;
                s.brow.Extent(X0, X1);
                std::vector<std::array<float, 2>> mid;
                float peakX = X0, peakY = -1e9f;
                for (int k = 0; k <= 48; ++k) {
                    const float x = X0 + (X1 - X0) * (0.01f + 0.98f * k / 48.0f);
                    float c, h;
                    if (!s.brow.Band(x, c, h)) continue;
                    mid.push_back({x, c});
                    if (c > peakY) { peakY = c; peakX = x; }
                }
                if (mid.size() >= 2) {
                    polyline(mid, false, 0.25f, 0.55f, 1.0f);
                    const auto cross = [&](float x, float y, float r, float g, float b) {
                        const float a = 0.0022f;
                        polyline({{x - a, y}, {x + a, y}}, false, r, g, b);
                        polyline({{x, y - a}, {x, y + a}}, false, r, g, b);
                    };
                    cross(mid.front()[0], mid.front()[1], 0.3f, 0.9f, 0.35f); // inner start
                    cross(peakX, peakY, 1.0f, 0.86f, 0.15f);                  // peak
                    cross(mid.back()[0], mid.back()[1], 1.0f, 0.27f, 0.23f);  // tail end
                }
            }
        }
        if (!s.browGuideLines.empty() && (s.browDraw || s.hairTool == HairPainter::None || !s.hover.hit)) {
            out.insert(out.end(), s.browGuideLines.begin(), s.browGuideLines.end());
            return true;
        }
    }
    if (!s.open || s.tab != 1 || s.hairTool == HairPainter::None || !s.hover.hit || s.target < 0) return false;
    const HairHit& h = s.hover;
    // a frame on the surface: t, b perpendicular to the normal
    const float* n = h.n;
    float t[3] = {n[1] * 0 - n[2] * 1, n[2] * 0 - n[0] * 0, n[0] * 1 - n[1] * 0}; // n x up
    float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (tl < 1e-3f) { t[0] = 1; t[1] = 0; t[2] = 0; tl = 1; }
    for (float& c : t) c /= tl;
    const float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
    float tint[3] = {1, 1, 1};
    if (s.hairTool == HairPainter::Paint) { tint[0] = 1.0f; tint[1] = 0.85f; tint[2] = 0.55f; }
    else if (s.hairTool == HairPainter::Erase) { tint[0] = 1.0f; tint[1] = 0.3f; tint[2] = 0.25f; }
    else if (s.hairTool == HairPainter::Comb) { tint[0] = 0.35f; tint[1] = 0.8f; tint[2] = 1.0f; }
    else if (s.hairTool == SkinPaint::TintPaint) { for (int c = 0; c < 3; ++c) tint[c] = SkinPaint::Tint(s.tintType).rgb[c] / 255.0f; }
    else if (s.hairTool == SkinPaint::SpotPaint) { tint[0] = 1.0f; tint[1] = 0.78f; tint[2] = 0.35f; }
    else if (s.hairTool == SkinPaint::ShinePaint) { tint[0] = 1.0f; tint[1] = 1.0f; tint[2] = 1.0f; }
    else if (s.hairTool == SkinPaint::TintErase || s.hairTool == SkinPaint::SpotErase || s.hairTool == SkinPaint::ShineErase) { tint[0] = 1.0f; tint[1] = 0.3f; tint[2] = 0.25f; }
    else { tint[0] = 0.75f; tint[1] = 1.0f; tint[2] = 0.55f; }
    const float R = s.hairRadius * 0.001f, lift = 0.0015f;
    const float hard = std::clamp(s.hairHardness, 0.0f, 0.99f);
    const auto ring = [&](float r, float bright) {
        constexpr int K = 48;
        float prev[3] = {};
        for (int k = 0; k <= K; ++k) {
            const float a = 6.2831853f * (float)k / K, ca = std::cos(a) * r, sa = std::sin(a) * r;
            const float p[3] = {h.p[0] + t[0] * ca + b[0] * sa + n[0] * lift, h.p[1] + t[1] * ca + b[1] * sa + n[1] * lift, h.p[2] + t[2] * ca + b[2] * sa + n[2] * lift};
            float w[3];
            s.host.toWorld(s.target, p, w);
            if (k > 0) {
                out.push_back({prev[0], prev[1], prev[2], 0, 0, 0, tint[0] * bright, tint[1] * bright, tint[2] * bright, 1.0f});
                out.push_back({w[0], w[1], w[2], 0, 0, 0, tint[0] * bright, tint[1] * bright, tint[2] * bright, 1.0f});
            }
            prev[0] = w[0]; prev[1] = w[1]; prev[2] = w[2];
        }
    };
    ring(R, 1.0f); // the radius, always full bright
    for (int k = 1; k <= 6; ++k) { // the falloff: brighter where the brush paints harder (strength x falloff)
        const float q = (float)k / 7.0f;
        ring(R * q, 0.15f + 0.85f * s.hairStrength * HairPainter::Falloff(q, hard));
    }
    if (hard > 0.01f) ring(R * hard, 0.9f); // where the hard core ends
    // the centre: a small cross
    for (int k = 0; k < 2; ++k) {
        const float* ax = k ? b : t;
        float p0[3], p1[3], w0[3], w1[3];
        for (int c = 0; c < 3; ++c) { p0[c] = h.p[c] - ax[c] * R * 0.12f + n[c] * lift; p1[c] = h.p[c] + ax[c] * R * 0.12f + n[c] * lift; }
        s.host.toWorld(s.target, p0, w0); s.host.toWorld(s.target, p1, w1);
        out.push_back({w0[0], w0[1], w0[2], 0, 0, 0, 1, 1, 1, 1.0f});
        out.push_back({w1[0], w1[1], w1[2], 0, 0, 0, 1, 1, 1, 1.0f});
    }
    return true;
}

void MorphEntityVertices(int i, std::vector<Vertex>& v) {
    State& s = S();
    if (i != s.target || !s.head.Bound() || HeadNeutral() || v.size() != s.head.Rest().size()) return;
    const bool neck = s.bodyLoaded && s.bodyHead.Valid();
    s.head.Deform(s.values, v, neck ? &s.bodyHead : nullptr, neck ? &s.bodyValues : nullptr);
    ApplyEyePlacement(v);
}

void Tick() {
    State& s = S();
    if (!s.hostSet) return;
    ClayPoll();
    ClothPoll();
    if (!s.envDone && s.host.sceneReady()) {
        s.envDone = true;
        wchar_t preset[MAX_PATH] = {};
        char seed[16] = {}, tab[16] = {}, hairTest[8] = {}, bodySeed[16] = {};
        const bool hasPreset = GetEnvironmentVariableW(L"TBD_CHARPRESET", preset, MAX_PATH) > 0;
        const bool hasSeed = GetEnvironmentVariableA("TBD_CHARSEED", seed, sizeof(seed)) > 0;
        if (GetEnvironmentVariableA("TBD_CHARTAB", tab, sizeof(tab)) > 0) { s.tab = tab[0] == 'b' ? 0 : tab[0] == 'c' ? 2 : 1; Open(true); if (tab[0] == 'p') SelectTab(3); }
        char clothRecipe[MAX_PATH] = {}; // TBD_CLOTHBUILD=1 (or a recipe.json): build the shirt headless once the scene is in
        if (GetEnvironmentVariableA("TBD_CLOTHBUILD", clothRecipe, sizeof(clothRecipe)) > 0) {
            if (strlen(clothRecipe) > 2) { std::string err; if (!s.shirt.Load(Utf8ToWide(clothRecipe), &err)) Log("Character: TBD_CLOTHBUILD recipe: %s", err.c_str()); }
            StartClothBuild();
        }
        char view[8] = {}; // TBD_CHARVIEW=0 3D, 1 2D, 2 3D + 2D
        if (GetEnvironmentVariableA("TBD_CHARVIEW", view, sizeof(view)) > 0) { Open(true); Do(A_View3D + std::clamp(atoi(view), 0, 2)); }
        // TBD_CHARHAIRLINE=1: a typical hairline drawn by rays at the head (forehead high, over the ears, low at the nape) -> the clay
        if (GetEnvironmentVariableA("TBD_CHARHAIRLINE", nullptr, 0) > 0 && (Open(true), BindHead()) && InitHair()) {
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (const Vertex& v : s.headNow) if (std::fabs(v.a - 0.5f) < 0.1f) { const float p[3] = {v.px, v.py, v.pz}; for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], p[c]); hi[c] = std::max(hi[c], p[c]); } }
            const float cx = 0.0f, cy = hi[1] - 0.09f, cz = (lo[2] + hi[2]) * 0.5f;
            s.clayLoop.clear();
            for (int k = 0; k < 120; ++k) {
                const float az = k / 120.0f * 6.2831853f; // 0 = the front (+z)
                const float el = (0.62f - 0.47f * (0.5f - 0.5f * std::cos(az))) ; // radians above the skull centre: high at the front, low at the back
                const float d[3] = {-std::sin(az) * std::cos(el), -std::sin(el), -std::cos(az) * std::cos(el)};
                const float o[3] = {cx - d[0] * 0.3f, cy - d[1] * 0.3f, cz - d[2] * 0.3f};
                const HairHit h = s.hair.Raycast(o, d, s.headNow);
                if (h.hit) s.clayLoop.push_back({h.p[0], h.p[1], h.p[2]});
            }
            if (!s.clayLoop.empty()) s.clayLoop.push_back(s.clayLoop.front());
            ClayFromLoop();
            Log("Character: TBD_CHARHAIRLINE: %s", s.status.c_str());
        }
        // TBD_CHARSCALP=1: the default scalp painted (the hair painter's Fill) -> the clay, as the PAINT SCALP workflow does
        if (GetEnvironmentVariableA("TBD_CHARSCALP", nullptr, 0) > 0 && (Open(true), BindHead()) && InitHair()) {
            s.hair.FillDefaults();
            s.clayMaskMode = true;
            ClayFromMask();
            Log("Character: TBD_CHARSCALP: %s", s.status.c_str());
        }
        char clayTool[8] = {}; // TBD_CHARCLAYTOOL=<1 inflate .. 5 comb>: pick a clay tool (1 or 2 shows the thickness heat map)
        if (GetEnvironmentVariableA("TBD_CHARCLAYTOOL", clayTool, sizeof(clayTool)) > 0) { Open(true); Do(A_ClayTool + std::clamp(atoi(clayTool), 1, 5)); }
        char groomEnv[64] = {}; // TBD_CHARGROOM=<style>|none: the HAIR STYLE picker's action, for captures and tests
        if (GetEnvironmentVariableA("TBD_CHARGROOM", groomEnv, sizeof(groomEnv)) > 0) { Open(true); ApplyGroom(groomEnv); }
        if (hasPreset) LoadPreset(preset);
        else if (hasSeed && LoadFace()) { s.seed = (uint32_t)atoi(seed); s.values = s.face.Randomize(s.seed, 0); s.faceDirty = true; }
        if (GetEnvironmentVariableA("TBD_CHARBODYSEED", bodySeed, sizeof(bodySeed)) > 0) { // headless body Randomize
            PollBody();
            EnsureBody();
            s.bodySeed = (uint32_t)atoi(bodySeed);
            Do(A_BodyRandom);
            if (s.tab == 0) FrameCamera();
        }
        if (s.bodyDirty) {
            ApplyBody();
            Log("Character: headless body -> %s, height scale %.3f", s.status.c_str(), s.heightScale);
        }
        if (hasPreset || hasSeed) {
            ApplyFace();
            Log("Character: headless %s -> %s", hasPreset ? "TBD_CHARPRESET" : "TBD_CHARSEED", s.status.c_str());
        }
        if (GetEnvironmentVariableA("TBD_CHARHAIRTEST", hairTest, sizeof(hairTest)) > 0) HairTest(atoi(hairTest));
        if (GetEnvironmentVariableA("TBD_CHARFEATURES", nullptr, 0) > 0 && !s.features) { Open(true); Do(A_Features); } // feature view on
        char bt[64] = {}; // TBD_CHARBROWS=<density>,<length>,<shape>: bake the brow maker headless (shape 1: a test-drawn boundary, higher and slimmer)
        if (GetEnvironmentVariableA("TBD_CHARBROWS", bt, sizeof(bt)) > 0) {
            float dn = 1, ln = 1; int shape = 0, prev = 0; // 4th value 1: a preview bake (timed, left showing)
            sscanf_s(bt, "%f,%f,%d,%d", &dn, &ln, &shape, &prev);
            s.brow.density = dn; s.brow.length = ln;
            if (shape == 1) { // a slim, high-arched brow drawn as a loop
                s.brow.loop.clear();
                for (int k = 0; k <= 24; ++k) { const float t = k / 24.0f; s.brow.loop.push_back({0.013f + 0.046f * t, 0.833f + 0.010f * std::sin(3.14159f * std::min(1.0f, t * 1.25f)) - 0.006f * t + 0.0022f * (1 - t)}); }
                for (int k = 24; k >= 0; --k) { const float t = k / 24.0f; s.brow.loop.push_back({0.013f + 0.046f * t, 0.833f + 0.010f * std::sin(3.14159f * std::min(1.0f, t * 1.25f)) - 0.006f * t - 0.0022f * (1 - t) - 0.0006f}); }
                s.brow.custom = true;
            }
            char st[512] = {}; // TBD_CHARBROWSTYLE=key=value,...: the brow sliders by their preset keys (kBrowSliders)
            if (GetEnvironmentVariableA("TBD_CHARBROWSTYLE", st, sizeof(st)) > 0) {
                for (char* tok = strtok(st, ","); tok; tok = strtok(nullptr, ",")) {
                    char* eq = strchr(tok, '=');
                    if (!eq) continue;
                    *eq = 0;
                    for (const BrowSlider& b : kBrowSliders) if (strcmp(b.key, tok) == 0) s.brow.*b.field = std::clamp((float)atof(eq + 1), b.lo, b.hi);
                }
                if (!s.brow.custom) { s.brow.loop.clear(); } // rebuilt from the style in BakeBrows
            }
            BakeBrows(prev == 1);
            if (prev == 1) s.browFull = false; // keep the preview on screen for the capture
            Log("Character: TBD_CHARBROWS -> %s", s.status.c_str());
        }
        char skinTest[8] = {}; // TBD_CHARSKINTEST=1: scripted skin paint (inner corners, blush, freckles, moles, shine); =2..4 also shows map 3..5 in 2D
        if (GetEnvironmentVariableA("TBD_CHARSKINTEST", skinTest, sizeof(skinTest)) > 0 && InitHair()) {
            Open(true);
            if (s.headNow.empty()) s.headNow = s.head.Rest();
            Do(A_InnerCorner);
            const auto dab = [&](int tool, float x, float y, float r, float st, int n) {
                s.hairTool = tool; s.hairRadius = r; s.hairStrength = st; s.hairHardness = 0.0f;
                const float o[3] = {x, y, 0.5f}, d[3] = {0, 0, -1};
                for (int k = 0; k < n; ++k) DoDab(s.hair.Raycast(o, d, s.headNow), 0, 0);
            };
            s.tintType = 1;
            for (const float side : {1.0f, -1.0f}) {
                dab(SkinPaint::TintPaint, side * 0.046f, 0.790f, 22.0f, 0.30f, 2);
                s.spotKind = 0; dab(SkinPaint::SpotPaint, side * 0.040f, 0.800f, 16.0f, 0.9f, 3);
                s.spotKind = 0; dab(SkinPaint::SpotPaint, 0.0f, 0.780f, 9.0f, 0.9f, 3);
            }
            s.spotKind = 1; dab(SkinPaint::SpotPaint, -0.030f, 0.765f, 4.0f, 1.0f, 3);
            s.spotKind = 2; dab(SkinPaint::SpotPaint, 0.060f, 0.840f, 12.0f, 1.0f, 3);
            dab(SkinPaint::ShinePaint, 0.0f, 0.790f, 9.0f, 1.0f, 4);
            dab(SkinPaint::ShinePaint, 0.0f, 0.862f, 16.0f, 1.0f, 4);
            s.hairTool = HairPainter::None;
            s.surfDirty = true;
            if (skinTest[0] >= '2') { s.mapView = skinTest[0] - '2' + 3; s.view = 2; }
            BakeHair();
            Log("Character: skin paint test done (tint %d, spots %d, shine %d)", (int)s.sp.tintPainted, (int)s.sp.spotsPainted, (int)s.sp.shinePainted);
        }
        if (GetEnvironmentVariableA("TBD_CHAREYETEST", nullptr, 0) > 0) { // scripted eye look on the zoomed eyes
            Open(true);
            Do(A_EyeZoom);
            s.eye.irisAmount = 0.85f; s.eye.irisColor[0] = 0.22f; s.eye.irisColor[1] = 0.48f; s.eye.irisColor[2] = 0.40f;
            s.eye.redness = 0.45f; s.eye.corner = 0.6f; s.eye.veins = 0.8f; s.eye.limbal = 0.6f;
            char pl[64] = {}; // TBD_CHAREYEPLACE="depth spread height size" (mm, mm, mm, factor)
            if (GetEnvironmentVariableA("TBD_CHAREYEPLACE", pl, sizeof(pl)) > 0) { sscanf(pl, "%f %f %f %f", &s.eyePlace[0], &s.eyePlace[1], &s.eyePlace[2], &s.eyePlace[3]); s.faceDirty = true; }
            BakeEye();
            Log("Character: eye test done (eye texture %d)", (int)(s.eyeTex != kNoTexture));
        }
        char skinName[64] = {}; // TBD_CHARSKIN=<name>: press that skin button (after the other edits, to check they are kept)
        if (GetEnvironmentVariableA("TBD_CHARSKIN", skinName, sizeof(skinName)) > 0) {
            ScanLooks();
            for (size_t k = 0; k < s.skins.size(); ++k) if (s.skins[k] == skinName) ApplySkin((int)k);
        }
        char t2d[8] = {}; // TBD_CHAR2DTEST=1: Paint tool, hardness 0.5, 2D view zoomed 3x at its centre with the brush shown there
        if (GetEnvironmentVariableA("TBD_CHAR2DTEST", t2d, sizeof(t2d)) > 0 && s.wnd2d) {
            s.hairTool = HairPainter::Paint; s.hairRadius = 20.0f; s.hairHardness = 0.5f;
            RECT cr; GetClientRect(s.wnd2d, &cr);
            s.cu2d = 0.78f; s.cv2d = 0.45f;
            Zoom2D(s.wnd2d, cr.right / 2, cr.bottom / 2, 3.0f);
            s.hover2d = true; s.mouse2d = {cr.right / 2, cr.bottom / 2};
            InvalidateRect(s.wnd2d, nullptr, FALSE); Redraw();
        }
    }
    if (s.open) {
        const bool had = s.bodyLoaded;
        PollBody();
        if (s.bodyLoaded && !had) Redraw();
    }
    if (s.bodyDirty) { ApplyBody(); Redraw(); }
    if (s.browFull && !s.drag && !s.browStroke) BakeBrows(); // a preview is showing and nothing is being dragged: the full bake
    if (s.faceDirty) { ApplyFace(); Redraw(); }
    // hair: bake at the stroke's end, and every frame while painting if the last bake was quick
    const double now = NowMs();
    if (s.bakeNow || (s.hairDirty && s.stroking && now - s.lastLiveBake > std::max(120.0, 1.2 * (s.bakeMs + s.texMs)))) {
        BakeHair();
        s.lastLiveBake = NowMs();
        Redraw();
    }
    if (s.open && s.target >= 0 && !s.eyeAuto) { s.eyeAuto = true; BakeEye(); Redraw(); } // the brown default goes on as soon as the head is bound
    if (s.eyeDirty && now - s.eyeLastBake > 90.0) { BakeEye(); Redraw(); } // eye sliders: the texture follows the drag
}

void Shutdown() {
    State& s = S();
    if (s.clothJob.joinable()) s.clothJob.join();
    if (s.wnd) DestroyWindow(s.wnd);
    if (s.wnd2d) DestroyWindow(s.wnd2d);
    s.wnd = s.wnd2d = nullptr;
    DestroyFonts(s.st);
}

} // namespace tbd::character
#pragma pop_macro("small")
