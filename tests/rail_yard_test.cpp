// Headless checks for the rail yards (src/city/transport.h YardTracks / YardPorts, road_tool.h "yard" snaps, rail_yard.h):
//   the yard snaps beside a road with its entrance on the road (and refuses a spot with no road);
//   every track of the model is a connection point of its own at both ends (N = 2 x tracks), 5 m apart on the shed roads;
//   a rotated yard moves its track ends with it, and a yard turned side-on offers none on its road side;
//   the rail tool snaps to the 2nd / 3rd / 5th track end, continues the yard track straight out, joins the rail graph,
//   refuses a join that is not along the track or runs back over it;
//   an unconnected yard runs no train (and says why); a connected one spawns its train on its own track, inside the lot,
//   which runs to the station on the rail, dwells, and comes back; no train ever appears out on the line.
// Build and run: tests\rail_yard_test.bat
#include <cmath>
#include <cstdio>
#include <cstring>

#include "city/rail_yard.h"
#include "city/road_tool.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static int Index(const char* name) {
    const auto& d = Defs();
    for (size_t i = 0; i < d.size(); ++i)
        if (std::string(d[i].name) == name) return (int)i;
    return -1;
}
static int PortOf(const std::vector<RailPort>& v, int track, int end) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].track == track && v[i].end == end) return (int)i;
    return -1;
}
static bool InLot(const Placed& b, V2 p, float pad = 0.5f) { return TransportWorld::Rect(b).Contains(p, pad); }

int main() {
    const int depot = Index("Railway Depot"), station = Index("Train Station"), cargo = Index("Cargo Train Terminal");
    const BuildingDef& D = Defs()[depot];
    const int road = ProfileIndex("Two-Lane Road");
    const float half = Profiles()[road].Half();
    CHECK(depot >= 0 && station >= 0 && cargo >= 0);

    // ------------------------------------------------------------------ the track ends follow the model
    {
        const std::vector<YardTrack> tr = YardTracks(D);
        CHECK(tr.size() == 7); // five shed roads + two sidings (transport_models.h RailDepot draws these)
        for (int i = 1; i < 5; ++i) CHECK(std::fabs(tr[i].z - tr[i - 1].z - 5.0f) < 1e-4f);
        const std::vector<RailPort> v = YardPorts(D, {0, 0}, {0, 1}, 0);
        CHECK(v.size() == 2 * tr.size()); // one connection point per track end
        for (size_t i = 0; i < v.size(); ++i)
            for (size_t k = i + 1; k < v.size(); ++k) CHECK(Len(v[i].p - v[k].p) > 4.9f);
        CHECK(YardTracks(Defs()[cargo]).size() == 3);
        CHECK(YardTracks(Defs()[station]).empty());
        // a rotated yard moves its track ends with it (free placement, the same centre)
        const V2 c{50, -20};
        const std::vector<RailPort> a = YardPorts(D, c, FrontFromDeg(0), 0), b = YardPorts(D, c, FrontFromDeg(90), 0);
        CHECK(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            const V2 r = a[i].p - c, want = c + V2{r.z, -r.x}; // a quarter turn in the tools' sense
            CHECK(Len(b[i].p - want) < 1e-3f);
            CHECK(std::fabs(Dot2(b[i].dir, V2{a[i].dir.z, -a[i].dir.x}) - 1.0f) < 1e-4f);
        }
    }

    // ------------------------------------------------------------------ placement: beside the road, entrance on it
    RoadTool t;
    auto flat = [](float, float) { return 0.0f; };
    t.ground = flat, t.net.ground = flat;
    t.BuildRoad(Cubic::Line({-500, 0}, {500, 0}), 0, 0, road);
    TransportWorld w;
    PlaceContext cx;
    cx.net = &t.net;
    cx.ground = flat;
    {
        const Placement p = w.Plan(cx, depot, {10, -70}, 0);
        CHECK(p.valid && p.roadSnapped && p.entranceEdge == 0 && p.roadSeg >= 0);
        CHECK(std::fabs(p.c.z + (half + kBuildingSnapGap + D.D() * 0.5f)) < 0.05f && std::fabs(p.c.x - 10) < 0.05f);
        CHECK(p.front.z > 0.999f && p.entranceDir.z > 0.999f); // the front (office, driveway) toward the road
        // no road in reach: refused, with the reason
        const Placement far = w.Plan(cx, depot, {10, -700}, 0);
        CHECK(!far.valid && far.error.find("road") != std::string::npos);
        // turned side-on (R: a quarter turn): its side is on the road, no track end is offered there
        const Placement q = w.Plan(cx, depot, {10, -90}, 90);
        std::printf("side-on: valid %d snapped %d err '%s' c=(%.1f,%.1f) front=(%.2f,%.2f) edge %d dir=(%.2f,%.2f)\n", q.valid, q.roadSnapped, q.error.c_str(), q.c.x, q.c.z, q.front.x, q.front.z, q.entranceEdge, q.entranceDir.x, q.entranceDir.z);
        CHECK(q.valid && q.roadSnapped && q.entranceDir.z > 0.999f);
        const std::vector<RailPort> qs = YardPorts(D, q.c, q.front, q.y, q.entranceDir);
        CHECK(qs.size() == YardTracks(D).size()); // only the far ends
        for (const RailPort& rp : qs) CHECK(rp.dir.z < -0.999f && rp.p.z < q.c.z);
        CHECK(std::fabs(std::fabs(q.c.z) - (half + kBuildingSnapGap + D.W() * 0.5f)) < 0.05f); // the long side's half
    }
    const Placement yp = w.Plan(cx, depot, {0, -70}, 0);
    CHECK(yp.valid);
    const int yi = w.Place(yp, depot);
    CHECK(yi == 0);
    w.items[0].state = BState::Done, w.items[0].progress = 1;
    const Placed& yard = w.items[0];
    std::vector<RailPort> ports = TransportWorld::PortsOf(&t.net, yard);
    CHECK(ports.size() == 14);
    t.ports = ports;

    // ------------------------------------------------------------------ no rail: no train, and the reason
    RailYardWorld rw;
    rw.Tick(t.net, w, 1.0f);
    CHECK(rw.trains.empty());
    CHECK(rw.StatusOf(yard.id) && rw.StatusOf(yard.id)->Message().find("not connected to rail") != std::string::npos);

    // ------------------------------------------------------------------ the rail tool snaps to the 2nd / 3rd / 5th track end
    t.active = true;
    t.Select(RoadTool::AssetIndex("Single Track Railway"));
    t.SetMode(ToolMode::Straight);
    for (int track : {1, 2, 4}) {
        const int k = PortOf(ports, track, 1);
        CHECK(k >= 0);
        const V2 near = ports[k].p + ports[k].dir * 1.5f + V2{ports[k].dir.z, -ports[k].dir.x} * 1.2f; // a little off
        t.Move(true, {near.x, 0, near.z});
        CHECK(t.cursor.port == k && std::strcmp(t.cursor.kind, "yard") == 0 && IsConnection(t.cursor.kind));
        CHECK(Len(XZ(t.cursor.p) - ports[k].p) < 1e-3f);
    }
    // build out of the 2nd track: 700 m east, straight on (the cursor a bit off the line: the snap carries the yard track on)
    const int k2 = PortOf(ports, 1, 1);
    {
        const V2 at = ports[k2].p;
        t.Move(true, {at.x + 1.0f, 0, at.z + 0.5f});
        t.Click();
        const V2 to = at + ports[k2].dir * 700.0f + V2{0, 6.0f};
        t.Move(true, {to.x, 0, to.z});
        std::vector<PreviewCurve> cv;
        CHECK(t.PendingCurves(cv));
        CHECK(!cv.empty() && Dot2(Norm(cv[0].c.Tan(0)), ports[k2].dir) > 0.9999f); // leaves along the yard track
        CHECK(t.Click());
        t.Cancel();
    }
    const int n2 = PortNode(t.net, ports[k2]);
    CHECK(n2 >= 0 && t.net.IsRailNode(n2)); // the yard is in the rail graph through its 2nd track
    // the 3rd track can join too (its own end, 5 m over), the 2nd no longer offers a yard snap (a railway is on it)
    {
        const int k3 = PortOf(ports, 2, 1);
        t.Move(true, {ports[k3].p.x + 0.5f, 0, ports[k3].p.z});
        CHECK(t.cursor.port == k3);
        t.Move(true, {ports[k2].p.x + 0.5f, 0, ports[k2].p.z});
        CHECK(t.cursor.port < 0 && t.cursor.node == n2);
        const int k5 = PortOf(ports, 4, 1);
        const V2 off = ports[k5].p + V2{std::cos(1.05f), std::sin(1.05f)} * 120.0f; // 60 degrees off the track
        std::vector<PreviewCurve> cv;
        // into the yard over its own track: refused
        t.Move(true, {ports[k5].p.x, 0, ports[k5].p.z});
        t.Click();
        const V2 back = ports[k5].p - ports[k5].dir * 60.0f;
        t.Move(true, {back.x, 0, back.z});
        CHECK(!t.PendingCurves(cv));
        std::printf("into the yard: %s\n", t.error.c_str());
        t.Cancel();
        // 60 degrees off, straight mode with the connect snap: the start's tangent is the track's, the chord is not
        t.Move(true, {ports[k5].p.x, 0, ports[k5].p.z});
        t.Click();
        t.Move(true, {off.x, 0, off.z});
        const bool ok60 = t.PendingCurves(cv);
        std::printf("60 deg off the track: %s (%s)\n", ok60 ? "bends out of it" : "refused", t.error.c_str());
        CHECK(ok60 ? Dot2(Norm(cv[0].c.Tan(0)), ports[k5].dir) > 0.999f : !t.error.empty()); // either leaves along the track or is refused
        t.Cancel();
    }

    // ------------------------------------------------------------------ connected, but no station yet: still no train
    rw.Tick(t.net, w, 6.0f);
    CHECK(rw.trains.empty() && rw.StatusOf(yard.id) && rw.StatusOf(yard.id)->Message().find("Station") != std::string::npos);

    // ------------------------------------------------------------------ a station on the line: the train runs
    const V2 stAt = ports[k2].p + ports[k2].dir * 420.0f + V2{0, -10.0f};
    Placement sp = w.Plan(cx, station, stAt, 0);
    std::printf("station: %s\n", sp.error.c_str());
    CHECK(sp.valid);
    const int si = w.Place(sp, station);
    w.items[(size_t)si].state = BState::Done;
    const V2 stC = w.items[(size_t)si].c;
    rw.Tick(t.net, w, 6.0f);
    CHECK(rw.trains.size() == 1);
    bool spawnOk = true, reached = false, home = false, stoppedAtStation = false;
    float maxSpeed = 0;
    std::vector<uint64_t> seen;
    for (int step = 0; step < 4000 && !home; ++step) {
        for (const YardTrain& tr : rw.trains) {
            if (std::find(seen.begin(), seen.end(), tr.id) == seen.end()) { // a train's first sighting: on the yard's own track, the whole consist in the lot
                seen.push_back(tr.id);
                for (int c = 0; c < kTrainCars; ++c) spawnOk = spawnOk && InLot(w.items[0], tr.car[c]);
                spawnOk = spawnOk && InLot(w.items[0], tr.spawn) && tr.phase == 0;
            }
            maxSpeed = std::max(maxSpeed, tr.speed);
            if (tr.phase == 1) {
                reached = true;
                stoppedAtStation = stoppedAtStation || Len(tr.Head() - stC) < 8.0f;
            }
            if (reached && tr.phase == 3) home = true;
        }
        rw.Tick(t.net, w, 1.0f);
    }
    std::printf("train: reached %d, at the platforms %d, back in the yard %d, top speed %.1f m/s, %zu trains\n", reached, stoppedAtStation, home, maxSpeed, rw.trains.size());
    CHECK(spawnOk);
    CHECK(reached && stoppedAtStation && home);
    CHECK(maxSpeed > 10.0f && maxSpeed <= kTrainMaxSpeed + 0.01f);
    CHECK((int)rw.trains.size() <= TrainsPerYard(D));
    // later runs (the second train, the next run after the layover) start in the yard too
    for (int step = 0; step < 3000; ++step) {
        for (const YardTrain& tr : rw.trains)
            if (std::find(seen.begin(), seen.end(), tr.id) == seen.end()) {
                seen.push_back(tr.id);
                spawnOk = spawnOk && InLot(w.items[0], tr.spawn);
            }
        for (const YardTrain& tr : rw.trains)
            if (tr.phase == 0 && tr.s < TrainLength() + 1.0f) spawnOk = spawnOk && InLot(w.items[0], tr.Head()); // a (re)start
        rw.Tick(t.net, w, 1.0f);
    }
    CHECK(spawnOk);
    CHECK(seen.size() >= 2);

    // ------------------------------------------------------------------ cut the rail: the trains are recalled
    t.net.RemoveSeg(t.net.nodes[(size_t)n2].segs.front());
    rw.Tick(t.net, w, 1.0f);
    CHECK(rw.trains.empty() && rw.StatusOf(yard.id)->Message().find("rail") != std::string::npos);

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
