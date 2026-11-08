// A rail yard's track end: where one of the yard's own tracks (drawn by the building's model) leaves its lot. The rail
// tool snaps a new railway to it ("yard" connection snap) and continues the yard track straight out; once a railway
// node sits on it the yard is part of the rail graph through that track (transport.h YardPorts, rail_yard.h trains).
// Header only, headless.
#pragma once
#include <cstdint>

#include "road_network.h"

namespace tbd {
namespace city {

struct RailPort {
    V2 p{};            // the track end on the lot's edge (xz)
    V2 dir{1, 0};      // pointing out of the yard, along the track
    float y = 0;       // the yard's level there
    uint64_t owner = 0; // the yard building's id
    int track = 0;     // which of the yard's tracks (YardTracks order)
    int end = 0;       // 0: the track's -x end, 1: its +x end (the building's frame)
};

// The live rail node on a port (a railway built onto it), or -1.
inline int PortNode(const Network& net, const RailPort& port, float radius = 1.0f) {
    int best = -1;
    float bd = radius;
    for (size_t i = 0; i < net.nodes.size(); ++i) {
        if (!net.nodes[i].alive || !net.IsRailNode((int)i)) continue;
        const float d = Len(XZ(net.nodes[i].p) - port.p);
        if (d < bd) bd = d, best = (int)i;
    }
    return best;
}

} // namespace city
} // namespace tbd
