#pragma once
#include <cmath>

// Column-major storage, column vectors: clip = projection * view * model * p.
struct Mat4 {
    float v[16]{};
    static Mat4 identity() {
        Mat4 m;
        m.v[0] = m.v[5] = m.v[10] = m.v[15] = 1.0f;
        return m;
    }
};
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                out.v[c * 4 + r] += a.v[k * 4 + r] * b.v[c * 4 + k];
    return out;
}
inline Mat4 rotationX(float a) {
    Mat4 m = Mat4::identity();
    m.v[5] = m.v[10] = std::cos(a);
    m.v[6] = std::sin(a); m.v[9] = -std::sin(a);
    return m;
}
inline Mat4 rotationY(float a) {
    Mat4 m = Mat4::identity();
    m.v[0] = m.v[10] = std::cos(a);
    m.v[2] = -std::sin(a); m.v[8] = std::sin(a);
    return m;
}
inline Mat4 translationZ(float z) {
    Mat4 m = Mat4::identity(); m.v[14] = z; return m;
}
inline Mat4 perspective(float aspect, float nearPlane = 0.1f, float farPlane = 100.0f) {
    const float f = 1.0f / std::tan(0.785398163f * 0.5f); // 45 degree vertical FOV
    Mat4 m;
    m.v[0] = f / aspect;
    m.v[5] = -f; // Vulkan viewport +Y points down. Flip projection Y.
    // Right-handed view space, Vulkan depth range [0,1], camera looks toward -Z.
    m.v[10] = farPlane / (nearPlane - farPlane);
    m.v[11] = -1.0f;
    m.v[14] = nearPlane * farPlane / (nearPlane - farPlane);
    return m;
}
