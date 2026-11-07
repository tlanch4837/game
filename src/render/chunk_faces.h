// The chunk bake's face table for C++ (Track R): shaders/chunk_faces.hlsli, compiled here with a float3 of its own so the same
// lines drive the bake's matrices and ChunkVolPS. Plus the face's orthographic matrix over a world box.
#pragma once
#include "../vecmath.h"

namespace tbd {
namespace chunkface {

struct float3 {
    float x = 0, y = 0, z = 0;
    constexpr float3() = default;
    constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
    float3 operator-(const float3& o) const { return {x - o.x, y - o.y, z - o.z}; }
};
inline float dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

#include "../shaders/chunk_faces.hlsli"

} // namespace chunkface

constexpr int kChunkFaceCount = chunkface::kChunkFaces;

// World -> face clip space for the box [lo, lo + size]: x = 2u - 1, y = 1 - 2v (v = 0 the top row), z = d (0 at the face's plane,
// 1 at the far side; the bake tests depth LESS). Column-major like every Mat4 here (m[col * 4 + row]).
inline Mat4 ChunkFaceMatrix(int f, Vec3 lo, Vec3 size) {
    chunkface::float3 o, U, V, D;
    chunkface::ChunkFaceFrame(f, o, U, V, D);
    const float inv[3] = {1.0f / size.x, 1.0f / size.y, 1.0f / size.z};
    const float lov[3] = {lo.x, lo.y, lo.z};
    // p_i = (w_i - lo_i) * inv_i; u = dot(p - o, U) etc.
    const auto row = [&](const chunkface::float3& axis, float scale, float bias, float* out) { // out = scale * dot(p - o, axis) + bias, in w
        const float a[3] = {axis.x, axis.y, axis.z}, oo[3] = {o.x, o.y, o.z};
        float c = 0.0f;
        for (int i = 0; i < 3; ++i) {
            out[i] = scale * a[i] * inv[i];
            c += a[i] * (-lov[i] * inv[i] - oo[i]);
        }
        out[3] = scale * c + bias;
    };
    float rx[4], ry[4], rz[4];
    row(U, 2.0f, -1.0f, rx);
    row(V, -2.0f, 1.0f, ry);
    row(D, 1.0f, 0.0f, rz);
    Mat4 m;
    for (int c = 0; c < 4; ++c) {
        m.At(0, c) = rx[c];
        m.At(1, c) = ry[c];
        m.At(2, c) = rz[c];
    }
    m.At(3, 3) = 1.0f;
    return m;
}

} // namespace tbd
