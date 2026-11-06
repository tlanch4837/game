#define UNICODE
#define _UNICODE
#include "face_live_tab.h"
#include "face_clip_player.h"
#include "face_lipsync.h"
#include "face_retarget.h"

#include <commdlg.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

#include "face_draw.h"
#include "face_fit.h"
#include "face_map.h"
#include "face_phone.h"

namespace tbd {
namespace {

using namespace facedraw;

const wchar_t* kStripClass = L"TBDTabStrip";
const wchar_t* kPageClass = L"TBDFaceTabPage";
constexpr UINT_PTR kTimer = 1;
constexpr size_t kMaxVerts = 400000;

enum Action {
    A_None, A_Record, A_Calibrate, A_UseSelected, A_Release, A_Head, A_Mirror, A_Auto, A_Manual, A_Refit, A_Save,
    A_Load, A_ResetAuto, A_View, A_PlayClip, A_MaxGraphics, A_Source, A_Slider, A_Select = 100, A_Toggle = 200
};

struct Hit {
    RECT r;
    int action;
    float* value = nullptr; // sliders
    float lo = 0, hi = 1;
};

COLORREF RegionColor(FaceRegion r) {
    switch (r) {
    case FaceRegion::Brows: return RGB(255, 200, 60);
    case FaceRegion::Eyes: return RGB(80, 200, 255);
    case FaceRegion::Nose: return RGB(240, 140, 200);
    case FaceRegion::Lips: return RGB(255, 90, 90);
    case FaceRegion::Cheeks: return RGB(160, 230, 120);
    case FaceRegion::Jaw: return RGB(200, 160, 255);
    default: return RGB(120, 120, 120);
    }
}

std::wstring Wide(const char* s) { return std::wstring(s, s + strlen(s)); }

struct Tab {
    FaceTabHost host;
    HWND strip = nullptr, page = nullptr;
    bool pageActive = true;
    Style st;
    // live data
    FaceFrame frame;
    std::vector<uint8_t> image;
    int w = 0, h = 0;
    uint64_t seq = 0;
    bool have = false;
    // target + rig
    int target = -1;
    std::string targetName;
    FaceRig rig;
    bool headDrive = true, mirror = true; // mirror is the default: the character acts like your reflection
    float strength = 1;
    int sel = BS_jawOpen;
    int listTop = 0; // first visible row of the channel list
    std::string status = "Select a head in the scene, then press Use selected.";
    float lastWeights[kBlendshapes] = {};
    float lastPose[3] = {}, lastHead[3] = {}; // head degrees: newest tracked, and last sent to the mesh
    bool dirty = true;
    FaceClipPlayer player; // loaded = a saved clip drives the rig instead of the webcam
    FaceRetarget retarget;
    // lip sync: the mic runs while recording; afterwards a worker transcribes and analyses, Tick picks up the result
    MicRecorder mic;
    LipTrack lips; // of the loaded clip, empty when it has none
    float lipStrength = 1;
    std::thread lipJob;
    std::atomic<int> lipState{0}; // 0 idle, 1 working, 2 finished
    std::string lipResult;        // written by the worker before state 2
    std::wstring lipClip;
    bool maxGraphics = false;
    double retargetTime = 0;
    ULONGLONG clipStart = 0;
    // ui
    std::vector<Hit> hits;
    int dragHit = -1;
    RECT viewRect = {};
    std::vector<float> influence;
    bool dotsDirty = true;
    bool view3D = true; // middle panel: the live 3D character (true) or the labelled face map (false)

    bool Manual() const { return rig.Mapping().mode == MapMode::Manual; }

    // The middle panel of the page: where the live character or the face map goes.
    RECT MidRect(int W, int H) const {
        const int m = st.S(10), side = st.S(340);
        const int mainW = W - side - 3 * m;
        return {m + mainW / 2 + m, m, m + mainW + m, H - m};
    }

    void SetView3D(bool on) {
        if (view3D == on) return;
        view3D = on;
        host.relayout();
    }

    void StopClip() {
        if (!player.Loaded()) return;
        player.Clear();
        lips.Clear();
        PlaySoundW(nullptr, nullptr, 0);
        if (host.overrideHeadPose) host.overrideHeadPose(nullptr, nullptr);
        have = false;
        seq = 0; // show the webcam again from its next frame
        dirty = true;
    }

    void PlayClip(HWND hwnd) {
        if (player.Loaded()) { StopClip(); status = "Clip stopped: back to the webcam."; return; }
        wchar_t buf[MAX_PATH] = L"";
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Face clip (*.tbdface)\0*.tbdface\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        LoadClip(buf);
    }

    void LoadClip(const std::wstring& path) {
        if (!player.Load(path.c_str())) { status = "That clip has no tracked face, or could not be read."; return; }
        const std::wstring stem = path.substr(0, path.find_last_of(L'.'));
        lips.Load((stem + L".tbdlips").c_str());
        PlaySoundW(nullptr, nullptr, 0);
        clipStart = GetTickCount64();
        if (lips.Loaded()) PlaySoundW((stem + L".wav").c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
        image.clear(); // the clip holds no pictures
        w = h = 0;
        dirty = true;
        status = target >= 0 ? "Playing the clip on " + targetName + (lips.Loaded() ? ", with lip sync." : ".") : "Clip loaded: press Use selected to see it on a head.";
    }

    // Saved clip + what the mic heard: transcribe on the GPU, then build the lip track beside the clip.
    void StartLipJob(const std::wstring& clipPath, std::vector<int16_t> pcm, double seconds) {
        if (lipJob.joinable()) lipJob.join();
        pcm.resize((size_t)(seconds * kLipSampleRate)); // same length as the clip, so they loop together
        lipClip = clipPath;
        lipState = 1;
        lipJob = std::thread([this, clipPath, pcm = std::move(pcm)] {
            const std::wstring stem = clipPath.substr(0, clipPath.find_last_of(L'.'));
            const std::wstring wav = stem + L".wav", wordsPath = stem + L".words.txt";
            std::string err;
            std::vector<LipWord> words;
            if (!WriteWav16k(wav.c_str(), pcm)) lipResult = "Could not write the audio.";
            else {
                const bool heard = TranscribeWav(wav, wordsPath, &err);
                if (heard) words = ReadLipWords(wordsPath.c_str());
                const LipTrack track = AnalyzeLips(pcm, words);
                if (!track.Save((stem + L".tbdlips").c_str())) lipResult = "Could not write the lip track.";
                else if (!heard) lipResult = "Transcription failed (" + err + "): lip sync from audio alone, no b/m/p/f/v closures.";
                else lipResult = "Lip sync ready, " + std::to_string(words.size()) + " words heard.";
            }
            lipState = 2;
        });
    }

    void Pull() {
        if (player.Loaded()) { // the clip stands in for the webcam
            FaceFrame f;
            if (player.Sample((GetTickCount64() - clipStart) / 1000.0, f)) {
                frame = f;
                have = true;
                if (host.overrideHeadPose) host.overrideHeadPose(f.rot, player.Neutral());
            }
            return;
        }
        FaceFrame f;
        uint64_t s = 0;
        if (host.live->Latest(&f, &image, &w, &h, &s) && s != seq) {
            frame = f;
            seq = s;
            have = true;
        }
    }

    // Webcam <-> iPhone. A port already in use leaves the webcam running.
    void SwitchSource() {
        StopClip();
        std::string err;
        const bool toPhone = !host.live->IsPhone();
        const bool ok = toPhone ? host.live->StartPhone(kLiveLinkPort, &err) : host.live->Start(-1, nullptr, host.modelDir.c_str(), &err);
        if (!ok) { status = err; return; }
        image.clear();
        w = h = 0;
        seq = 0;
        have = false;
        retarget.Reset();
        dirty = true;
        status = toPhone ? "Listening for the iPhone. Hold a relaxed face while it calibrates." : "Back to the webcam.";
    }

    void Release() {
        if (target >= 0) {
            host.restoreMesh(target);
            host.driveHead(target, false);
        }
        target = -1;
        targetName.clear();
        rig = FaceRig();
    }

    void UseSelected() {
        const int i = host.selectedIndex();
        if (i < 0) { status = "No entity selected: click a head in the viewport first."; return; }
        std::vector<Vertex> v;
        if (!host.entityMesh(i, &v) || v.size() < 3) { status = "That entity has no mesh to deform."; return; }
        if (v.size() > kMaxVerts) { status = "Mesh too large (over 400k vertices): remesh it smaller first."; return; }
        Release();
        std::vector<FitPart> parts;
        if (host.entityParts) host.entityParts(i, &parts);
        float lm[kFaceLandmarks * 3];
        std::string err;
        std::vector<Vertex> fitVerts;
        std::vector<FitPart> fitParts;
        WithoutHair(v, parts, fitVerts, fitParts);
        const bool fitted = FitFaceLandmarks(fitVerts, fitParts, host.modelDir.c_str(), lm, &err);
        const RigExtras extras = RigExtrasFromParts(parts);
        if (!rig.Init(v, fitted ? lm : nullptr, fitted ? &extras : nullptr)) { status = "Could not read the mesh."; return; }
        std::vector<Vertex> rest, relaxed;
        if (fitted && host.lidRelaxed && host.lidRelaxed(i, &rest, &relaxed)) rig.SetLidRelax(rest, relaxed);
        target = i;
        targetName = host.entityName(i);
        if (headDrive && !fitted) host.driveHead(i, true); // a fitted rig turns the head itself, with the neck held still
        if (host.frameHead && fitted) {
            const float* nose = rig.Landmarks() + 6 * 3; // between the eyes and the mouth
            host.frameHead(i, nose, rig.FaceWidth());
        }
        retarget.Reset();
        status = fitted && !rig.HasMouth()
                     ? "Fitted to " + targetName + ", but its lips are one welded surface: the mouth cannot open. Use a head modelled with a mouth."
                 : fitted ? "Fitted to " + targetName + ". Switch to Manual to tune channels."
                        : "Could not find a face on " + targetName + " (" + err + "): expressions are off, only head rotation works.";
        dirty = dotsDirty = true;
        listTop = 0;
        std::fill(lastWeights, lastWeights + kBlendshapes, -1.0f);
    }

    void Rebuild() {
        rig.Rebuild();
        dirty = dotsDirty = true;
    }

    // ---- per frame: pose the target
    void Tick() {
        if (lipState == 2) {
            lipJob.join();
            lipState = 0;
            const std::string done = lipResult;
            if (!host.live->Recording()) {
                StopClip();
                LoadClip(lipClip);
            }
            status = done;
        }
        if (!player.Loaded() && (!host.live || !host.live->Running())) return;
        Pull();
        if (target < 0 || !rig.Fitted()) return;
        if (host.entityName(target) != targetName) { // deleted or reordered
            target = -1;
            rig = FaceRig();
            status = "Target entity is gone.";
            return;
        }
        float wts[kBlendshapes] = {};
        if (have && frame.valid) {
            float sum = 0;
            for (int c = 0; c < kBlendshapes; ++c) sum += frame.bs[c];
            if (sum > 0) std::copy(frame.bs, frame.bs + kBlendshapes, wts);
            else BlendshapesFromChannels(frame.weight, wts); // the blendshape model is not installed
            // an iPhone measures depth, so it has none of the webcam's look-down blink to take off
            if (player.Loaded() || !host.live->IsPhone()) retarget.Apply(wts, (float)std::max(frame.time - retargetTime, 0.0));
            retargetTime = frame.time;
            for (int c = 0; c < kBlendshapes; ++c) wts[c] *= strength;
            if (player.Loaded() && lips.Loaded()) {
                float v[kLipVisemes];
                lips.Sample(player.LocalTime((GetTickCount64() - clipStart) / 1000.0), v);
                ApplyLips(v, lipStrength, wts);
            }
        }
        if (mirror) {
            float m[kBlendshapes];
            for (int c = 0; c < kBlendshapes; ++c) m[MirroredBlendshape(c)] = wts[c];
            std::copy(m, m + kBlendshapes, wts);
        }
        // Degrees from the neutral pose, in the same sense FaceHeadRotation gave the whole entity.
        float head[3] = {};
        if (headDrive) {
            float pose[3] = {}, neutral[3] = {};
            bool known = false;
            if (player.Loaded()) {
                if (have && frame.valid) { std::copy(frame.rot, frame.rot + 3, pose); std::copy(player.Neutral(), player.Neutral() + 3, neutral); known = true; }
            } else known = host.live->Pose(pose, neutral);
            const float m = mirror ? -1.0f : 1.0f;
            if (known) { lastPose[0] = pose[0] - neutral[0]; lastPose[1] = (pose[1] - neutral[1]) * m; lastPose[2] = -(pose[2] - neutral[2]) * m; }
            std::copy(lastPose, lastPose + 3, head); // holds the last pose while no face is tracked
        }
        bool changed = dirty;
        for (int c = 0; c < kBlendshapes; ++c)
            if (std::abs(wts[c] - lastWeights[c]) > 0.004f) changed = true;
        for (int k = 0; k < 3; ++k)
            if (std::abs(head[k] - lastHead[k]) > 0.2f) changed = true;
        if (!changed) return;
        std::copy(wts, wts + kBlendshapes, lastWeights);
        std::copy(head, head + 3, lastHead);
        dirty = false;
        std::vector<Vertex> out;
        rig.Pose(wts, out, head);
        host.setMesh(target, out, rig.PartRanges());
    }

    // ---- drawing
    void Button(HDC dc, RECT r, const wchar_t* label, int action, bool on = false) {
        Fill(dc, r.left, r.top, r.right, r.bottom, on ? kAccent : RGB(52, 46, 40));
        SetBkMode(dc, TRANSPARENT);
        SIZE sz;
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &sz);
        Text(dc, (r.left + r.right) / 2, r.top + (r.bottom - r.top - sz.cy) / 2, label, kText, st.small, TA_CENTER);
        hits.push_back({r, action});
    }

    void Slider(HDC dc, int x, int y, int width, const wchar_t* label, float* v, float lo, float hi) {
        const int lh = st.S(18);
        Text(dc, x, y, label, kMuted, st.small);
        RECT tr = {x + st.S(70), y + st.S(4), x + width - st.S(46), y + lh - st.S(2)};
        Fill(dc, tr.left, tr.top + (tr.bottom - tr.top) / 2 - 1, tr.right, tr.top + (tr.bottom - tr.top) / 2 + 1, kPanel);
        const float t = std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f);
        const int kx = tr.left + (int)(t * (tr.right - tr.left));
        Fill(dc, kx - st.S(3), tr.top, kx + st.S(3), tr.bottom, kAccent);
        wchar_t b[24];
        swprintf_s(b, L"%.2f", *v);
        Text(dc, x + width, y, b, kText, st.small, TA_RIGHT);
        Hit h{tr, A_Slider, v, lo, hi};
        h.r.top -= st.S(2);
        h.r.bottom += st.S(2);
        hits.push_back(h);
    }

    // The labelled face map: every mesh vertex as a dot (tinted by how much the selected channel moves it) with the 468
    // detected landmarks over it, colored by the face region they belong to.
    void DrawRigView(HDC dc, RECT area) {
        Fill(dc, area.left, area.top, area.right, area.bottom, kPanel);
        viewRect = area;
        Text(dc, area.left + st.S(8), area.top + st.S(4), L"Face map: detected landmarks and regions", kText, st.small);
        if (!rig.Valid() || !rig.Fitted()) {
            Text(dc, (area.left + area.right) / 2, (area.top + area.bottom) / 2,
                 rig.Valid() ? L"No face found on this mesh" : L"No target mesh", kMuted, st.small, TA_CENTER);
            return;
        }
        float x0, y0, x1, y1;
        rig.BoxXY(x0, y0, x1, y1);
        const int aw = area.right - area.left - st.S(16), ah = area.bottom - area.top - st.S(60);
        const float sc = std::min(aw / (x1 - x0), ah / (y1 - y0));
        const float ox = (area.left + area.right) * 0.5f - (x0 + x1) * 0.5f * sc;
        const float oy = (area.top + area.bottom) * 0.5f - st.S(6) + (y0 + y1) * 0.5f * sc;
        auto X = [&](float px) { return (int)(ox + px * sc); };
        auto Y = [&](float py) { return (int)(oy - py * sc); };
        const std::vector<Vertex>& base = rig.Base();
        if (dotsDirty) {
            rig.InfluenceMap(sel, influence);
            dotsDirty = false;
        }
        const size_t stride = std::max<size_t>(1, base.size() / 16000);
        for (size_t i = 0; i < base.size(); i += stride) {
            if (!rig.FrontFacing(base[i])) continue;
            const float inf = std::min(1.0f, influence[i] * 14.0f);
            const COLORREF c = RGB(70 + (int)(185 * inf), 70 + (int)(110 * inf), 70 - (int)(40 * inf));
            const int px = X(base[i].px), py = Y(base[i].py);
            SetPixelV(dc, px, py, c);
            SetPixelV(dc, px + 1, py, c);
        }
        const float* lm = rig.Landmarks();
        int cnt[(int)FaceRegion::Count] = {};
        float cx[(int)FaceRegion::Count] = {}, cy[(int)FaceRegion::Count] = {};
        for (int i = 0; i < kFaceLandmarks; ++i) {
            const FaceRegion r = FaceRig::RegionOf(i);
            const int px = X(lm[i * 3]), py = Y(lm[i * 3 + 1]);
            const COLORREF c = RegionColor(r);
            Fill(dc, px - 1, py - 1, px + 2, py + 2, c);
            cnt[(int)r]++;
            cx[(int)r] += lm[i * 3];
            cy[(int)r] += lm[i * 3 + 1];
        }
        // legend, and a label at the centre of each region
        int lx = area.left + st.S(8);
        const int ly = area.bottom - st.S(36);
        for (int r = 1; r < (int)FaceRegion::Count; ++r) {
            Fill(dc, lx, ly + st.S(4), lx + st.S(8), ly + st.S(12), RegionColor((FaceRegion)r));
            const std::wstring n = Wide(FaceRegionName((FaceRegion)r));
            Text(dc, lx + st.S(12), ly, n, kMuted, st.small);
            SIZE sz;
            GetTextExtentPoint32W(dc, n.c_str(), (int)n.size(), &sz);
            lx += st.S(12) + sz.cx + st.S(10);
            if (lx > area.right - st.S(70) && r < (int)FaceRegion::Count - 1) { lx = area.left + st.S(8); }
        }
        Text(dc, area.left + st.S(8), area.bottom - st.S(18),
             (L"Highlighted: " + Wide(kBlendshapeNames[sel]) + L"  (click a channel in the list)").c_str(), kMuted, st.small);
    }

    void Render(HDC dc, int W, int H) {
        hits.clear();
        const int m = st.S(10), side = st.S(340), lh = st.S(18);
        Fill(dc, 0, 0, W, H, kBg);
        SetBkMode(dc, TRANSPARENT);
        const int mainW = W - side - 3 * m;
        const int half = mainW / 2;
        const bool picture = have && (w > 0 || player.Loaded()); // an iPhone sends weights and no picture, so there is no mesh to draw on one
        DrawVideo(dc, RECT{m, m, m + half, H - m}, have ? image.data() : nullptr, w, h, picture ? &frame : nullptr, true, st);
        if (view3D) { // the engine's live 3D view sits on top of this panel
            const RECT mid = MidRect(W, H);
            Fill(dc, mid.left, mid.top, mid.right, mid.bottom, kPanel);
            viewRect = mid;
        } else {
            DrawRigView(dc, MidRect(W, H));
        }

        const int x0 = W - side - m + st.S(6), sw = side - st.S(12);
        int y = m;
        wchar_t b[200];
        Text(dc, x0, y, L"Face Mocap Live", kText, st.font);
        y += lh;
        swprintf_s(b, L"%s   %.0f fps   %s", host.live->SourceName().c_str(), host.live->TrackingFps(),
                   have && frame.valid ? L"face tracked" : L"no face");
        Text(dc, x0, y, b, kMuted, st.small);
        y += lh;
        if (host.live->IsPhone() && !player.Loaded()) {
            RECT wrap = {x0, y, x0 + sw, y + 3 * lh};
            HGDIOBJ old = SelectObject(dc, st.small);
            SetTextColor(dc, host.live->TrackingFps() > 0 ? kMuted : kAccent);
            DrawTextW(dc, host.live->PhoneStatus().c_str(), -1, &wrap, DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, old);
            y += 3 * lh;
        }
        if (host.live->Calibrated()) {
            Text(dc, x0, y, L"Neutral pose set", kMuted, st.small);
        } else {
            const char* hint = host.live->CalibHint();
            swprintf_s(b, L"Calibrating %.0f%%: %hs", host.live->CalibProgress() * 100.0f,
                       hint ? hint : "look at the screen, relaxed...");
            Text(dc, x0, y, b, kAccent, st.small);
        }
        y += lh + st.S(4);
        const int bh = st.S(22), gap = st.S(6), bw3 = (sw - 2 * gap) / 3;
        Button(dc, {x0, y, x0 + bw3, y + bh}, host.live->Recording() ? L"Stop rec" : L"Record", A_Record, host.live->Recording());
        Button(dc, {x0 + bw3 + gap, y, x0 + 2 * bw3 + gap, y + bh}, L"Recalibrate", A_Calibrate);
        Button(dc, {x0 + 2 * (bw3 + gap), y, x0 + sw, y + bh}, mirror ? L"Mirror on" : L"Mirror off", A_Mirror, mirror);
        y += bh + st.S(6);
        Button(dc, {x0, y, x0 + sw, y + bh}, host.live->IsPhone() ? L"Source: iPhone (click for webcam)" : L"Source: webcam (click for iPhone)", A_Source, host.live->IsPhone());
        y += bh + st.S(6);
        Slider(dc, x0, y, sw, L"Lip sync strength", &lipStrength, 0.0f, 1.0f);
        y += lh + st.S(6);

        Text(dc, x0, y, L"Target mesh", kText, st.small);
        y += lh;
        Text(dc, x0, y, target >= 0 ? Wide(targetName.c_str()).c_str() : L"(none)", kMuted, st.small);
        y += lh;
        const int bw2 = (sw - 2 * gap) / 3;
        Button(dc, {x0, y, x0 + bw2, y + bh}, L"Use selected", A_UseSelected);
        Button(dc, {x0 + bw2 + gap, y, x0 + 2 * bw2 + gap, y + bh}, L"Release", A_Release);
        Button(dc, {x0 + 2 * (bw2 + gap), y, x0 + sw, y + bh}, headDrive ? L"Head: on" : L"Head: off", A_Head, headDrive);
        y += bh + st.S(6);
        Button(dc, {x0, y, x0 + sw, y + bh}, player.Loaded() ? L"Stop clip (back to webcam)" : L"Play clip on this mesh...", A_PlayClip, player.Loaded());
        y += bh + st.S(10);

        Text(dc, x0, y, L"Mapping", kText, st.small);
        y += lh;
        const int hw2 = (sw - gap) / 2;
        Button(dc, {x0, y, x0 + hw2, y + bh}, L"Auto", A_Auto, !Manual());
        Button(dc, {x0 + hw2 + gap, y, x0 + sw, y + bh}, L"Manual", A_Manual, Manual());
        y += bh + st.S(6);
        Button(dc, {x0, y, x0 + sw, y + bh}, view3D ? L"Showing: live character (click for labelled map)" : L"Showing: labelled face map (click for live character)", A_View);
        y += bh + st.S(6);
        Button(dc, {x0, y, x0 + sw, y + bh}, maxGraphics ? L"Graphics: MAX (click for editor view)" : L"Graphics: editor view (click for MAX)", A_MaxGraphics, maxGraphics);
        y += bh + st.S(6);
        if (!Manual()) {
            Slider(dc, x0, y, sw, L"Strength", &strength, 0.0f, 2.0f);
            y += lh;
        } else {
            Button(dc, {x0, y, x0 + bw2, y + bh}, L"Save map", A_Save);
            Button(dc, {x0 + bw2 + gap, y, x0 + 2 * bw2 + gap, y + bh}, L"Load map", A_Load);
            Button(dc, {x0 + 2 * (bw2 + gap), y, x0 + sw, y + bh}, L"Reset", A_ResetAuto);
            y += bh + st.S(6);
        }
        y += st.S(4);

        // the 52 channels, scrolled with the mouse wheel; the selected one gets a gain slider in Manual mode
        const int footer = lh * 2 + m + (Manual() ? lh * 2 : 0);
        const int rows = std::max(3, (H - y - footer) / lh);
        listTop = std::clamp(listTop, 0, std::max(0, kBlendshapes - rows));
        for (int c = listTop; c < std::min(kBlendshapes, listTop + rows); ++c) {
            const ChannelTune& ct = rig.Mapping().ch[c];
            const RECT row = {x0 - st.S(4), y - 1, x0 + sw + st.S(4), y + lh - 2};
            if (c == sel) Fill(dc, row.left, row.top, row.right, row.bottom, RGB(52, 46, 40));
            const RECT box = {x0, y + st.S(3), x0 + st.S(11), y + st.S(14)};
            Fill(dc, box.left, box.top, box.right, box.bottom, kPanel);
            if (ct.enabled) Fill(dc, box.left + 2, box.top + 2, box.right - 2, box.bottom - 2, kMesh);
            Text(dc, x0 + st.S(18), y, Wide(kBlendshapeNames[c]), c == sel ? kText : kMuted, st.small);
            const float wv = have && frame.valid ? frame.bs[c] : 0.0f;
            const int bx = x0 + st.S(150), bw = sw - st.S(150);
            Fill(dc, bx, y + st.S(5), bx + bw, y + st.S(12), kPanel);
            Fill(dc, bx, y + st.S(5), bx + (int)(bw * std::clamp(wv, 0.0f, 1.0f)), y + st.S(12), kAccent);
            hits.push_back({box, A_Toggle + c});
            hits.push_back({row, A_Select + c});
            y += lh;
        }
        y += st.S(4);
        if (Manual()) {
            Slider(dc, x0, y, sw, (L"Gain: " + Wide(kBlendshapeNames[sel])).c_str(), &rig.Mapping().ch[sel].gain, 0.0f, 3.0f);
            y += lh;
        }
        RECT sr = {x0, H - m - 2 * lh, x0 + sw, H - m};
        SetTextColor(dc, kMuted);
        SelectObject(dc, st.small);
        DrawTextA(dc, status.c_str(), -1, &sr, DT_LEFT | DT_WORDBREAK);
    }

    // ---- input
    void SetSlider(const Hit& h, int mx) {
        const float t = std::clamp((float)(mx - h.r.left) / std::max(1, (int)(h.r.right - h.r.left)), 0.0f, 1.0f);
        *h.value = h.lo + t * (h.hi - h.lo);
        dirty = true;
    }

    void SaveRecording(HWND hwnd) {
        FaceClip clip;
        const std::wstring videos = host.stopVideoLog ? host.stopVideoLog() : L"";
        std::vector<int16_t> pcm = mic.Stop();
        if (!host.live->StopRecording(clip)) { status = "Nothing was recorded."; return; }
        wchar_t buf[MAX_PATH] = L"webcam_capture.tbdface";
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Face clip (*.tbdface)\0*.tbdface\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"tbdface";
        ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) {
            status = clip.Save(buf) ? "Clip saved." : "Could not write the clip.";
            if (!pcm.empty() && status == "Clip saved.") {
                StartLipJob(buf, std::move(pcm), clip.frames.back().time);
                status = "Clip saved. Transcribing on the GPU...";
            }
        }
        if (!videos.empty()) status += " Videos in bin\\facelog.";
    }

    void MapFile(HWND hwnd, bool save) {
        wchar_t buf[MAX_PATH] = L"face.tbdfacemap";
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Face map (*.tbdfacemap)\0*.tbdfacemap\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"tbdfacemap";
        ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        if (save) {
            if (GetSaveFileNameW(&ofn)) status = rig.Mapping().Save(buf) ? "Map saved." : "Could not write the map.";
        } else if (GetOpenFileNameW(&ofn)) {
            FaceMapping mp;
            if (mp.Load(buf)) { rig.Mapping() = mp; dirty = true; status = "Map loaded."; }
            else status = "Not a face map file.";
        }
    }

    void Click(HWND hwnd, int mx, int my) {
        dragHit = -1;
        for (int i = (int)hits.size() - 1; i >= 0; --i) {
            const Hit& h = hits[i];
            if (mx < h.r.left || mx >= h.r.right || my < h.r.top || my >= h.r.bottom) continue;
            const int a = h.action;
            if (a == A_Slider) { dragHit = i; SetSlider(h, mx); return; }
            if (a >= A_Toggle) { rig.Mapping().ch[a - A_Toggle].enabled ^= true; dirty = true; return; }
            if (a >= A_Select) { sel = a - A_Select; dotsDirty = true; return; }
            switch (a) {
            case A_Record:
                if (host.live->Recording()) SaveRecording(hwnd);
                else {
                    const bool heard = mic.Start();
                    host.live->StartRecording();
                    if (!heard) status = "Recording the clip, but no microphone could be opened: no lip sync.";
                    if (host.startVideoLog && !host.startVideoLog()) status = "Recording the clip, but the video log could not start.";
                }
                break;
            case A_Source: SwitchSource(); break;
            case A_Calibrate: host.live->Recalibrate(); retarget.Reset(); status = "Recalibrating: look at the camera with a relaxed face."; break;
            case A_Mirror: mirror ^= true; host.setMirror(mirror); dirty = true; break;
            case A_PlayClip: PlayClip(hwnd); break;
            case A_UseSelected: UseSelected(); break;
            case A_Release: Release(); status = "Released."; break;
            case A_Head:
                headDrive ^= true;
                if (target >= 0 && !rig.Fitted()) host.driveHead(target, headDrive);
                dirty = true;
                break;
            case A_Auto:
                if (Manual()) { rig.Mapping() = FaceMapping(); dirty = true; }
                SetView3D(true);
                break;
            case A_Manual:
                if (!Manual()) {
                    rig.Mapping().mode = MapMode::Manual;
                    status = "Manual: scroll the list, pick a channel, set its gain, or switch a channel off.";
                    dirty = true;
                }
                break;
            case A_View: SetView3D(!view3D); break;
            case A_MaxGraphics:
                maxGraphics ^= true;
                if (host.setMaxGraphics) host.setMaxGraphics(maxGraphics);
                if (maxGraphics) SetView3D(true);
                break;
            case A_ResetAuto: {
                FaceMapping d;
                d.mode = MapMode::Manual;
                rig.Mapping() = d;
                dirty = true;
                break;
            }
            case A_Save: MapFile(hwnd, true); break;
            case A_Load: MapFile(hwnd, false); break;
            }
            return;
        }
    }
};

Tab* g_tab = nullptr;

LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Tab* t = g_tab;
    switch (msg) {
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (t) { SetCapture(hwnd); t->Click(hwnd, (short)LOWORD(lp), (short)HIWORD(lp)); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_MOUSEMOVE:
        if (t && (wp & MK_LBUTTON) && t->dragHit >= 0 && t->dragHit < (int)t->hits.size())
            t->SetSlider(t->hits[t->dragHit], (short)LOWORD(lp));
        return 0;
    case WM_MOUSEWHEEL:
        if (t) { // scrolls the channel list
            t->listTop -= GET_WHEEL_DELTA_WPARAM(wp) / 120 * 3;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (t) t->dragHit = -1;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (t && rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            t->Render(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Strip geometry (device pixels)
RECT SceneTabRect(const Tab& t) { return {t.st.S(8), t.st.S(5), t.st.S(8 + 100), t.st.S(34)}; }
RECT FaceTabRect(const Tab& t) { return {t.st.S(112), t.st.S(5), t.st.S(112 + 200), t.st.S(34)}; }

LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Tab* t = g_tab;
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (!t) break;
        const POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        const RECT f = FaceTabRect(*t), s = SceneTabRect(*t);
        const RECT x = {f.right - t->st.S(26), f.top, f.right - t->st.S(4), f.bottom};
        if (PtInRect(&x, p)) FaceTab_Close();
        else if (PtInRect(&f, p)) { t->pageActive = true; t->host.relayout(); }
        else if (PtInRect(&s, p)) { t->pageActive = false; t->host.relayout(); }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (t) {
            Fill(dc, 0, 0, rc.right, rc.bottom, RGB(16, 13, 10));
            auto draw = [&](RECT r, const wchar_t* label, bool active, bool close) {
                HBRUSH br = CreateSolidBrush(active ? kBg : RGB(36, 31, 26));
                HGDIOBJ ob = SelectObject(dc, br);
                HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
                RoundRect(dc, r.left, r.top, r.right, r.bottom + t->st.S(12), t->st.S(12), t->st.S(12));
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(br);
                SetBkMode(dc, TRANSPARENT);
                Text(dc, r.left + t->st.S(14), r.top + t->st.S(7), label, active ? kText : kMuted, t->st.small);
                if (close) Text(dc, r.right - t->st.S(15), r.top + t->st.S(7), L"x", kMuted, t->st.small, TA_CENTER);
            };
            draw(SceneTabRect(*t), L"Scene", !t->pageActive, false);
            draw(FaceTabRect(*t), L"Face Mocap Live", t->pageActive, true);
            Fill(dc, 0, rc.bottom - 1, rc.right, rc.bottom, t->pageActive ? kBg : RGB(36, 31, 26));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Register(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = PageProc;
    wc.lpszClassName = kPageClass;
    RegisterClassW(&wc);
    wc.lpfnWndProc = StripProc;
    wc.lpszClassName = kStripClass;
    RegisterClassW(&wc);
}

} // namespace

void FaceTab_Open(const FaceTabHost& host) {
    if (g_tab) {
        g_tab->pageActive = true;
        g_tab->host.relayout();
        InvalidateRect(g_tab->strip, nullptr, FALSE);
        return;
    }
    Register(host.instance);
    g_tab = new Tab;
    g_tab->host = host;
    g_tab->st.dpi = (int)GetDpiForWindow(host.parent);
    CreateFonts(g_tab->st);
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    g_tab->strip = CreateWindowExW(0, kStripClass, L"", style, 0, 0, 10, 10, host.parent, nullptr, host.instance, nullptr);
    g_tab->page = CreateWindowExW(0, kPageClass, L"", style, 0, 0, 10, 10, host.parent, nullptr, host.instance, nullptr);
    SetTimer(g_tab->page, kTimer, 33, nullptr);
    if (host.setMirror) host.setMirror(g_tab->mirror);
    if (host.maxGraphicsOn) g_tab->maxGraphics = host.maxGraphicsOn();
    if (host.selectedIndex() >= 0) g_tab->UseSelected(); // start on the selected head right away
    host.relayout();
}

bool FaceTab_IsOpen() { return g_tab != nullptr; }
bool FaceTab_PageActive() { return g_tab && g_tab->pageActive; }
int FaceTab_StripHeight(int dpi) { return g_tab ? MulDiv(36, dpi, 96) : 0; }

// Where the live 3D view goes while the page shows it: the middle panel of the page, in parent client coordinates.
bool FaceTab_ViewportRect(const RECT& page, RECT* out) {
    if (!g_tab || !g_tab->pageActive || !g_tab->view3D) return false;
    const RECT mid = g_tab->MidRect(page.right - page.left, page.bottom - page.top);
    *out = {page.left + mid.left, page.top + mid.top, page.left + mid.right, page.top + mid.bottom};
    return true;
}

void FaceTab_Layout(const RECT& strip, const RECT& page, HWND viewportWindow) {
    if (!g_tab) return;
    MoveWindow(g_tab->strip, strip.left, strip.top, strip.right - strip.left, strip.bottom - strip.top, TRUE);
    MoveWindow(g_tab->page, page.left, page.top, page.right - page.left, page.bottom - page.top, TRUE);
    ShowWindow(g_tab->page, g_tab->pageActive ? SW_SHOWNA : SW_HIDE);
    if (viewportWindow) {
        ShowWindow(viewportWindow, g_tab->pageActive && !g_tab->view3D ? SW_HIDE : SW_SHOWNA);
        SetWindowPos(viewportWindow, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // above the page
    }
}

void FaceTab_Tick() {
    if (g_tab) g_tab->Tick();
}

void FaceTab_Close() {
    if (!g_tab) return;
    Tab* t = g_tab;
    g_tab = nullptr; // procs see "no tab" from here on
    t->StopClip();
    t->Release();
    if (t->lipJob.joinable()) t->lipJob.join();
    t->mic.Stop();
    if (t->host.stopVideoLog) t->host.stopVideoLog();
    if (t->host.live && t->host.live->Recording()) {
        FaceClip discard;
        t->host.live->StopRecording(discard);
    }
    t->host.stopCapture();
    KillTimer(t->page, kTimer);
    DestroyWindow(t->page);
    DestroyWindow(t->strip);
    DestroyFonts(t->st);
    const auto relayout = t->host.relayout;
    delete t;
    relayout();
}

} // namespace tbd
