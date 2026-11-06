// Minimal vector/matrix math. Right-handed, Y up, column vectors, column-major storage,
// clip-space depth 0..1 (what both Vulkan and D3D12 expect).
#pragma once
#include <cmath>

namespace tbd {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Normalize(Vec3 a) {
    float len = std::sqrt(Dot(a, a));
    return len > 0 ? a * (1.0f / len) : a;
}

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float m[16] = {}; // m[col * 4 + row]

    float& At(int row, int col) { return m[col * 4 + row]; }
    float At(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 Identity() {
        Mat4 r;
        r.At(0, 0) = r.At(1, 1) = r.At(2, 2) = r.At(3, 3) = 1.0f;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += a.At(row, k) * b.At(k, col);
            r.At(row, col) = sum;
        }
    return r;
}

inline Vec4 operator*(const Mat4& a, Vec4 v) {
    return {a.At(0, 0) * v.x + a.At(0, 1) * v.y + a.At(0, 2) * v.z + a.At(0, 3) * v.w,
            a.At(1, 0) * v.x + a.At(1, 1) * v.y + a.At(1, 2) * v.z + a.At(1, 3) * v.w,
            a.At(2, 0) * v.x + a.At(2, 1) * v.y + a.At(2, 2) * v.z + a.At(2, 3) * v.w,
            a.At(3, 0) * v.x + a.At(3, 1) * v.y + a.At(3, 2) * v.z + a.At(3, 3) * v.w};
}

inline Mat4 LookAtRH(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = Normalize(target - eye);
    Vec3 s = Normalize(Cross(f, up));
    Vec3 u = Cross(s, f);
    Mat4 r = Mat4::Identity();
    r.At(0, 0) = s.x;  r.At(0, 1) = s.y;  r.At(0, 2) = s.z;  r.At(0, 3) = -Dot(s, eye);
    r.At(1, 0) = u.x;  r.At(1, 1) = u.y;  r.At(1, 2) = u.z;  r.At(1, 3) = -Dot(u, eye);
    r.At(2, 0) = -f.x; r.At(2, 1) = -f.y; r.At(2, 2) = -f.z; r.At(2, 3) = Dot(f, eye);
    return r;
}

// Perspective projection with reversed Z, depth 1 (near) .. 0 (far), looking down -Z: float precision then stays
// usable out to the horizon.
inline Mat4 PerspectiveRH(float fovY, float aspect, float nearZ, float farZ) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 r;
    r.At(0, 0) = f / aspect;
    r.At(1, 1) = f;
    r.At(2, 2) = nearZ / (farZ - nearZ);
    r.At(2, 3) = nearZ * farZ / (farZ - nearZ);
    r.At(3, 2) = -1.0f;
    return r;
}

// Pixel coordinates (origin top-left, y down) to clip space, for 2D overlays.
inline Mat4 OrthoPixels(float width, float height) {
    Mat4 r;
    r.At(0, 0) = 2.0f / width;
    r.At(0, 3) = -1.0f;
    r.At(1, 1) = -2.0f / height;
    r.At(1, 3) = 1.0f;
    r.At(3, 3) = 1.0f;
    return r;
}

// Orthographic projection of the box x in [-halfW, halfW], y in [-halfH, halfH], distance nearZ .. farZ in front
// of the eye (looking down -Z), to depth 0 .. 1.
inline Mat4 OrthoRH(float halfW, float halfH, float nearZ, float farZ) {
    Mat4 r;
    r.At(0, 0) = 1.0f / halfW;
    r.At(1, 1) = 1.0f / halfH;
    r.At(2, 2) = 1.0f / (nearZ - farZ);
    r.At(2, 3) = nearZ / (nearZ - farZ);
    r.At(3, 3) = 1.0f;
    return r;
}

constexpr float kPi = 3.14159265358979f;
inline float Radians(float deg) { return deg * (kPi / 180.0f); }
inline float Degrees(float rad) { return rad * (180.0f / kPi); }

// Unit quaternion, (x, y, z) = vector part.
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat Normalize(Quat q) {
    float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return len > 0 ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
}
inline Quat QuatAxisAngle(Vec3 axis, float radians) {
    float s = std::sin(radians * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
}
inline Vec3 Rotate(Quat q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    return u * (2.0f * Dot(u, v)) + v * (q.w * q.w - Dot(u, u)) + Cross(u, v) * (2.0f * q.w);
}

// Euler angles in degrees (x = pitch, y = yaw, z = roll). Rotation = Ry * Rx * Rz.
inline Quat QuatFromEuler(Vec3 deg) {
    Quat qx = QuatAxisAngle({1, 0, 0}, Radians(deg.x));
    Quat qy = QuatAxisAngle({0, 1, 0}, Radians(deg.y));
    Quat qz = QuatAxisAngle({0, 0, 1}, Radians(deg.z));
    return Normalize(qy * qx * qz);
}
inline Vec3 EulerFromQuat(Quat q) {
    const Vec3 c0 = Rotate(q, {1, 0, 0}), c1 = Rotate(q, {0, 1, 0}), c2 = Rotate(q, {0, 0, 1});
    // Matrix entries R[row][col] = column `col`'s component `row`.
    const float r12 = c2.y, r02 = c2.x, r22 = c2.z, r10 = c0.y, r11 = c1.y, r20 = c0.z, r00 = c0.x;
    float pitch, yaw, roll;
    pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, -r12)));
    if (std::fabs(r12) < 0.99999f) {
        yaw = std::atan2(r02, r22);
        roll = std::atan2(r10, r11);
    } else { // looking straight up or down: roll and yaw are the same rotation, fold it into yaw
        yaw = std::atan2(-r20, r00);
        roll = 0.0f;
    }
    return {Degrees(pitch), Degrees(yaw), Degrees(roll)};
}

// Model matrix: translate * rotate * scale.
inline Mat4 TRS(Vec3 t, Quat q, Vec3 s) {
    Mat4 m = Mat4::Identity();
    const Vec3 c0 = Rotate(q, {1, 0, 0}) * s.x, c1 = Rotate(q, {0, 1, 0}) * s.y, c2 = Rotate(q, {0, 0, 1}) * s.z;
    m.At(0, 0) = c0.x; m.At(1, 0) = c0.y; m.At(2, 0) = c0.z;
    m.At(0, 1) = c1.x; m.At(1, 1) = c1.y; m.At(2, 1) = c1.z;
    m.At(0, 2) = c2.x; m.At(1, 2) = c2.y; m.At(2, 2) = c2.z;
    m.At(0, 3) = t.x;  m.At(1, 3) = t.y;  m.At(2, 3) = t.z;
    return m;
}

// General 4x4 inverse (cofactors); returns identity for a singular matrix.
inline Mat4 Inverse(const Mat4& a) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return Mat4::Identity();
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
    return r;
}

struct Ray {
    Vec3 origin, dir; // dir is unit length
};

} // namespace tbd
