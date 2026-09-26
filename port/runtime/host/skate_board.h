// Skateboard mod: the board as geometry. Its pose in the world, the generated mesh (a deck, two
// trucks, four wheels), and projecting it through the game's camera onto the screen.
//
// Melee's world: x to the right, y up, z toward the camera; a fighter faces +x or -x. The board's own
// frame: x along its length (the nose points the way the fighter faces), y out of the grip tape,
// z across. Pure math, no guest memory and no ImGui, so tests/skate_core_test.cpp runs it anywhere.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>

#include "skate_tunables.h"

namespace skate {

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 a);
Vec3 normalize(Vec3 a, Vec3 fallback = {0, 1, 0});

// Rotation and position: columns are the board's x, y, z axes in the world.
struct Pose {
  Vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
  Vec3 origin{};
  Vec3 apply(Vec3 local) const { return origin + x * local.x + y * local.y + z * local.z; }
  Vec3 rotate(Vec3 local) const { return x * local.x + y * local.y + z * local.z; }
};
// a then b: b's frame expressed in a's (world = a.apply(b.apply(p))).
Pose compose(const Pose& a, const Pose& b);
// Blends two poses (for the catch snap): position linearly, axes linearly then re-orthonormalised.
Pose blend(const Pose& a, const Pose& b, float t);

// The resting board under a grounded fighter: at the ground point, nose toward `facing`, lying on the
// floor whose normal is (nx, ny) (the fighter's floor normal, fp+0x844).
Pose rest_pose(Vec3 ground, float facing, float nx, float ny, const Tunables& t);
// Under the feet in the air: `feet` is where the feet are (their midpoint), world up.
Pose air_pose(Vec3 feet, float facing, const Tunables& t);

// ---- mesh
enum class Part : uint8_t { Deck, Grip, Truck, Wheel };
struct Box { Vec3 center, half; Part part; };
// The board in its own frame, sized by the tunables. Centred on the deck.
std::vector<Box> board_mesh(const Tunables& t);

// ---- camera and projection
struct Camera {
  float view[3][4] = {};   // HSD_CObj view matrix (world -> camera, camera looks down -z)
  float fov_deg = 30.0f;   // vertical field of view
  float aspect = 1.0f;     // width / height the game's projection uses
  float near_z = 1.0f;
  bool valid = false;
};
struct Rect { float x = 0, y = 0, w = 0, h = 0; };
// Where the game's picture sits in a window of this size when shown at `aspect` (letterboxed).
Rect image_rect(float window_w, float window_h, float aspect);

struct Projected { float x, y, depth; bool ok; };
Projected project(const Camera& cam, const Rect& r, Vec3 world);

// One filled face ready to draw, in screen pixels, sorted back to front by the caller of build_faces.
struct Face {
  float px[4], py[4];
  float depth;
  float shade;    // 0..1 light
  Part part;
  int owner;      // which board it belongs to (the caller's index)
};
// Appends the visible faces of a board at `pose` (back faces dropped).
void append_board_faces(std::vector<Face>& out, const std::vector<Box>& mesh, const Pose& pose, const Camera& cam,
                        const Rect& r, int owner);
// Back to front, so drawing in order paints the nearest faces last (there is no depth buffer).
void sort_faces(std::vector<Face>& faces);

}  // namespace skate
