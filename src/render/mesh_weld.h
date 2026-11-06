// Turns a flat vertex list into shared vertices and indices. GPUs reuse the result of a recently transformed index, so
// welded meshes run the vertex shader far less often than flat triangle lists.
#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "renderer.h"

namespace tbd {

inline void WeldVertices(const Vertex* in, uint32_t count, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    std::unordered_map<std::string_view, uint32_t> seen; // keys point into `in`, every field a float with no padding
    seen.reserve(count);
    vertices.clear();
    vertices.reserve(count / 3 + 16);
    indices.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const std::string_view key(reinterpret_cast<const char*>(&in[i]), sizeof(Vertex));
        const auto [it, added] = seen.try_emplace(key, (uint32_t)vertices.size());
        if (added) vertices.push_back(in[i]);
        indices[i] = it->second;
    }
}

} // namespace tbd
