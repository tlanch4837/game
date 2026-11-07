// The five faces of a city chunk bake (Track R): which way each orthographic view looks into the cell's box, and how its
// texels map to the box. One table for both sides: the bake's matrices (C++, render/chunk_faces.h includes this file inside
// its own namespace with a float3 of its own) and ChunkVolPS (mesh.hlsl), so the two can never disagree.
//
// The box is normalized: p in [0,1]^3, x east, y up, z south (world axes). A face's texel (u, v) in [0,1]^2, v = 0 the top row,
// and d the depth into the box along its view (0 at the face's own plane, 1 at the far side):
//     p = o + u * U + v * V + d * D           u = dot(p - o, U), v = dot(p - o, V), d = dot(p - o, D)
//   face 0  top        looking down       u = x       v = z       d = 1 - y
//   face 1  east  (+x) looking west       u = 1 - z   v = 1 - y   d = 1 - x
//   face 2  west  (-x) looking east       u = z       v = 1 - y   d = x
//   face 3  south (+z) looking north      u = x       v = 1 - y   d = 1 - z
//   face 4  north (-z) looking south      u = 1 - x   v = 1 - y   d = z
// The sides keep the roof on their top row, so a dumped face reads upright.
// Atlas: a cell owns a strip of five tiles, face f at texel x = slot.x + f * tile.
#ifdef __cplusplus
#define CHUNKFACE_FN inline
#define CHUNKFACE_OUT(T) T&
#else
#define CHUNKFACE_FN
#define CHUNKFACE_OUT(T) out T
#endif

static const int kChunkFaces = 5;

CHUNKFACE_FN void ChunkFaceFrame(int f, CHUNKFACE_OUT(float3) o, CHUNKFACE_OUT(float3) U, CHUNKFACE_OUT(float3) V, CHUNKFACE_OUT(float3) D) {
    if (f == 0) {
        o = float3(0, 1, 0); U = float3(1, 0, 0); V = float3(0, 0, 1); D = float3(0, -1, 0);
    } else if (f == 1) {
        o = float3(1, 1, 1); U = float3(0, 0, -1); V = float3(0, -1, 0); D = float3(-1, 0, 0);
    } else if (f == 2) {
        o = float3(0, 1, 0); U = float3(0, 0, 1); V = float3(0, -1, 0); D = float3(1, 0, 0);
    } else if (f == 3) {
        o = float3(0, 1, 1); U = float3(1, 0, 0); V = float3(0, -1, 0); D = float3(0, 0, -1);
    } else {
        o = float3(1, 1, 0); U = float3(-1, 0, 0); V = float3(0, -1, 0); D = float3(0, 0, 1);
    }
}

// (u, v, d) of a normalized box point as seen by face f.
CHUNKFACE_FN float3 ChunkFaceUVD(int f, float3 p) {
    float3 o, U, V, D;
    ChunkFaceFrame(f, o, U, V, D);
    const float3 q = p - o;
    return float3(dot(q, U), dot(q, V), dot(q, D));
}

// The side face that sees a surface the ray (box-space direction r) hits while travelling mostly along x (axisX) or z.
CHUNKFACE_FN int ChunkSideFacing(float3 r, bool axisX) {
    return axisX ? (r.x < 0.0 ? 1 : 2) : (r.z < 0.0 ? 3 : 4);
}
