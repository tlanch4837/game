// Headless test of src/max_look.h: Ultra's MAX look applied at startup, before a new city sets its own sun, must leave the
// same sun as a player switching High -> Ultra afterwards (the iPad's "no sky, no clouds until I switch presets" bug).
#include <cstdio>

#include "max_look.h"

static int g_fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fails; } } while (0)

static void CitySun(tbd::Sun& s) { s.showSky = false, s.hour = 15.6f, s.tiltDeg = 30.0f; } // as game_core.inl SetCitySun

static bool Same(const tbd::Sun& a, const tbd::Sun& b) {
    return a.hour == b.hour && a.tiltDeg == b.tiltDeg && a.showSky == b.showSky && a.shadows == b.shadows && a.animate == b.animate;
}

int main() {
    // A: the settings file says Ultra, applied on the first GUI frame; then the player starts a new map.
    tbd::Sun a;
    tbd::MaxLook la;
    la.Set(a, true);
    la.EditBase(a, CitySun);
    // B: the new map first, then High -> Ultra on the Graphics page.
    tbd::Sun b;
    tbd::MaxLook lb;
    lb.EditBase(b, CitySun);
    lb.Set(b, false); // High
    lb.Set(b, true);  // Ultra
    CHECK(Same(a, b));
    CHECK(a.showSky && a.shadows && !a.animate && a.hour == 13.6f); // MAX: the marched sky, clouds and shadows
    CHECK(la.on && lb.on);
    // Ultra -> High after either path: the city's own sun (no marched sky, the afternoon light).
    la.Set(a, false), lb.Set(b, false);
    CHECK(Same(a, b));
    CHECK(!a.showSky && a.hour == 15.6f && a.tiltDeg == 30.0f);
    // Without MAX, EditBase is a plain edit.
    tbd::Sun c;
    tbd::MaxLook lc;
    lc.EditBase(c, CitySun);
    CHECK(!lc.on && !c.showSky && c.hour == 15.6f);
    // Set twice is a no-op (ApplySettings runs from the title and the dock).
    tbd::Sun d;
    tbd::MaxLook ld;
    ld.Set(d, true), ld.EditBase(d, CitySun), ld.Set(d, true);
    CHECK(ld.on && d.showSky && d.hour == 13.6f);
    ld.Set(d, false);
    CHECK(!d.showSky && d.hour == 15.6f); // the base is still the city's sun, not MAX's
    std::printf(g_fails ? "max_look_test: %d FAILED\n" : "max_look_test: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
