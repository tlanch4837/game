// "Graphics: MAX" (the Ultra preset's look): sky, clouds and sun shadows on, the sun at a fixed key light. Header only,
// headless (tests\max_look_test). MAX sits on top of the scene's own sun: it keeps the sun it replaced (the base) and puts
// it back when it goes off. Whatever sets the scene's own sun while MAX may be on (a new city map) goes through EditBase,
// so the change lands in the base and MAX stays on top: Ultra at startup then looks the same as switching High -> Ultra.
#pragma once
#include "sun.h"

namespace tbd {

struct MaxLook {
    bool on = false;
    float hour = 0.0f, tilt = 0.0f;
    bool sky = true, shadows = true, animate = false;

    void Set(Sun& sun, bool want) {
        if (want == on) return;
        on = want;
        if (want) {
            hour = sun.hour, tilt = sun.tiltDeg, sky = sun.showSky, shadows = sun.shadows, animate = sun.animate;
            // the sun's everyday path stays behind a head that faces +Z, so lean it round to the front: a key light
            sun.hour = 13.6f;
            sun.tiltDeg = 50.0f;
            sun.showSky = sun.shadows = true;
            sun.animate = false;
        } else {
            sun.hour = hour, sun.tiltDeg = tilt, sun.showSky = sky, sun.shadows = shadows, sun.animate = animate;
        }
    }

    // Changes the scene's own sun (`edit(sun)`): with MAX on, MAX comes off, the edit is made, and MAX goes back on.
    template <class Edit> void EditBase(Sun& sun, Edit&& edit) {
        const bool was = on;
        Set(sun, false);
        edit(sun);
        Set(sun, was);
    }
};

} // namespace tbd
