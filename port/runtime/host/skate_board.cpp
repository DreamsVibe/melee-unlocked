// SPDX-License-Identifier: GPL-2.0-or-later
#include "skate_board.h"

#include <algorithm>
#include <cmath>

namespace skate {

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a, Vec3 fallback) {
  const float l = length(a);
  return l > 1e-6f ? a * (1.0f / l) : fallback;
}

Pose compose(const Pose& a, const Pose& b) {
  Pose r;
  r.x = a.rotate(b.x);
  r.y = a.rotate(b.y);
  r.z = a.rotate(b.z);
  r.origin = a.apply(b.origin);
  return r;
}

Pose blend(const Pose& a, const Pose& b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  Pose r;
  r.origin = a.origin + (b.origin - a.origin) * t;
  const Vec3 x = normalize(a.x + (b.x - a.x) * t, b.x);
  Vec3 y = a.y + (b.y - a.y) * t;
  y = normalize(y - x * dot(y, x), b.y);   // Gram-Schmidt keeps it a rotation
  r.x = x;
  r.y = y;
  r.z = cross(x, y);
  return r;
}

Pose rest_pose(Vec3 ground, float facing, float nx, float ny, const Tunables& t) {
  const float f = facing < 0.0f ? -1.0f : 1.0f;
  Pose p;
  p.y = normalize(Vec3{nx, ny, 0.0f});
  // The floor's tangent, the same one the game moves along (self_vel = (ny, -nx) * gr_vel).
  p.x = normalize(Vec3{p.y.y, -p.y.x, 0.0f} * f, Vec3{f, 0, 0});
  p.z = cross(p.x, p.y);
  p.origin = ground + p.y * (t.wheel_size + t.foot_height + t.board_thickness * 0.5f);
  return p;
}

Pose air_pose(Vec3 feet, float facing, const Tunables& t) {
  const float f = facing < 0.0f ? -1.0f : 1.0f;
  Pose p;
  p.x = {f, 0, 0};
  p.y = {0, 1, 0};
  p.z = cross(p.x, p.y);
  // The foot joints sit a little above the soles.
  p.origin = feet - p.y * (t.board_thickness * 0.5f + 0.4f - t.foot_height);
  return p;
}

std::vector<Box> board_mesh(const Tunables& t) {
  const float L = t.board_length, W = t.board_width, T = t.board_thickness, w = t.wheel_size;
  std::vector<Box> boxes;
  boxes.push_back({{0, 0, 0}, {L * 0.5f, T * 0.5f, W * 0.5f}, Part::Deck});
  if (w > 0.0f) {
    const float axle_x = L * 0.34f;
    for (float sx : {-1.0f, 1.0f}) {
      boxes.push_back({{sx * axle_x, -T * 0.5f - w * 0.2f, 0}, {w * 0.18f, w * 0.2f, W * 0.38f}, Part::Truck});
      for (float sz : {-1.0f, 1.0f})
        boxes.push_back({{sx * axle_x, -T * 0.5f - w * 0.5f, sz * (W * 0.5f - w * 0.3f)}, {w * 0.5f, w * 0.5f, w * 0.28f}, Part::Wheel});
    }
  }
  return boxes;
}

Rect image_rect(float window_w, float window_h, float aspect) {
  Rect r;
  if (window_w <= 0 || window_h <= 0 || aspect <= 0) return r;
  if (window_w / window_h > aspect) { r.h = window_h; r.w = window_h * aspect; r.x = (window_w - r.w) * 0.5f; }
  else { r.w = window_w; r.h = window_w / aspect; r.y = (window_h - r.h) * 0.5f; }
  return r;
}

namespace {
Vec3 to_view(const Camera& c, Vec3 p) {
  return {c.view[0][0] * p.x + c.view[0][1] * p.y + c.view[0][2] * p.z + c.view[0][3],
          c.view[1][0] * p.x + c.view[1][1] * p.y + c.view[1][2] * p.z + c.view[1][3],
          c.view[2][0] * p.x + c.view[2][1] * p.y + c.view[2][2] * p.z + c.view[2][3]};
}
Vec3 to_view_dir(const Camera& c, Vec3 d) {
  return {c.view[0][0] * d.x + c.view[0][1] * d.y + c.view[0][2] * d.z,
          c.view[1][0] * d.x + c.view[1][1] * d.y + c.view[1][2] * d.z,
          c.view[2][0] * d.x + c.view[2][1] * d.y + c.view[2][2] * d.z};
}
}  // namespace

Projected project(const Camera& cam, const Rect& r, Vec3 world) {
  const Vec3 v = to_view(cam, world);
  const float depth = -v.z;
  if (!cam.valid || depth < std::max(cam.near_z, 0.01f)) return {0, 0, depth, false};
  const float f = 1.0f / std::tan(cam.fov_deg * 0.5f * 3.14159265f / 180.0f);
  const float nx = v.x * f / (cam.aspect * depth);
  const float ny = v.y * f / depth;
  return {r.x + (nx * 0.5f + 0.5f) * r.w, r.y + (0.5f - ny * 0.5f) * r.h, depth, true};
}

void append_board_faces(std::vector<Face>& out, const std::vector<Box>& mesh, const Pose& pose, const Camera& cam,
                        const Rect& r, int owner) {
  const Vec3 light = normalize(Vec3{0.35f, 0.85f, 0.4f});
  for (const Box& b : mesh) {
    for (int axis = 0; axis < 3; ++axis) {
      for (float s : {-1.0f, 1.0f}) {
        // The face's four corners in order around it, in the box's (axis, u, v) coordinates.
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const float cu[4] = {-1, 1, 1, -1}, cv[4] = {-1, -1, 1, 1};
        Vec3 n_local{};
        (&n_local.x)[axis] = s;
        const Vec3 n_world = pose.rotate(n_local);
        const Vec3 c_world = pose.apply(b.center + Vec3{n_local.x * b.half.x, n_local.y * b.half.y, n_local.z * b.half.z});
        // Facing away from the camera (which sits at the view-space origin): not drawn.
        if (dot(to_view_dir(cam, n_world), to_view(cam, c_world) * -1.0f) <= 0.0f) continue;
        Face f{};
        bool ok = true;
        float depth = 0.0f;
        for (int k = 0; k < 4; ++k) {
          Vec3 local = b.center;
          (&local.x)[axis] += s * (&b.half.x)[axis];
          (&local.x)[u] += cu[k] * (&b.half.x)[u];
          (&local.x)[v] += cv[k] * (&b.half.x)[v];
          const Projected p = project(cam, r, pose.apply(local));
          if (!p.ok) { ok = false; break; }
          f.px[k] = p.x;
          f.py[k] = p.y;
          depth += p.depth;
        }
        if (!ok) continue;
        f.depth = depth * 0.25f;
        f.shade = 0.45f + 0.55f * std::max(0.0f, dot(n_world, light));
        f.part = b.part == Part::Deck && axis == 1 && s > 0 ? Part::Grip : b.part;
        f.owner = owner;
        out.push_back(f);
      }
    }
  }
}

void sort_faces(std::vector<Face>& faces) {
  std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth > b.depth; });
}

}  // namespace skate
