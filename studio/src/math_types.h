#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace cpm {

static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kDeg2Rad = kPi / 180.0f;
static constexpr float kRad2Deg = 180.0f / kPi;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    float dot(const Vec2& o) const { return x * o.x + y * o.y; }
    float len() const { return std::sqrt(x * x + y * y); }
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float lenSq() const { return x * x + y * y + z * z; }
    float len() const { return std::sqrt(lenSq()); }
    Vec3 normalized() const {
        float l = len();
        return l > 1e-8f ? (*this) * (1.0f / l) : Vec3(0, 1, 0);
    }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

// Column-major 4x4 matrix (m[col * 4 + row]), matching ImGuizmo directly.
// Translation lives in m[12], m[13], m[14].
struct Mat4 {
    float m[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    float& operator()(int row, int col) { return m[col * 4 + row]; }
    float operator()(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 Identity() { return Mat4(); }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }

    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }

    static Mat4 RotationX(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r(1, 1) = c;  r(1, 2) = -s;
        r(2, 1) = s;  r(2, 2) = c;
        return r;
    }

    static Mat4 RotationY(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r(0, 0) = c;  r(0, 2) = s;
        r(2, 0) = -s; r(2, 2) = c;
        return r;
    }

    static Mat4 RotationZ(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r(0, 0) = c; r(0, 1) = -s;
        r(1, 0) = s; r(1, 1) = c;
        return r;
    }

    static Mat4 FromEulerDeg(const Vec3& deg) {
        return RotationY(deg.y * kDeg2Rad) * RotationX(deg.x * kDeg2Rad) * RotationZ(deg.z * kDeg2Rad);
    }

    static Mat4 TRS(const Vec3& pos, const Vec3& eulerDeg, const Vec3& scale) {
        return Translation(pos) * FromEulerDeg(eulerDeg) * Scale(scale);
    }

    // Left-handed LookAt (Unity / DirectX 11 LH: +Z forward, +Y up, +X right)
    static Mat4 LookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 zaxis = (target - eye).normalized();
        Vec3 xaxis = up.cross(zaxis).normalized();
        Vec3 yaxis = zaxis.cross(xaxis);

        Mat4 r;
        r(0, 0) = xaxis.x; r(0, 1) = xaxis.y; r(0, 2) = xaxis.z; r(0, 3) = -xaxis.dot(eye);
        r(1, 0) = yaxis.x; r(1, 1) = yaxis.y; r(1, 2) = yaxis.z; r(1, 3) = -yaxis.dot(eye);
        r(2, 0) = zaxis.x; r(2, 1) = zaxis.y; r(2, 2) = zaxis.z; r(2, 3) = -zaxis.dot(eye);
        r(3, 0) = 0.0f;    r(3, 1) = 0.0f;    r(3, 2) = 0.0f;    r(3, 3) = 1.0f;
        return r;
    }

    // Left-handed Perspective for DirectX 11 (z_ndc in [0, 1])
    static Mat4 PerspectiveLH(float fovYRad, float aspect, float zn, float zf) {
        float yScale = 1.0f / std::tan(fovYRad * 0.5f);
        float xScale = yScale / std::max(0.01f, aspect);
        Mat4 r{};
        for (int i = 0; i < 16; ++i) r.m[i] = 0.0f;
        r(0, 0) = xScale;
        r(1, 1) = yScale;
        r(2, 2) = zf / (zf - zn);
        r(2, 3) = -(zn * zf) / (zf - zn);
        r(3, 2) = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                r(row, col) =
                    (*this)(row, 0) * b(0, col) +
                    (*this)(row, 1) * b(1, col) +
                    (*this)(row, 2) * b(2, col) +
                    (*this)(row, 3) * b(3, col);
            }
        }
        return r;
    }

    Vec4 mulVec4(const Vec4& v) const {
        return {
            (*this)(0, 0) * v.x + (*this)(0, 1) * v.y + (*this)(0, 2) * v.z + (*this)(0, 3) * v.w,
            (*this)(1, 0) * v.x + (*this)(1, 1) * v.y + (*this)(1, 2) * v.z + (*this)(1, 3) * v.w,
            (*this)(2, 0) * v.x + (*this)(2, 1) * v.y + (*this)(2, 2) * v.z + (*this)(2, 3) * v.w,
            (*this)(3, 0) * v.x + (*this)(3, 1) * v.y + (*this)(3, 2) * v.z + (*this)(3, 3) * v.w
        };
    }

    Vec3 transformPoint(const Vec3& p) const {
        Vec4 r = mulVec4(Vec4(p, 1.0f));
        return {r.x, r.y, r.z};
    }

    Vec3 transformDir(const Vec3& d) const {
        Vec4 r = mulVec4(Vec4(d, 0.0f));
        return {r.x, r.y, r.z};
    }

    Mat4 transposed() const {
        Mat4 r;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                r(row, col) = (*this)(col, row);
        return r;
    }

    Mat4 inverse() const {
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

        float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
        if (std::fabs(det) < 1e-12f) return Identity();
        det = 1.0f / det;
        Mat4 out;
        for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * det;
        return out;
    }
};

struct AABB {
    Vec3 bmin{1e30f, 1e30f, 1e30f};
    Vec3 bmax{-1e30f, -1e30f, -1e30f};
    void expand(const Vec3& p) {
        bmin.x = std::min(bmin.x, p.x); bmin.y = std::min(bmin.y, p.y); bmin.z = std::min(bmin.z, p.z);
        bmax.x = std::max(bmax.x, p.x); bmax.y = std::max(bmax.y, p.y); bmax.z = std::max(bmax.z, p.z);
    }
    void expand(const AABB& o) { expand(o.bmin); expand(o.bmax); }
    Vec3 center() const { return (bmin + bmax) * 0.5f; }
    Vec3 extent() const { return bmax - bmin; }
};

struct Ray {
    Vec3 origin;
    Vec3 dir;
};

} // namespace cpm
