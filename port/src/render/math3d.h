#pragma once
#include <cmath>

namespace regs::render {

// Column-major 4x4 matrices and column vectors, as OpenGL takes them (m[col*4 + row]).
struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(const Vec3& a) {
  float l = std::sqrt(dot(a, a));
  return l > 0 ? a * (1.0f / l) : a;
}

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
// q rotates v (standard Hamilton convention: v' = q v q*)
inline Vec3 rotate(const Quat& q, const Vec3& v) {
  Vec3 u{q.x, q.y, q.z};
  Vec3 t = cross(u, v) * 2.0f;
  return v + t * q.w + cross(u, t);
}
inline Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.at(rr, k) * b.at(k, c);
      r.at(rr, c) = s;
    }
  return r;
}

inline Mat4 translation(const Vec3& t) {
  Mat4 r;
  r.at(0, 3) = t.x;
  r.at(1, 3) = t.y;
  r.at(2, 3) = t.z;
  return r;
}

inline Mat4 scaling(float s) {
  Mat4 r;
  r.at(0, 0) = r.at(1, 1) = r.at(2, 2) = s;
  return r;
}

// rotation matrix of q (column vectors: R·v = rotate(q, v))
inline Mat4 rotation(const Quat& q) {
  Mat4 r;
  float x = q.x, y = q.y, z = q.z, w = q.w;
  r.at(0, 0) = 1 - 2 * (y * y + z * z); r.at(0, 1) = 2 * (x * y - z * w);     r.at(0, 2) = 2 * (x * z + y * w);
  r.at(1, 0) = 2 * (x * y + z * w);     r.at(1, 1) = 1 - 2 * (x * x + z * z); r.at(1, 2) = 2 * (y * z - x * w);
  r.at(2, 0) = 2 * (x * z - y * w);     r.at(2, 1) = 2 * (y * z + x * w);     r.at(2, 2) = 1 - 2 * (x * x + y * y);
  return r;
}

// gluPerspective (fov in degrees, GL depth range [-1, 1])
inline Mat4 perspective(float fovyDeg, float aspect, float zNear, float zFar) {
  Mat4 r;
  float f = 1.0f / std::tan(fovyDeg * 3.14159265358979f / 360.0f);
  r.at(0, 0) = f / aspect;
  r.at(1, 1) = f;
  r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
  r.at(2, 3) = 2 * zFar * zNear / (zNear - zFar);
  r.at(3, 2) = -1;
  r.at(3, 3) = 0;
  return r;
}

// gluLookAt
inline Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
  Vec3 z = normalize(eye - target), x = normalize(cross(up, z)), y = cross(z, x);
  Mat4 r;
  r.at(0, 0) = x.x; r.at(0, 1) = x.y; r.at(0, 2) = x.z; r.at(0, 3) = -dot(x, eye);
  r.at(1, 0) = y.x; r.at(1, 1) = y.y; r.at(1, 2) = y.z; r.at(1, 3) = -dot(y, eye);
  r.at(2, 0) = z.x; r.at(2, 1) = z.y; r.at(2, 2) = z.z; r.at(2, 3) = -dot(z, eye);
  return r;
}

// inverse of a rotation + translation (a view matrix): transposed rotation, translation −Rᵀ·t
inline Mat4 inverseRigid(const Mat4& a) {
  Mat4 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.at(i, j) = a.at(j, i);
  for (int i = 0; i < 3; ++i) r.at(i, 3) = -(r.at(i, 0) * a.at(0, 3) + r.at(i, 1) * a.at(1, 3) + r.at(i, 2) * a.at(2, 3));
  return r;
}

inline Vec3 transformPoint(const Mat4& a, const Vec3& p) {
  return {a.at(0, 0) * p.x + a.at(0, 1) * p.y + a.at(0, 2) * p.z + a.at(0, 3),
          a.at(1, 0) * p.x + a.at(1, 1) * p.y + a.at(1, 2) * p.z + a.at(1, 3),
          a.at(2, 0) * p.x + a.at(2, 1) * p.y + a.at(2, 2) * p.z + a.at(2, 3)};
}

// upper-left 3x3 of the inverse transpose (normal matrix), column-major
inline void normalMatrix(const Mat4& a, float out[9]) {
  float m00 = a.at(0, 0), m01 = a.at(0, 1), m02 = a.at(0, 2);
  float m10 = a.at(1, 0), m11 = a.at(1, 1), m12 = a.at(1, 2);
  float m20 = a.at(2, 0), m21 = a.at(2, 1), m22 = a.at(2, 2);
  float c00 = m11 * m22 - m12 * m21, c01 = m12 * m20 - m10 * m22, c02 = m10 * m21 - m11 * m20;
  float det = m00 * c00 + m01 * c01 + m02 * c02;
  float id = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
  // inverse transpose = cofactor matrix / det
  float c10 = m02 * m21 - m01 * m22, c11 = m00 * m22 - m02 * m20, c12 = m01 * m20 - m00 * m21;
  float c20 = m01 * m12 - m02 * m11, c21 = m02 * m10 - m00 * m12, c22 = m00 * m11 - m01 * m10;
  // out[col*3 + row] = cof[row][col] / det
  out[0] = c00 * id; out[3] = c01 * id; out[6] = c02 * id;
  out[1] = c10 * id; out[4] = c11 * id; out[7] = c12 * id;
  out[2] = c20 * id; out[5] = c21 * id; out[8] = c22 * id;
}

}  // namespace regs::render
