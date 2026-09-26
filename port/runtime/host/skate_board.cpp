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

// ---------------- tricks
namespace {
constexpr float kPi = 3.14159265f;

float smooth(float s) { s = std::clamp(s, 0.0f, 1.0f); return s * s * (3.0f - 2.0f * s); }
float ease_out(float s) { s = std::clamp(s, 0.0f, 1.0f); return 1.0f - (1.0f - s) * (1.0f - s); }

// Rotations in the board's own frame: roll about its length (x), yaw about its up (y), pitch about
// its width (z).
Pose rot_x(float a) { Pose p; const float c = std::cos(a), n = std::sin(a); p.y = {0, c, n}; p.z = {0, -n, c}; return p; }
Pose rot_y(float a) { Pose p; const float c = std::cos(a), n = std::sin(a); p.x = {c, 0, -n}; p.z = {n, 0, c}; return p; }
Pose rot_z(float a) { Pose p; const float c = std::cos(a), n = std::sin(a); p.x = {c, n, 0}; p.y = {-n, c, 0}; return p; }
Pose offset(Vec3 o) { Pose p; p.origin = o; return p; }
}  // namespace

Pose trick_primitive(Trick trick, float s, float direction, float rotations, const Tunables& t) {
  const float d = direction < 0.0f ? -1.0f : 1.0f;
  const float e = smooth(s);
  const float turn = 2.0f * kPi * rotations * e;
  switch (trick) {
    case Trick::Kickflip: return rot_x(d * turn);
    case Trick::Heelflip: return rot_x(-d * turn);
    case Trick::Shoveit: return rot_y(d * turn);
    case Trick::Varial: return compose(rot_y(d * 0.5f * turn), rot_x(d * turn));   // a 360 flip with a 180 shove
    case Trick::Impossible: {
      // Wraps end over end around the back foot: a pitch about the tail, not the centre.
      const Vec3 pivot{-t.board_length * 0.5f, 0, 0};
      Pose r = rot_z(d * turn);
      r.origin = pivot - r.rotate(pivot);
      return r;
    }
    case Trick::Stomp: {
      // A quick drop under the feet and a slam, nose first.
      const float bump = std::sin(kPi * std::clamp(s, 0.0f, 1.0f));
      return compose(offset(Vec3{0, -t.pop_height * 0.9f * bump, 0}), rot_z(-0.45f * bump));
    }
    case Trick::Grab:
      // Stays at the feet and tilts toward a hand.
      return rot_x(d * 0.55f * std::sin(kPi * std::clamp(s, 0.0f, 1.0f)));
  }
  return Pose{};
}

const char* phase_name(Phase p) { return p == Phase::Pop ? "pop" : p == Phase::Flip ? "flip" : "catch"; }

TrickTime trick_time(float u, const ActiveWindow& w) {
  const float a0 = std::clamp(w.start, 0.0f, 0.98f), a1 = std::clamp(w.end, a0 + 0.01f, 1.0f);
  if (u < a0) return {Phase::Pop, a0 > 0.0f ? u / a0 : 1.0f};
  if (u < a1) return {Phase::Flip, (u - a0) / (a1 - a0)};
  return {Phase::Catch, a1 < 1.0f ? std::clamp((u - a1) / (1.0f - a1), 0.0f, 1.0f) : 1.0f};
}

Pose trick_pose(const TrickRow& row, float u, const ActiveWindow& w, const Tunables& t) {
  const TrickTime tt = trick_time(u, w);
  float lift = t.pop_height;
  Pose spin;   // identity: at rest and once caught (every whole or half turn looks the same)
  switch (tt.phase) {
    case Phase::Pop: lift = t.pop_height * ease_out(tt.s); break;
    case Phase::Flip: spin = trick_primitive(row.trick, tt.s, row.direction, row.rotations, t); break;
    case Phase::Catch: lift = t.pop_height * (1.0f - smooth(tt.s)); break;
  }
  return compose(offset(Vec3{0, lift, 0}), spin);
}

// ---------------- trick sampler
namespace {
// The rotation part of a joint matrix with its scale taken out (columns normalised).
void rotation_of(const float m[3][4], float r[3][3]) {
  for (int col = 0; col < 3; ++col) {
    const float l = std::sqrt(m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col]);
    const float k = l > 1e-6f ? 1.0f / l : 0.0f;
    for (int row = 0; row < 3; ++row) r[row][col] = m[row][col] * k;
  }
}
}  // namespace

void TrickSampler::begin(float facing) {
  *this = TrickSampler{};
  facing_ = facing < 0.0f ? -1.0f : 1.0f;
}

void TrickSampler::add(const float hip[3][4], Vec3 feet) {
  float r[3][3];
  rotation_of(hip, r);
  const float feet_rel = feet.y - hip[1][3];
  if (samples_ == 0) {
    feet_rel0_ = feet_rel;
  } else {
    // d = r * prev^T: this frame's turn, as an axis and angle.
    float d[3][3];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) d[i][j] = r[i][0] * prev_[j][0] + r[i][1] * prev_[j][1] + r[i][2] * prev_[j][2];
    const float c = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    const float angle = std::acos(c);
    if (angle > 1e-4f) {
      Vec3 axis{d[2][1] - d[1][2], d[0][2] - d[2][0], d[1][0] - d[0][1]};
      axis = normalize(axis, Vec3{0, 0, 0});
      w_ = w_ + axis * angle;
    }
    drop_ = std::max(drop_, feet_rel0_ - feet_rel);   // how far the feet came down toward/past the hip
  }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) prev_[i][j] = r[i][j];
  ++samples_;
}

float TrickSampler::total_angle() const { return length(w_); }

TrickRow TrickSampler::classify(float board_length) const {
  TrickRow row;
  row.source = Source::Auto;
  const float forward = w_.x * facing_, up = w_.y, side = w_.z;
  const float total = total_angle();
  if (total < 1.05f) {   // under 60 degrees: the body barely turns
    row.trick = drop_ > board_length * 0.35f ? Trick::Stomp : Trick::Grab;
    row.direction = forward >= 0.0f ? 1.0f : -1.0f;
    row.rotations = 1.0f;
    return row;
  }
  const float af = std::fabs(forward), au = std::fabs(up), as = std::fabs(side);
  if (af >= au && af >= as) {
    row.trick = Trick::Varial;
    row.direction = forward >= 0.0f ? 1.0f : -1.0f;
    row.rotations = std::max(1.0f, std::round(af / (2.0f * kPi)));
  } else if (au >= as) {
    row.trick = Trick::Shoveit;
    row.direction = up >= 0.0f ? 1.0f : -1.0f;
    row.rotations = std::max(0.5f, std::round(au / kPi) * 0.5f);
  } else {
    // In the screen's plane. Facing +x, a turn about +z lifts the nose: a back flip. A front flip
    // turns the other way, and a front flip is a kickflip.
    const bool front = side * facing_ < 0.0f;
    row.trick = front ? Trick::Kickflip : Trick::Heelflip;
    row.direction = 1.0f;
    row.rotations = std::max(1.0f, std::round(as / (2.0f * kPi)));
  }
  return row;
}

Pose stumble_pose(int frame, float direction) {
  // Kicks out sideways and wobbles back as the stumble plays out.
  const float f = (float)std::max(frame, 0);
  const float decay = std::exp(-f / 10.0f);
  const float d = direction < 0.0f ? -1.0f : 1.0f;
  return compose(rot_y(d * 0.6f * decay * std::cos(f * 0.35f)), rot_x(0.18f * decay * std::sin(f * 0.8f)));
}

}  // namespace skate
