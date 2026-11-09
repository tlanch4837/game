// Phase 4 coverage by travel time with capacity (CS2 sim roadmap Phase 4). Header only, headless
// (tests\sim_svc_coverage_test).
//
//   SvcGraph    a compact copy of the herds' LinkTable (free-flow minutes per link, links into each node): the coverage
//               pass runs on a worker on this immutable snapshot while the main thread may rebuild the table.
//   SvcCovIn    the snapshot: city cells (node, people) and service sites (spec after funding, node or position).
//   SvcCover    the pure pass: per site a bounded Dijkstra on the reversed graph (minutes from every node to the site,
//               cut at the site's reach) or the crow-fly distance; every (site, cell) pair within reach is sorted by
//               (minutes, site, cell) and served local first: a cell takes from its nearest sites until its people are
//               covered, a site gives until its capacity is gone, so an overloaded clinic covers less. A cell's
//               coverage = sum over its suppliers of share x falloff(minutes) (255 near, 30 % at the reach edge).
//               Also the nearest kSvcNear sites per cell and slot (dispatch and school seats read these).
// The result depends only on the input: the host applies it at the next tick boundary, whatever the worker's timing.
#pragma once

#include <array>
#include <cmath>
#include <memory>
#include <queue>
#include <vector>

#include "city_cells.h"
#include "sim_herds.h"
#include "sim_svc_catalog.h"

namespace tbd {
namespace city {

constexpr int kSvcNear = 3;

struct SvcGraph {
    int nodes = 0;
    uint64_t version = 0;
    std::vector<int> inOff, inFrom; // links into node n: inFrom[inOff[n] .. inOff[n+1]) are their start nodes
    std::vector<float> inMin;       // and their free-flow minutes
    static SvcGraph From(const LinkTable& t) {
        SvcGraph g;
        g.nodes = t.nodes, g.version = t.version;
        g.inOff.assign((size_t)t.nodes + 1, 0);
        if (t.inOff.size() != (size_t)t.nodes + 1) return g;
        g.inOff = t.inOff;
        g.inFrom.resize(t.inIdx.size()), g.inMin.resize(t.inIdx.size());
        for (size_t k = 0; k < t.inIdx.size(); ++k) {
            const LinkInfo& l = t.links[(size_t)t.inIdx[k]];
            g.inFrom[k] = l.from, g.inMin[k] = l.len / std::max(1e-3f, l.freeMpm);
        }
        return g;
    }
    // Minutes from every node to `dest`, cut at `limit` (INFINITY beyond); `touched` lists the nodes reached.
    void Reach(int dest, float limit, std::vector<float>& minutes, std::vector<int>& touched) const {
        for (int n : touched) minutes[(size_t)n] = INFINITY;
        touched.clear();
        if (minutes.size() != (size_t)nodes) minutes.assign((size_t)nodes, INFINITY);
        if (dest < 0 || dest >= nodes) return;
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        minutes[(size_t)dest] = 0, touched.push_back(dest), q.push({0.0f, dest});
        while (!q.empty()) {
            const auto [d, n] = q.top();
            q.pop();
            if (d > minutes[(size_t)n]) continue;
            for (int k = inOff[(size_t)n]; k < inOff[(size_t)n + 1]; ++k) {
                const int f = inFrom[(size_t)k];
                const float nd = d + inMin[(size_t)k];
                if (nd > limit || nd >= minutes[(size_t)f]) continue;
                if (!std::isfinite(minutes[(size_t)f])) touched.push_back(f);
                minutes[(size_t)f] = nd, q.push({nd, f});
            }
        }
    }
};

struct SvcCellIn {
    CellKey key{};
    float x = 0, z = 0;
    int node = -1;          // road node of the cell's busiest building (-1: no road)
    uint32_t people = 0;    // residents + half the workers: the demand on a service
};
struct SvcSite {
    uint32_t id = 0;        // BuildingId (or a host id for garbage sites)
    SvcSpec spec;           // after funding
    float x = 0, z = 0;
    int node = -1;
};
struct SvcCovIn {
    std::vector<SvcCellIn> cells;
    std::vector<SvcSite> sites;
    std::shared_ptr<const SvcGraph> graph; // null: crow flies for everything (no roads yet)
};
struct SvcNear {
    int32_t site = -1;      // index into SvcCovIn::sites
    float minutes = INFINITY;
};
struct SvcCovOut {
    std::vector<std::array<uint8_t, kServicesV2>> cov; // per cell
    std::vector<SvcNear> nearest;                     // (cell * kServicesV2 + slot) * kSvcNear + k
    std::vector<uint32_t> served;                     // per site: people it covers
    std::vector<uint8_t> slotHasSite;                 // per slot: any site at all
    const SvcNear& Near(size_t cell, int slot, int k) const { return nearest[(cell * kServicesV2 + (size_t)slot) * kSvcNear + (size_t)k]; }
};

inline float SvcFalloff(float minutes, float reach) { return reach > 0 ? std::clamp(1.0f - 0.7f * minutes / reach, 0.0f, 1.0f) : 0.0f; }

} // namespace city
} // namespace tbd
