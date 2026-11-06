// The road building tool, modelled on Cities: Skylines II's net tool:
//   Tool modes  Straight (2 clicks, chains), Simple curve (3 clicks), Complex curve (4 clicks), Continuous (tangent
//               chained arcs), Grid (3 clicks), Replace (upgrade / flip one-way), Swap (press on a road, drag the
//               way traffic should go, release: it becomes the picked road, pointing that way), plus Bulldoze.
//   Elevation   -60 .. +60 m in steps of 10 / 5 / 2.5 / 1.25 m (Page Up / Page Down); over 3 m above the ground it is
//               a bridge (pillars, parapets), under 4 m below it a tunnel (portals where it dives in).
//   Parallel    a second road offset to the right of each one placed.
//   Snapping    all on / off, cell length (8 m), existing geometry (nodes and road surfaces), straight directions
//               (15 degree steps from the connected road or the world axes), net side (zone-cell distances from a
//               road), guidelines (extensions and perpendiculars of nearby roads), zone grid (8 m), contour lines.
//   Assets      every road profile, prefab intersections (T, four-way, Y) and roundabouts (small / medium / large).
// Crossing an existing road at about the same height makes an intersection; the network splits both roads there.
// Platform free: the host feeds the cursor's ground point and clicks, and draws the preview this returns.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "road_network.h"

namespace tbd {
namespace city {

enum class ToolMode : int { Straight, SimpleCurve, ComplexCurve, Continuous, Grid, Replace, Swap, Bulldoze };
enum SnapBit : uint32_t {
    SnapLength = 1u << 0,
    SnapGeometry = 1u << 1,
    SnapStraight = 1u << 2,
    SnapNetSide = 1u << 3,
    SnapGuide = 1u << 4,
    SnapZone = 1u << 5,
    SnapContour = 1u << 6,
};
enum class Tab : int { Small, Medium, Large, Highway, Quay, Path, Intersection, Roundabout, Count };
enum class AssetKind { Road, Intersection, Roundabout };
struct Asset {
    AssetKind kind;
    Tab tab;
    int profile;
    std::string name;
    std::vector<float> armDeg; // intersections: arm directions
    float radius = 0;          // roundabouts
};

inline const std::vector<Asset>& Assets() {
    static const std::vector<Asset> list = [] {
        std::vector<Asset> v;
        const auto& P = Profiles();
        for (size_t i = 0; i < P.size(); ++i) v.push_back({AssetKind::Road, (Tab)(int)P[i].cat, (int)i, P[i].name, {}, 0});
        auto X = [&](const char* name, const char* road, std::vector<float> arms) { v.push_back({AssetKind::Intersection, Tab::Intersection, ProfileIndex(road), name, std::move(arms), 0}); };
        X("Small T-Intersection", "Two-Lane Road", {0, 90, 180});
        X("Small Four-Way Intersection", "Two-Lane Road", {0, 90, 180, 270});
        X("Y-Intersection", "Two-Lane Road", {90, 210, 330});
        X("Medium T-Intersection", "Four-Lane Road", {0, 90, 180});
        X("Medium Four-Way Intersection", "Four-Lane Divided Road", {0, 90, 180, 270});
        X("Large Four-Way Intersection", "Six-Lane Divided Road", {0, 90, 180, 270});
        X("Five-Way Intersection", "Two-Lane Road", {0, 72, 144, 216, 288});
        auto R = [&](const char* name, const char* road, float radius) { v.push_back({AssetKind::Roundabout, Tab::Roundabout, ProfileIndex(road), name, {}, radius}); };
        R("Small Roundabout", "One-Lane One-Way Road", 16);
        R("Medium Roundabout", "Two-Lane One-Way Road", 24);
        R("Large Roundabout", "Three-Lane One-Way Road", 34);
        R("Highway Roundabout", "Three-Lane One-Way Highway", 60);
        return v;
    }();
    return list;
}

inline const char* TabName(Tab t) {
    static const char* const k[] = {"Small Roads", "Medium Roads", "Large Roads", "Highways", "Quays", "Paths", "Intersections", "Roundabouts"};
    return k[(int)t];
}
inline const char* ModeName(ToolMode m) {
    static const char* const k[] = {"Straight", "Simple Curve", "Complex Curve", "Continuous", "Grid", "Replace", "Swap", "Bulldoze"};
    return k[(int)m];
}

struct Snap {
    Vec3 p;
    int node = -1, seg = -1;
    float t = 0;
    const char* kind = "";
};
struct PreviewCurve {
    Cubic c;
    float ya = 0, yb = 0;
    int profile = 0;
};
struct Segment3 {
    Vec3 a, b;
};
struct WorldLabel {
    Vec3 p;
    char text[48];
};

class RoadTool {
public:
    Network net;
    bool active = false;
    ToolMode mode = ToolMode::Straight;
    int asset = 0;          // index into Assets()
    Tab tab = Tab::Small;   // the picker's open tab
    int lastRoad[(int)Tab::Count] = {}; // remembered pick per tab
    float elevation = 0;
    int stepIdx = 1;
    static constexpr float kSteps[4] = {10.0f, 5.0f, 2.5f, 1.25f};
    bool parallel = false;
    float parallelOffset = 0; // 0: auto (profile width + 8 m)
    bool snapAll = true;
    uint32_t snaps = SnapLength | SnapGeometry | SnapStraight | SnapNetSide | SnapGuide;
    float gridSpacing = 96.0f; // 12 zone cells
    float rotationDeg = 0;     // intersections / roundabouts
    GroundFn ground;
    std::function<void(float x, float z, float hardRadius, float radius, float targetY)> flatten; // levels the terrain under ground roads

    // ---- state the host / GUI reads
    std::vector<Snap> pts;
    Snap cursor;
    bool haveCursor = false;
    int hoverSeg = -1;
    std::vector<Segment3> guides; // guidelines the cursor snapped to (and the start's direction)
    std::string error;
    std::string status;

    RoadTool() {
        for (int t = 0; t < (int)Tab::Count; ++t) lastRoad[t] = FirstInTab((Tab)t);
        asset = lastRoad[0];
    }

    static int FirstInTab(Tab t) {
        const auto& a = Assets();
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].tab == t) return (int)i;
        return 0;
    }
    const Asset& Current() const { return Assets()[asset]; }
    int ProfileId() const { return Current().profile; }
    const Profile& Prof() const { return Profiles()[ProfileId()]; }
    bool On(uint32_t bit) const { return snapAll && (snaps & bit); }
    float Step() const { return kSteps[stepIdx & 3]; }
    float ParallelOffset() const { return parallelOffset > 0 ? parallelOffset : Prof().width + 8.0f; }
    bool Pending() const { return !pts.empty(); }

    void Open() { active = true; }
    void Close() { active = false, Cancel(); }
    void Select(int a) {
        asset = a;
        tab = Assets()[a].tab;
        lastRoad[(int)tab] = a;
        if (Assets()[a].kind != AssetKind::Road) pts.clear();
        if (Assets()[a].kind == AssetKind::Road && (mode == ToolMode::Bulldoze)) mode = ToolMode::Straight;
    }
    void SelectTab(Tab t) { Select(lastRoad[(int)t]); }
    void SetMode(ToolMode m) {
        if (m != mode) Cancel();
        mode = m;
    }
    void Cancel() {
        swapSeg_ = -1;
        pts.clear();
        contHas_ = false;
        error.clear();
    }
    void Raise(int dir) { elevation = std::clamp(std::round((elevation + dir * Step()) / Step()) * Step(), -60.0f, 60.0f); }
    void CycleStep() { stepIdx = (stepIdx + 1) & 3; }
    void Rotate(float deg) { rotationDeg = std::fmod(rotationDeg + deg + 360.0f, 360.0f); }

    // ---- undo
    void PushUndo() {
        undo_.push_back(net);
        if (undo_.size() > 64) undo_.erase(undo_.begin());
        redo_.clear();
    }
    bool Undo() {
        if (undo_.empty()) return false;
        redo_.push_back(net);
        net = undo_.back();
        undo_.pop_back();
        net.version = ++versionSeed_ * 1000003ull;
        Cancel();
        return true;
    }
    bool Redo() {
        if (redo_.empty()) return false;
        undo_.push_back(net);
        net = redo_.back();
        redo_.pop_back();
        net.version = ++versionSeed_ * 1000003ull;
        return true;
    }
    bool CanUndo() const { return !undo_.empty(); }

    // ---- scripted building (demo towns, tests): no snapping, crossings still make intersections
    static int AssetIndex(const char* name) {
        const auto& a = Assets();
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].name == name) return (int)i;
        return 0;
    }
    void BuildRoad(const Cubic& c, float ya, float yb, int profile) { CommitCurve({c, ya, yb, profile}, nullptr, nullptr); }
    void BuildAsset(int assetIndex, Vec3 centre) {
        const int keepAsset = asset;
        const Snap keepCursor = cursor;
        const bool keepHave = haveCursor;
        asset = assetIndex;
        cursor = {centre};
        haveCursor = true;
        std::vector<PreviewCurve> v;
        PendingCurves(v);
        if (!v.empty()) {
            if (Current().kind == AssetKind::Roundabout) CommitRoundabout(v, Current().radius);
            else CommitIntersection(v);
        }
        asset = keepAsset, cursor = keepCursor, haveCursor = keepHave;
    }

    float Ground(float x, float z) const { return ground ? ground(x, z) : 0.0f; }

    // ------------------------------------------------------------------------------------------------ cursor
    // The cursor's ground hit (or none). Works out the snapped point, the hovered road and the guidelines.
    void Move(bool onGround, Vec3 hit) {
        haveCursor = onGround;
        guides.clear();
        hoverSeg = -1;
        if (!onGround) return;
        const Asset& as = Current();
        if (mode == ToolMode::Swap && swapSeg_ >= 0) { // dragging: keep the pressed road, show the drag direction
            hoverSeg = net.segs[swapSeg_].alive ? swapSeg_ : -1;
            cursor = {hit};
            const Vec3 a{swapStart_.x, swapStart_.y + 0.8f, swapStart_.z}, b{hit.x, swapStart_.y + 0.8f, hit.z};
            guides.push_back({a, b});
            const V2 d = XZ(b) - XZ(a);
            const float l = Len(d);
            if (l > 0.5f) { // arrowhead
                const V2 u = d * (1.0f / l), s = {-u.z, u.x}, h = XZ(b) - u * 2.5f;
                guides.push_back({b, {h.x + s.x * 1.5f, b.y, h.z + s.z * 1.5f}});
                guides.push_back({b, {h.x - s.x * 1.5f, b.y, h.z - s.z * 1.5f}});
            }
            return;
        }
        if (mode == ToolMode::Replace || mode == ToolMode::Swap || mode == ToolMode::Bulldoze) {
            float t, d;
            hoverSeg = net.SegmentAt(XZ(hit), 1.0f, t, d);
            cursor = {hit};
            return;
        }
        const bool control = as.kind == AssetKind::Road && IsControlPoint((int)pts.size());
        cursor = SnapAt(hit, !control);
    }

    // A left click. Returns true when the network changed.
    bool Click() {
        error.clear();
        if (!haveCursor) return false;
        const Asset& as = Current();
        if (mode == ToolMode::Bulldoze) {
            if (hoverSeg < 0) return false;
            PushUndo();
            net.RemoveSeg(hoverSeg);
            hoverSeg = -1;
            return true;
        }
        if (mode == ToolMode::Swap) { // press: remember the road and where the drag starts; Release() applies it
            if (hoverSeg < 0 || as.kind != AssetKind::Road) return false;
            swapSeg_ = hoverSeg;
            swapStart_ = cursor.p;
            return false;
        }
        if (mode == ToolMode::Replace) {
            if (hoverSeg < 0 || as.kind != AssetKind::Road) return false;
            PushUndo();
            Segment& s = net.segs[hoverSeg];
            if (s.profile == as.profile) { // same road: flip its direction (one-way / asymmetric roads)
                std::swap(s.a, s.b);
                std::swap(s.c1, s.c2);
            } else s.profile = as.profile;
            ++net.version;
            return true;
        }
        if (as.kind != AssetKind::Road) { // prefab: one click
            std::vector<PreviewCurve> curves;
            if (!PendingCurves(curves)) return false;
            PushUndo();
            if (as.kind == AssetKind::Roundabout) CommitRoundabout(curves, as.radius);
            else CommitIntersection(curves);
            return true;
        }
        const int need = PointsNeeded();
        const bool continuousNext = mode == ToolMode::Continuous && contHas_ && pts.size() == 1;
        if ((int)pts.size() + 1 < need && !continuousNext) {
            if (!pts.empty() && Len(XZ(cursor.p) - XZ(pts.back().p)) < 1.0f) return false; // a double click on the same spot
            pts.push_back(cursor);
            return false;
        }
        std::vector<PreviewCurve> curves;
        if (!PendingCurves(curves)) return false;
        PushUndo();
        Cubic last = curves.front().c;
        for (size_t i = 0; i < curves.size(); ++i) {
            const bool main = i == 0 || mode == ToolMode::Grid;
            CommitCurve(curves[i], main && mode != ToolMode::Grid ? &pts.front() : nullptr, main && mode != ToolMode::Grid ? &cursor : nullptr);
        }
        // chain from the end, like CS2 (the grid starts over)
        if (mode == ToolMode::Grid) pts.clear();
        else {
            Snap end;
            end.p = {last.p3.x, curves.front().yb, last.p3.z};
            end.node = net.NearestNode(last.p3, 0.75f);
            end.kind = "node";
            contDir_ = last.Tan(1);
            contHas_ = mode == ToolMode::Continuous;
            pts.clear();
            pts.push_back(end);
        }
        return true;
    }

    // Left button up. Swap mode: the pressed road takes the picked profile, its forward lanes pointing the way the
    // mouse moved (a drag under 1 m keeps its direction). True when the network changed.
    bool Release() {
        if (mode != ToolMode::Swap || swapSeg_ < 0) return false;
        const int si = swapSeg_;
        swapSeg_ = -1;
        if (!net.segs[si].alive || Current().kind != AssetKind::Road) return false;
        const V2 drag = XZ(cursor.p) - XZ(swapStart_);
        bool flip = false;
        if (Len(drag) >= 1.0f) {
            float t, d;
            if (net.SegmentAt(XZ(swapStart_), 4.0f, t, d) != si) t = 0.5f;
            const V2 tan = net.Curve(si).Tan(t);
            flip = tan.x * drag.x + tan.z * drag.z < 0;
        }
        if (!flip && net.segs[si].profile == Current().profile) return false;
        PushUndo();
        Segment& s = net.segs[si];
        if (flip) {
            std::swap(s.a, s.b);
            std::swap(s.c1, s.c2);
        }
        s.profile = Current().profile;
        ++net.version;
        return true;
    }
    bool Dragging() const { return swapSeg_ >= 0; }

    int PointsNeeded() const {
        switch (mode) {
        case ToolMode::SimpleCurve: return 3;
        case ToolMode::ComplexCurve: return 4;
        case ToolMode::Grid: return 3;
        default: return 2;
        }
    }
    bool IsControlPoint(int index) const {
        if (mode == ToolMode::SimpleCurve) return index == 1;
        if (mode == ToolMode::ComplexCurve) return index == 1 || index == 2;
        return false;
    }

    // ------------------------------------------------------------------------------------------------ preview
    // The curves a click would build now (main road first, then parallel copies / grid lines). False + error when invalid
    // (curves still filled so the ghost shows red).
    bool PendingCurves(std::vector<PreviewCurve>& out) {
        out.clear();
        error.clear();
        if (!haveCursor || mode == ToolMode::Replace || mode == ToolMode::Swap || mode == ToolMode::Bulldoze) return false;
        const Asset& as = Current();
        const int prof = as.profile;
        const V2 c = XZ(cursor.p);
        if (as.kind == AssetKind::Intersection) {
            const float y = cursor.p.y, arm = std::max(28.0f, Profiles()[prof].width * 2.2f);
            for (float deg : as.armDeg) {
                const float a = (deg + rotationDeg) * 0.01745329f;
                const V2 e = c + V2{std::cos(a), -std::sin(a)} * arm;
                out.push_back({Cubic::Line(c, e), y, Ground(e.x, e.z) + elevation, prof});
            }
            return Validate(out);
        }
        if (as.kind == AssetKind::Roundabout) {
            const float R = as.radius, k = 0.5523f * R, y = cursor.p.y;
            for (int i = 0; i < 4; ++i) { // counter-clockwise seen from above (north up): right-hand traffic
                const float a0 = (rotationDeg + 90.0f * i) * 0.01745329f, a1 = a0 + 1.5707963f;
                const V2 p0 = c + V2{std::cos(a0), -std::sin(a0)} * R, p3 = c + V2{std::cos(a1), -std::sin(a1)} * R;
                const V2 t0 = {-std::sin(a0), -std::cos(a0)}, t1 = {-std::sin(a1), -std::cos(a1)};
                const float y0 = std::fabs(elevation) < 0.01f ? Ground(p0.x, p0.z) : y, y3 = std::fabs(elevation) < 0.01f ? Ground(p3.x, p3.z) : y;
                out.push_back({{p0, p0 + t0 * k, p3 - t1 * k, p3}, y0, y3, prof}); // on the ground it follows the terrain
            }
            return true;
        }
        if (pts.empty()) return false;
        const Snap& a = pts.front();
        const V2 A = XZ(a.p);
        const float ya = a.p.y, yb = cursor.p.y;
        Cubic curve = Cubic::Line(A, c);
        switch (mode) {
        case ToolMode::SimpleCurve:
            if (pts.size() >= 2) curve = Cubic::Quad(A, XZ(pts[1].p), c);
            break;
        case ToolMode::ComplexCurve:
            if (pts.size() == 2) curve = Cubic::Quad(A, XZ(pts[1].p), c);
            else if (pts.size() >= 3) curve = {A, XZ(pts[1].p), XZ(pts[2].p), c};
            break;
        case ToolMode::Continuous:
            if (contHas_) {
                const V2 ab = c - A;
                const float L = Len(ab), cosPhi = Dot2(Norm(ab), contDir_);
                if (L > 0.1f && cosPhi < 0.9995f) {
                    if (cosPhi < 0.17f) {
                        error = "Curve too sharp";
                        curve = Cubic::Quad(A, A + contDir_ * (L * 0.5f), c);
                    } else curve = Cubic::Quad(A, A + contDir_ * (L / (2.0f * cosPhi)), c);
                }
            }
            break;
        case ToolMode::Grid:
            if (pts.size() >= 2) return GridCurves(out);
            break;
        default: break;
        }
        out.push_back({curve, ya, yb, prof});
        if (parallel) {
            const float d = ParallelOffset();
            const Cubic o = curve.Offset(d);
            out.push_back({o, Ground(o.p0.x, o.p0.z) + (ya - Ground(A.x, A.z)), Ground(o.p3.x, o.p3.z) + (yb - Ground(c.x, c.z)), prof});
        }
        const bool ok = Validate(out);
        return ok && error.empty();
    }

    // The labels CS2 shows while drawing: length, slope, angle to the connected road, elevation.
    void Labels(std::vector<WorldLabel>& out) {
        out.clear();
        std::vector<PreviewCurve> curves;
        PendingCurves(curves);
        if (curves.empty() || Current().kind != AssetKind::Road || mode == ToolMode::Grid) {
            if (mode == ToolMode::Grid && curves.size() > 1) {
                WorldLabel l;
                l.p = cursor.p;
                std::snprintf(l.text, sizeof(l.text), "%zu roads", curves.size());
                out.push_back(l);
            }
            return;
        }
        const PreviewCurve& pc = curves.front();
        const float len = pc.c.Length();
        WorldLabel l;
        const V2 mid = pc.c.At(0.5f);
        l.p = {mid.x, 0.5f * (pc.ya + pc.yb) + 2.0f, mid.z};
        const float slope = len > 0.1f ? std::fabs(pc.yb - pc.ya) / len * 100.0f : 0;
        if (slope >= 0.5f) std::snprintf(l.text, sizeof(l.text), "%.0f m   %.0f%%", len, slope);
        else std::snprintf(l.text, sizeof(l.text), "%.0f m", len);
        out.push_back(l);
        // angle to the road the start connects to
        if (!pts.empty() && pts.front().node >= 0 && net.nodes[pts.front().node].alive) {
            const int n = pts.front().node;
            const V2 d = pc.c.Tan(0);
            float best = 999;
            for (int s : net.nodes[n].segs) {
                const V2 u = net.OutDir(s, n);
                const float ang = std::acos(std::clamp(Dot2(u, d), -1.0f, 1.0f)) * 57.29578f;
                best = std::min(best, ang);
            }
            if (best < 999) {
                WorldLabel a;
                const V2 p = pc.c.At(0.0f) + d * 6.0f;
                a.p = {p.x, pc.ya + 2.0f, p.z};
                std::snprintf(a.text, sizeof(a.text), "%.0f deg", best);
                out.push_back(a);
            }
        }
        if (std::fabs(elevation) > 0.01f) {
            WorldLabel e;
            e.p = {cursor.p.x, cursor.p.y + 4.0f, cursor.p.z};
            std::snprintf(e.text, sizeof(e.text), "%+.1f m%s", elevation, elevation <= -mesh::kTunnelDepth - 0.5f ? "  tunnel" : elevation >= mesh::kElevated ? "  bridge" : "");
            out.push_back(e);
        }
    }

    // ------------------------------------------------------------------------------------------------ ghost geometry
    // Unlit triangles and lines (DrawTransient) for the pending road(s), the snap marker, handles and guidelines.
    void Ghost(std::vector<Vertex>& tris, std::vector<Vertex>& lines) {
        tris.clear(), lines.clear();
        if (!active) return;
        auto line = [&](Vec3 a, Vec3 b, Vec3 c) {
            lines.push_back({a.x, a.y, a.z, 0, 0, 0, c.x, c.y, c.z, 1});
            lines.push_back({b.x, b.y, b.z, 0, 0, 0, c.x, c.y, c.z, 1});
        };
        auto circle = [&](Vec3 p, float r, Vec3 c) {
            for (int i = 0; i < 24; ++i) {
                const float a0 = i * 0.2617994f, a1 = (i + 1) * 0.2617994f;
                line({p.x + std::cos(a0) * r, p.y, p.z + std::sin(a0) * r}, {p.x + std::cos(a1) * r, p.y, p.z + std::sin(a1) * r}, c);
            }
        };
        // tunnels stay visible while building, as dashed centre lines
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive) continue;
            const Cubic c = net.Curve((int)s);
            for (int i = 0; i < 24; i += 2) {
                const float t0 = i / 24.0f, t1 = (i + 1) / 24.0f;
                const V2 p0 = c.At(t0), p1 = c.At(t1);
                const float y0 = net.YAt(net.segs[s], t0), y1 = net.YAt(net.segs[s], t1);
                if (y0 < Ground(p0.x, p0.z) - mesh::kTunnelDepth) line({p0.x, y0, p0.z}, {p1.x, y1, p1.z}, {0.35f, 0.65f, 1.0f});
            }
        }
        if (hoverSeg >= 0 && net.segs[hoverSeg].alive) {
            const Segment& s = net.segs[hoverSeg];
            PreviewCurve pc{net.Curve(s), net.NodeY(s.a), net.NodeY(s.b), s.profile};
            const bool del = mode == ToolMode::Bulldoze;
            GhostCurve(pc, del ? Vec3{0.95f, 0.25f, 0.2f} : Vec3{0.2f, 0.55f, 1.0f}, tris, lines, 0.35f);
            if (!del) GhostCurve({pc.c, pc.ya, pc.yb, ProfileId()}, {0.45f, 0.8f, 1.0f}, tris, lines, 0.6f, true);
        }
        if (!haveCursor) return;
        std::vector<PreviewCurve> curves;
        const bool ok = PendingCurves(curves);
        const Vec3 good{0.18f, 0.5f, 1.0f}, bad{0.95f, 0.28f, 0.22f};
        for (const PreviewCurve& pc : curves) GhostCurve(pc, ok ? good : bad, tris, lines, 0.4f);
        for (const Segment3& g : guides) line(g.a, g.b, {1.0f, 0.95f, 0.6f});
        // handles and the control polygon
        const Vec3 handle{1, 1, 1};
        for (size_t i = 0; i < pts.size(); ++i) {
            circle({pts[i].p.x, pts[i].p.y + 0.5f, pts[i].p.z}, 1.6f, handle);
            if (i) line({pts[i - 1].p.x, pts[i - 1].p.y + 0.5f, pts[i - 1].p.z}, {pts[i].p.x, pts[i].p.y + 0.5f, pts[i].p.z}, {0.7f, 0.8f, 1.0f});
        }
        if (!pts.empty()) line({pts.back().p.x, pts.back().p.y + 0.5f, pts.back().p.z}, {cursor.p.x, cursor.p.y + 0.5f, cursor.p.z}, {0.7f, 0.8f, 1.0f});
        const bool snapped = cursor.kind && cursor.kind[0];
        circle({cursor.p.x, cursor.p.y + 0.5f, cursor.p.z}, snapped ? 2.2f : 1.4f, snapped ? Vec3{1.0f, 0.85f, 0.2f} : handle);
        if (cursor.p.y - Ground(cursor.p.x, cursor.p.z) > 0.5f || cursor.p.y < Ground(cursor.p.x, cursor.p.z) - 0.5f) // elevation stick
            line(cursor.p, {cursor.p.x, Ground(cursor.p.x, cursor.p.z), cursor.p.z}, {0.8f, 0.85f, 0.95f});
        if (On(SnapContour)) Contours(XZ(cursor.p), 140.0f, 5.0f, lines);
    }

private:
    std::vector<Network> undo_, redo_;
    uint64_t versionSeed_ = 1;
    V2 contDir_;
    bool contHas_ = false;
    int swapSeg_ = -1; // Swap: the road pressed on (until the button comes up)
    Vec3 swapStart_{};

    // A strip-by-strip ghost of one curve, lifted a little, with its outline.
    void GhostCurve(const PreviewCurve& pc, Vec3 tint, std::vector<Vertex>& tris, std::vector<Vertex>& lines, float lift, bool outlineOnly = false) const {
        const Profile& p = Profiles()[pc.profile];
        const float len = pc.c.Length();
        const int n = std::max(2, (int)std::ceil(len / 3.0f));
        std::vector<float> edge(p.strips.size() + 1);
        edge[0] = p.Half();
        for (size_t k = 0; k < p.strips.size(); ++k) edge[k + 1] = edge[k] - p.strips[k].w;
        auto P = [&](int i, float off) {
            const float t = (float)i / n;
            const V2 c = pc.c.At(t) + LeftOf(pc.c.Tan(t)) * off;
            return Vec3{c.x, FollowY(pc.c, pc.ya, pc.yb, t, ground) + lift, c.z};
        };
        for (int i = 1; i <= n && !outlineOnly; ++i)
            for (size_t k = 0; k < p.strips.size(); ++k) {
                const Kind kd = p.strips[k].kind;
                const float shade = kd == Kind::Lane || kd == Kind::Path || kd == Kind::Bike ? 0.75f : kd == Kind::Parking || kd == Kind::Shoulder ? 0.9f : 1.25f;
                const Vec3 c = {std::min(1.0f, tint.x * shade), std::min(1.0f, tint.y * shade), std::min(1.0f, tint.z * shade)};
                const Vec3 a = P(i - 1, edge[k]), b = P(i - 1, edge[k + 1]), d = P(i, edge[k]), e = P(i, edge[k + 1]);
                for (const Vec3& v : {a, b, e, a, e, d}) tris.push_back({v.x, v.y, v.z, 0, 0, 0, c.x, c.y, c.z, 1});
            }
        const Vec3 edgeCol{std::min(1.0f, tint.x + 0.5f), std::min(1.0f, tint.y + 0.4f), 1.0f};
        for (int i = 1; i <= n; ++i)
            for (float off : {edge.front(), edge.back()}) {
                const Vec3 a = P(i - 1, off), b = P(i, off);
                lines.push_back({a.x, a.y + 0.05f, a.z, 0, 0, 0, edgeCol.x, edgeCol.y, edgeCol.z, 1});
                lines.push_back({b.x, b.y + 0.05f, b.z, 0, 0, 0, edgeCol.x, edgeCol.y, edgeCol.z, 1});
            }
        if (p.OneWay()) // direction chevrons
            for (int i = 2; i < n; i += 4) {
                const float t = (float)i / n;
                const V2 c = pc.c.At(t), d = pc.c.Tan(t), l = LeftOf(d);
                const float y = pc.ya + (pc.yb - pc.ya) * t + lift + 0.1f, s = std::min(3.0f, p.Half() * 0.6f);
                const V2 tip = c + d * s, l0 = c + l * s, r0 = c - l * s;
                lines.push_back({l0.x, y, l0.z, 0, 0, 0, 1, 1, 1, 1});
                lines.push_back({tip.x, y, tip.z, 0, 0, 0, 1, 1, 1, 1});
                lines.push_back({tip.x, y, tip.z, 0, 0, 0, 1, 1, 1, 1});
                lines.push_back({r0.x, y, r0.z, 0, 0, 0, 1, 1, 1, 1});
            }
    }

    // Marching squares contour lines every `step` metres around a point.
    void Contours(V2 centre, float radius, float step, std::vector<Vertex>& lines) const {
        const float cell = 4.0f;
        const int n = (int)(radius * 2 / cell);
        const float x0 = std::floor((centre.x - radius) / cell) * cell, z0 = std::floor((centre.z - radius) / cell) * cell;
        std::vector<float> h((size_t)(n + 1) * (n + 1));
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) h[(size_t)j * (n + 1) + i] = Ground(x0 + i * cell, z0 + j * cell);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const float c[4] = {h[(size_t)j * (n + 1) + i], h[(size_t)j * (n + 1) + i + 1], h[(size_t)(j + 1) * (n + 1) + i + 1], h[(size_t)(j + 1) * (n + 1) + i]};
                const V2 p[4] = {{x0 + i * cell, z0 + j * cell}, {x0 + (i + 1) * cell, z0 + j * cell}, {x0 + (i + 1) * cell, z0 + (j + 1) * cell}, {x0 + i * cell, z0 + (j + 1) * cell}};
                const float lo = std::min({c[0], c[1], c[2], c[3]}), hi = std::max({c[0], c[1], c[2], c[3]});
                for (float lvl = std::ceil(lo / step) * step; lvl < hi; lvl += step) {
                    V2 hits[4];
                    int k = 0;
                    for (int e = 0; e < 4 && k < 4; ++e) {
                        const float a = c[e], b = c[(e + 1) % 4];
                        if ((a < lvl) != (b < lvl)) hits[k++] = Lerp2(p[e], p[(e + 1) % 4], (lvl - a) / (b - a));
                    }
                    const float d = Len((p[0] + p[2]) * 0.5f - centre);
                    if (d > radius) continue;
                    const float fade = 1.0f - d / radius;
                    const bool major = std::fmod(std::fabs(lvl), step * 5) < 0.01f;
                    const Vec3 col = major ? Vec3{0.95f * fade + 0.05f, 0.8f * fade + 0.05f, 0.4f * fade} : Vec3{0.6f * fade, 0.85f * fade, 1.0f * fade};
                    for (int m = 0; m + 1 < k; m += 2) {
                        lines.push_back({hits[m].x, lvl + 0.3f, hits[m].z, 0, 0, 0, col.x, col.y, col.z, 1});
                        lines.push_back({hits[m + 1].x, lvl + 0.3f, hits[m + 1].z, 0, 0, 0, col.x, col.y, col.z, 1});
                    }
                }
            }
    }

    // ------------------------------------------------------------------------------------------------ snapping
    Snap SnapAt(Vec3 hit, bool endpoint) {
        Snap s;
        V2 p = XZ(hit);
        const float hwNew = Prof().Half();
        const Asset& as = Current();
        auto withY = [&](Snap& r, V2 q) {
            r.p = {q.x, Ground(q.x, q.z) + elevation, q.z};
            return r;
        };
        if (!snapAll) return withY(s, p);
        // 1. existing geometry: nodes, then road surfaces (endpoints only)
        if (endpoint && On(SnapGeometry)) {
            const int n = net.NearestNode(p, std::max(5.0f, hwNew + 3.0f));
            if (n >= 0) {
                s.p = net.NodePos(n), s.node = n, s.kind = "node";
                return s;
            }
            float t, d;
            const int seg = as.kind == AssetKind::Road ? net.SegmentAt(p, 1.5f, t, d) : net.SegmentAt(p, 0.0f, t, d);
            if (seg >= 0) {
                const V2 q = net.Curve(seg).At(t);
                s.p = {q.x, net.YAt(net.segs[seg], t), q.z}, s.seg = seg, s.t = t, s.kind = "road";
                return s;
            }
        }
        const Snap* from = pts.empty() ? nullptr : &pts.back();
        bool done = false;
        // 2. guidelines from nearby roads: extensions of dead ends and perpendiculars through nodes
        std::vector<std::pair<V2, V2>> lines; // point, direction
        if (On(SnapGuide)) {
            for (size_t i = 0; i < net.nodes.size(); ++i) {
                const Node& nd = net.nodes[i];
                if (!nd.alive || Len(XZ(nd.p) - p) > 220.0f) continue;
                for (int sg : nd.segs) {
                    const V2 u = net.OutDir(sg, (int)i);
                    if (nd.segs.size() == 1) lines.push_back({XZ(nd.p), u * -1.0f});
                    lines.push_back({XZ(nd.p), RightOf(u)});
                    lines.push_back({XZ(nd.p), LeftOf(u)});
                }
            }
        }
        // 3. straight directions from the previous point: 15 degree steps from its road (or the world axes)
        if (from && On(SnapStraight)) {
            const V2 o = XZ(from->p);
            V2 ref{1, 0};
            if (from->node >= 0 && net.nodes[from->node].alive && !net.nodes[from->node].segs.empty()) ref = net.OutDir(net.nodes[from->node].segs.front(), from->node) * -1.0f;
            else if (from->seg >= 0 && net.segs[from->seg].alive) ref = net.Curve(from->seg).Tan(from->t);
            else if (contHas_) ref = contDir_;
            const V2 d = p - o;
            const float L = Len(d);
            if (L > 2.0f) {
                const float rel = Angle(d) - Angle(ref), stepA = 15.0f * 0.01745329f;
                const float snapped = std::round(rel / stepA) * stepA;
                const float off = std::fabs(rel - snapped) * L; // metres off the snapped ray at the cursor
                if (off < std::max(2.5f, L * 0.035f)) {
                    const float a = Angle(ref) + snapped;
                    const V2 dir{std::cos(a), std::sin(a)};
                    float len = L;
                    // a guideline crossing this ray near the cursor wins (and pins the length)
                    V2 best;
                    float bestD = 6.0f;
                    for (const auto& ln : lines) {
                        V2 q;
                        if (LineHit(o, dir, ln.first, ln.second, q) && Dot2(q - o, dir) > 4.0f && Len(q - p) < bestD) bestD = Len(q - p), best = q;
                    }
                    if (bestD < 6.0f) {
                        p = best;
                        guides.push_back({{o.x, from->p.y + 0.6f, o.z}, {p.x, from->p.y + 0.6f, p.z}});
                        s.kind = "guide";
                    } else {
                        if (On(SnapLength)) len = std::max(8.0f, std::round(L / 8.0f) * 8.0f);
                        p = o + dir * len;
                        s.kind = On(SnapLength) ? "length" : "angle";
                        const V2 farEnd = o + dir * (len + 30.0f);
                        guides.push_back({{o.x, from->p.y + 0.6f, o.z}, {farEnd.x, from->p.y + 0.6f, farEnd.z}});
                    }
                    done = true;
                }
            }
        }
        if (!done && !lines.empty()) { // single guideline, or two crossing near the cursor
            V2 best = p;
            float bd = 3.0f;
            const std::pair<V2, V2>* hitLine = nullptr;
            for (const auto& ln : lines) {
                const V2 rel = p - ln.first;
                const float along = Dot2(rel, ln.second);
                if (along < 3.0f) continue;
                const V2 q = ln.first + ln.second * along;
                if (Len(q - p) < bd) bd = Len(q - p), best = q, hitLine = &ln;
            }
            if (hitLine) {
                for (const auto& ln : lines) { // a crossing of two guidelines snaps harder
                    if (&ln == hitLine) continue;
                    V2 q;
                    if (LineHit(hitLine->first, hitLine->second, ln.first, ln.second, q) && Len(q - p) < 5.0f && Dot2(q - ln.first, ln.second) > 3.0f) {
                        best = q;
                        guides.push_back({{ln.first.x, hit.y + 0.6f, ln.first.z}, {q.x, hit.y + 0.6f, q.z}});
                        break;
                    }
                }
                p = best;
                guides.push_back({{hitLine->first.x, hit.y + 0.6f, hitLine->first.z}, {p.x, hit.y + 0.6f, p.z}});
                s.kind = "guide";
                done = true;
            }
        }
        // 4. net side: whole zone cells away from the nearest road's edge (parallel streets / lot depths)
        if (!done && On(SnapNetSide)) {
            int bestSeg = -1;
            float bt = 0, bd = 1e30f;
            for (size_t i = 0; i < net.segs.size(); ++i) {
                if (!net.segs[i].alive) continue;
                float t, d;
                net.Nearest((int)i, p, t, d);
                if (d < bd) bd = d, bt = t, bestSeg = (int)i;
            }
            if (bestSeg >= 0 && bd < 120.0f && bt > 0.02f && bt < 0.98f) {
                const float base = net.Prof(bestSeg).Half() + hwNew;
                const float k = std::round((bd - base) / 8.0f), target = base + std::max(0.0f, k) * 8.0f;
                if (std::fabs(bd - target) < 1.5f) {
                    const Cubic c = net.Curve(bestSeg);
                    const V2 q = c.At(bt), n = Norm(p - q);
                    p = q + n * target;
                    s.kind = "net side";
                    guides.push_back({{q.x, hit.y + 0.6f, q.z}, {p.x, hit.y + 0.6f, p.z}});
                    done = true;
                }
            }
        }
        // 5. zone grid (8 m world cells)
        if (!done && On(SnapZone)) {
            p = {std::round(p.x / 8.0f) * 8.0f, std::round(p.z / 8.0f) * 8.0f};
            s.kind = "grid";
        }
        return withY(s, p);
    }

    // ------------------------------------------------------------------------------------------------ validation
    bool Validate(std::vector<PreviewCurve>& curves) {
        for (const PreviewCurve& pc : curves) {
            const Profile& p = Profiles()[pc.profile];
            const float len = pc.c.Length();
            if (len < 4.0f) { error = "Road too short"; return false; }
            if (std::fabs(pc.yb - pc.ya) / len > 0.32f) { error = "Slope too steep"; return false; }
            if (pc.c.MaxTurnPerMetre() > 1.0f / std::max(4.0f, p.Half() * 0.9f)) { error = "Curve too sharp"; return false; }
        }
        // the angle against roads already at the start / end
        if (Current().kind == AssetKind::Road && mode != ToolMode::Grid && !curves.empty()) {
            const PreviewCurve& pc = curves.front();
            for (int end = 0; end < 2; ++end) {
                const Snap& sp = end ? cursor : pts.front();
                const V2 d = end ? pc.c.Tan(1) * -1.0f : pc.c.Tan(0);
                std::vector<V2> arms;
                if (sp.node >= 0 && sp.node < (int)net.nodes.size() && net.nodes[sp.node].alive)
                    for (int s : net.nodes[sp.node].segs) arms.push_back(net.OutDir(s, sp.node));
                else if (sp.seg >= 0 && sp.seg < (int)net.segs.size() && net.segs[sp.seg].alive) {
                    const V2 t = net.Curve(sp.seg).Tan(sp.t);
                    arms.push_back(t), arms.push_back(t * -1.0f);
                }
                for (const V2& u : arms)
                    if (Dot2(u, d) > 0.96f) { error = "Angle too sharp"; return false; } // under ~16 degrees
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------------ committing
    // The node at p: an existing node within 0.75 m, a split of a road passing within 0.6 m, or a new node.
    int FindOrMake(Vec3 p) {
        const V2 q = XZ(p);
        const int n = net.NearestNode(q, 0.75f);
        if (n >= 0) return n;
        for (size_t i = 0; i < net.segs.size(); ++i) {
            if (!net.segs[i].alive) continue;
            float t, d;
            net.Nearest((int)i, q, t, d);
            if (d > 0.6f) continue;
            const float len = net.Curve((int)i).Length();
            if (t * len < 1.0f) return net.segs[i].a;
            if ((1 - t) * len < 1.0f) return net.segs[i].b;
            if (std::fabs(net.YAt(net.segs[i], t) - p.y) > 3.0f) continue; // a road above / below: no junction
            return net.Split((int)i, t);
        }
        // at elevation 0 a new node is a ground node (it follows the terrain), however the cursor's height rounded
        const bool grounded = std::fabs(elevation) < 0.01f ? std::fabs(p.y - Ground(p.x, p.z)) < 2.0f : std::fabs(p.y - Ground(p.x, p.z)) < 0.3f;
        return net.AddNode(p, grounded);
    }

    // Adds a road from node na to nb along curve c; splits it (and the roads it crosses) at every same-level crossing.
    void AddWithCrossings(int na, int nb, Cubic c, int profile, std::vector<int>* made = nullptr) {
        if (na == nb) return;
        c.p0 = XZ(net.nodes[na].p), c.p3 = XZ(net.nodes[nb].p);
        const float ya = net.NodeY(na), yb = net.NodeY(nb);
        const int follow = net.nodes[na].grounded && net.nodes[nb].grounded ? 1 : 0;
        struct X {
            float t;
            V2 p;
            float y;
        };
        std::vector<X> xs;
        const int N = 64;
        std::vector<V2> pn(N + 1);
        for (int i = 0; i <= N; ++i) pn[i] = c.At((float)i / N);
        const size_t segCount = net.segs.size();
        for (size_t s = 0; s < segCount; ++s) {
            if (!net.segs[s].alive) continue;
            const Cubic e = net.Curve((int)s);
            std::vector<V2> pe(N + 1);
            for (int j = 0; j <= N; ++j) pe[j] = e.At((float)j / N);
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) {
                    const V2 r = pn[i + 1] - pn[i], q = pe[j + 1] - pe[j];
                    const float den = Cross2(r, q);
                    if (std::fabs(den) < 1e-6f) continue;
                    const V2 w = pe[j] - pn[i];
                    const float u = Cross2(w, q) / den, v = Cross2(w, r) / den;
                    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
                    const float tn = (i + u) / N, te = (j + v) / N;
                    const V2 P = pn[i] + r * u;
                    if (Len(P - c.p0) < 3.0f || Len(P - c.p3) < 3.0f) continue;
                    const float yn = FollowY(c, ya, yb, tn, ground, follow), ye = net.YAt(net.segs[s], te);
                    if (std::fabs(yn - ye) > 4.0f) continue; // grade separated: bridge or tunnel passes
                    xs.push_back({tn, P, ye});
                }
        }
        std::sort(xs.begin(), xs.end(), [](const X& a, const X& b) { return a.t < b.t; });
        std::vector<X> keep;
        for (const X& x : xs)
            if (keep.empty() || Len(x.p - keep.back().p) > 4.0f) keep.push_back(x);
        int prev = na;
        Cubic rest = c;
        float used = 0;
        for (const X& x : keep) {
            const int m = FindOrMake({x.p.x, x.y, x.p.z});
            if (m == prev || m == nb) continue;
            Cubic l, r;
            rest.Split((x.t - used) / (1 - used), l, r);
            l.p3 = r.p0 = XZ(net.nodes[m].p);
            const int id = net.AddSeg(prev, m, l, profile);
            if (made) made->push_back(id);
            prev = m, rest = r, used = x.t;
        }
        rest.p3 = XZ(net.nodes[nb].p);
        if (prev != nb) {
            const int id = net.AddSeg(prev, nb, rest, profile);
            if (made) made->push_back(id);
        }
    }

    // Levels the terrain under ground roads (twice: the second pass settles the smoothing over the first pass's cut).
    // Off-ground roads (bridges, tunnels, ramps) leave it alone except where they actually touch the ground.
    void Level(std::vector<int> segs) {
        if (!flatten) return;
        // the roads already at the touched nodes get re-levelled too, so the new bed does not tilt theirs
        std::vector<int> nodesTouched;
        for (int s : std::vector<int>(segs))
            if (s >= 0 && net.segs[s].alive)
                for (int n : {net.segs[s].a, net.segs[s].b}) {
                    nodesTouched.push_back(n);
                    for (int o : net.nodes[n].segs)
                        if (std::find(segs.begin(), segs.end(), o) == segs.end()) segs.push_back(o);
                }
        for (int pass = 0; pass < 2; ++pass) {
            for (int n : nodesTouched) { // junction pads
                const Node& nd = net.nodes[n];
                if (!nd.alive || !nd.grounded || nd.segs.size() < 3) continue;
                float r = 0;
                for (int o : nd.segs) r = std::max(r, net.Prof(o).Half());
                flatten(nd.p.x, nd.p.z, r + 6.0f, r + 14.0f, net.NodeY(n));
            }
            for (int s : segs) {
                if (s < 0 || !net.segs[s].alive) continue;
                const Cubic c = net.Curve(s);
                const float len = c.Length(), hw = net.Prof(s).Half();
                const bool follows = net.Follows(net.segs[s]);
                const int n = std::max(2, (int)(len / 2.0f));
                for (int i = 0; i <= n; ++i) {
                    const float t = (float)i / n;
                    const V2 q = c.At(t);
                    const float y = net.YAt(net.segs[s], t), g = Ground(q.x, q.z);
                    if (!follows && (y > g + 1.5f || y < g - mesh::kTunnelDepth)) continue;
                    // the embankment / cut widens with its depth (about 1:2), so deep cuts get smooth banks, not scalloped walls
                    flatten(q.x, q.z, hw + 1.0f, hw + 1.0f + std::max(10.0f, 2.0f * std::fabs(g - y)), y);
                }
            }
        }
        // auto-smooth the banks: blur the ground beside each road (outside its bed) so overlapping round brushes and
        // neighbouring roads' cuts leave a smooth slope instead of scallops and ridges
        for (int pass = 0; pass < 2; ++pass)
            for (int s : segs) {
                if (s < 0 || !net.segs[s].alive) continue;
                const Cubic c = net.Curve(s);
                const float len = c.Length(), hw = net.Prof(s).Half();
                const int n = std::max(2, (int)(len / 2.0f));
                for (int i = 0; i <= n; ++i) {
                    const float t = (float)i / n;
                    const V2 q = c.At(t), l = LeftOf(c.Tan(t));
                    for (float side : {-1.0f, 1.0f})
                        for (float o = hw + 3.0f; o <= hw + 24.0f; o += 2.0f) {
                            const V2 e = q + l * (side * o);
                            if (OnAnyBed(e)) continue; // never soften a road bed
                            float sum = 0;
                            for (int dz = -2; dz <= 2; ++dz)
                                for (int dx = -2; dx <= 2; ++dx) sum += Ground(e.x + dx * 2.0f, e.z + dz * 2.0f);
                            flatten(e.x, e.z, 0.0f, 3.0f, sum / 25.0f);
                        }
                }
            }
    }

    // Inside some road's bed (its half width + 1 m)?
    bool OnAnyBed(V2 p) const {
        for (size_t i = 0; i < net.segs.size(); ++i) {
            if (!net.segs[i].alive) continue;
            const Cubic c = net.Curve((int)i);
            const float hw = net.Prof((int)i).Half() + 1.5f;
            // cheap reject on the control box
            const float x0 = std::min({c.p0.x, c.p1.x, c.p2.x, c.p3.x}) - hw, x1 = std::max({c.p0.x, c.p1.x, c.p2.x, c.p3.x}) + hw;
            const float z0 = std::min({c.p0.z, c.p1.z, c.p2.z, c.p3.z}) - hw, z1 = std::max({c.p0.z, c.p1.z, c.p2.z, c.p3.z}) + hw;
            if (p.x < x0 || p.x > x1 || p.z < z0 || p.z > z1) continue;
            float t, d;
            net.Nearest((int)i, p, t, d);
            if (d < hw) return true;
        }
        return false;
    }

    void CommitCurve(const PreviewCurve& pc, const Snap* start, const Snap* end) {
        const int na = FindOrMake(start ? start->p : Vec3{pc.c.p0.x, pc.ya, pc.c.p0.z});
        const int nb = FindOrMake(end ? end->p : Vec3{pc.c.p3.x, pc.yb, pc.c.p3.z});
        std::vector<int> made;
        AddWithCrossings(na, nb, pc.c, pc.profile, &made);
        Level(made);
    }

    void CommitIntersection(const std::vector<PreviewCurve>& arms) {
        if (arms.empty()) return;
        const int centre = FindOrMake({arms[0].c.p0.x, arms[0].ya, arms[0].c.p0.z});
        std::vector<int> made;
        for (const PreviewCurve& a : arms) {
            const int e = FindOrMake({a.c.p3.x, a.yb, a.c.p3.z});
            AddWithCrossings(centre, e, a.c, a.profile, &made);
        }
        Level(made);
    }

    void CommitRoundabout(const std::vector<PreviewCurve>& ring, float radius) {
        std::vector<int> nodes;
        for (const PreviewCurve& q : ring) nodes.push_back(FindOrMake({q.c.p0.x, q.ya, q.c.p0.z}));
        std::vector<int> made;
        for (size_t i = 0; i < ring.size(); ++i) AddWithCrossings(nodes[i], nodes[(i + 1) % ring.size()], ring[i].c, ring[i].profile, &made);
        // roads that ran through the middle end at the ring
        const V2 c = (ring[0].c.p0 + ring[2].c.p0) * 0.5f;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || std::find(made.begin(), made.end(), (int)s) != made.end()) continue;
            const Cubic k = net.Curve((int)s);
            if (Len(k.At(0.5f) - c) < radius * 0.8f && Len(k.p0 - c) < radius + 1.0f && Len(k.p3 - c) < radius + 1.0f) net.RemoveSeg((int)s);
        }
        Level(made);
    }

    bool GridCurves(std::vector<PreviewCurve>& out) {
        const V2 a = XZ(pts[0].p), b = XZ(pts[1].p), c = XZ(cursor.p);
        const V2 u = Norm(b - a);
        const float L1 = Len(b - a);
        const float side = Dot2(c - b, RightOf(u));
        const V2 v = RightOf(u) * (side >= 0 ? 1.0f : -1.0f);
        const float L2 = std::fabs(side);
        const int n1 = std::max(1, (int)std::round(L1 / gridSpacing)), n2 = std::max(1, (int)std::round(L2 / gridSpacing));
        const int prof = ProfileId();
        auto add = [&](V2 p, V2 q) { out.push_back({Cubic::Line(p, q), Ground(p.x, p.z) + elevation, Ground(q.x, q.z) + elevation, prof}); };
        if (L2 < 4.0f) {
            add(a, b);
            return Validate(out);
        }
        for (int i = 0; i <= n2; ++i) add(a + v * (L2 * i / n2), a + v * (L2 * i / n2) + u * L1);
        for (int j = 0; j <= n1; ++j) add(a + u * (L1 * j / n1), a + u * (L1 * j / n1) + v * L2);
        return Validate(out);
    }
};

} // namespace city
} // namespace tbd
