// Skateboard mod: the parts that are pure arithmetic, checked without the game.
//
// Covers the spec's acceptance items that do not need a match: mounting keeps a dash's speed and a
// standstill stays still, rolling never goes faster than you came in, holding back brakes at the
// character's normal friction, a missed L-cancel keeps friction vanilla, the trick primitives and
// their timing, the bone-sampling classifier, both data tables' JSON, and the camera projection.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <string>

#include "skate_board.h"
#include "skate_data.h"
#include "skate_rules.h"
#include "skate_tunables.h"

using namespace skate;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool near(Vec3 a, Vec3 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

// Runs the momentum rule for `frames` frames of one state, the way skate.cpp does each frame.
static float roll(Group g, float carried, float stick, float game_vel_each_frame, int frames, const Tunables& t,
                  float friction = 0.08f, float walk_max = 1.6f) {
  float entry = std::fabs(carried);
  for (int i = 0; i < frames; ++i) {
    Frame f;
    f.group = g;
    f.stick_x = stick;
    f.carried = carried;
    f.base_friction = friction;
    f.walk_max = walk_max;
    const Mode m = pick_mode(f, t);
    const Move mv = board_move(m, f, std::isnan(game_vel_each_frame) ? carried : game_vel_each_frame, entry, t);
    carried = mv.carried;
  }
  return carried;
}

static void test_tunables() {
  Tunables t;
  CHECK(t.mount_frames == 3 && t.skate_friction == 0.0f && t.stumble_extra_frames == 8);
  CHECK(t.slide_ground_attacks && t.airdodge_land_friction < 0.0f && !t.skate_push);
  std::string err, warn;
  CHECK(parse_tunables(R"({"mount_frames": 5, "slide_ground_attacks": "off", "airdodge_land_friction": 0.02, "bogus": 1})", t, err, warn));
  CHECK(t.mount_frames == 5 && !t.slide_ground_attacks && near(t.airdodge_land_friction, 0.02f));
  CHECK(warn.find("bogus") != std::string::npos);
  CHECK(parse_tunables(R"({"airdodge_land_friction": "vanilla", "mount_frames": 999})", t, err, warn));
  CHECK(t.airdodge_land_friction < 0.0f && t.mount_frames == 60);   // clamped
  Tunables before = t;
  CHECK(!parse_tunables("not json", t, err, warn));
  CHECK(t.mount_frames == before.mount_frames);
  Tunables round;
  CHECK(parse_tunables(tunables_to_json(t), round, err, warn) && round.mount_frames == t.mount_frames && warn.empty());
}

static void test_rules() {
  const Tunables t;
  // Mount from: standing, walking, dashing, running, crouching, waveland, landing. Grounded only.
  for (int m : {ms::Wait, ms::WalkSlow, ms::Dash, ms::Run, ms::Squat, ms::LandingFallSpecial, ms::Landing, ms::LandingAirN})
    CHECK(can_mount(m, true));
  CHECK(!can_mount(ms::Wait, false));
  CHECK(!can_mount(ms::GuardOn, true) && !can_mount(ms::GroundAttackFirst, true));
  CHECK(drops_board(classify(ms::DamageFirst)) && drops_board(classify(ms::CliffFirst)) && drops_board(classify(ms::CaptureFirst)));
  CHECK(drops_board(classify(0)) && !drops_board(classify(ms::GuardOn)) && !drops_board(classify(ms::Wait)));
  CHECK(classify(400) == Group::Attack);   // a character's special

  // Mounting from a dash keeps the dash speed; standing still stays still.
  CHECK(near(roll(Group::Locomotion, 1.9f, 0.0f, NAN, 120, t), 1.9f));
  CHECK(near(roll(Group::Locomotion, 0.0f, 1.0f, 2.2f, 60, t), 0.0f));          // stick forward: no acceleration
  // Rolling speed never goes above what the character entered with, whatever the state computes.
  CHECK(near(roll(Group::Locomotion, 1.2f, 1.0f, 2.5f, 60, t), 1.2f));          // dash/run trying to add speed
  CHECK(near(roll(Group::Locomotion, 1.2f, 0.0f, -2.0f, 10, t), 1.2f));         // dash back trying to flip it
  CHECK(near(roll(Group::Attack, 1.2f, 0.0f, 3.0f, 10, t), 1.2f));             // dash attack: kept only at entry speed
  CHECK(near(roll(Group::Attack, 1.2f, 0.0f, 0.4f, 10, t), 0.4f));             // an attack that stops you does
  // Holding back brakes at the character's normal deceleration.
  CHECK(near(roll(Group::Locomotion, 1.0f, -1.0f, NAN, 5, t, 0.08f), 1.0f - 5 * 0.08f));
  CHECK(near(roll(Group::Locomotion, 1.0f, -1.0f, NAN, 100, t, 0.08f), 0.0f));  // and stops, never reverses
  CHECK(near(roll(Group::Locomotion, -1.0f, 1.0f, NAN, 5, t, 0.08f), -1.0f + 5 * 0.08f));
  CHECK(near(roll(Group::Locomotion, 1.0f, -0.2f, NAN, 5, t, 0.08f), 1.0f));   // inside the dead zone: no brake

  // Friction written for each mode.
  CHECK(friction_for(Mode::Roll, Group::Locomotion, 0.08f, t) == 0.0f);
  CHECK(near(friction_for(Mode::Brake, Group::Locomotion, 0.08f, t), 0.08f));
  CHECK(near(friction_for(Mode::Vanilla, Group::Waveland, 0.08f, t), 0.08f));   // "vanilla"
  Tunables t2 = t;
  t2.airdodge_land_friction = 0.01f;
  CHECK(near(friction_for(Mode::Vanilla, Group::Waveland, 0.08f, t2), 0.01f));
  CHECK(friction_for(Mode::Free, Group::Attack, 0.08f, t) == 0.0f);
  // A missed L-cancel: vanilla friction for the landing, still never faster.
  Frame f;
  f.group = Group::LandingAir;
  f.stumbling = true;
  f.carried = 1.0f;
  f.base_friction = 0.08f;
  CHECK(pick_mode(f, t) == Mode::Vanilla);
  CHECK(near(board_move(Mode::Vanilla, f, 0.92f, 1.0f, t).carried, 0.92f));
  CHECK(near(board_move(Mode::Vanilla, f, 1.5f, 1.0f, t).carried, 1.0f));
  f.stumbling = false;
  CHECK(pick_mode(f, t) == Mode::Roll);   // clean catch keeps rolling
  // Waveland: vanilla wavedash slide.
  f.group = Group::Waveland;
  CHECK(pick_mode(f, t) == Mode::Vanilla);
  // slide_ground_attacks off: attacks play fully vanilla.
  t2.slide_ground_attacks = false;
  f.group = Group::Attack;
  CHECK(pick_mode(f, t2) == Mode::PassThrough && near(friction_for(Mode::PassThrough, Group::Attack, 0.08f, t2), 0.08f));
  // Optional push: up to walk speed, never above.
  Tunables tp = t;
  tp.skate_push = true;
  CHECK(near(roll(Group::Locomotion, 0.0f, 1.0f, NAN, 200, tp, 0.08f, 1.1f), 1.1f));
  CHECK(near(roll(Group::Locomotion, 1.5f, 1.0f, NAN, 50, tp, 0.08f, 1.1f), 1.5f));   // already faster: no push
}

static void test_tricks() {
  const Tunables t;
  // Every primitive starts at rest and every whole turn ends there.
  for (int i = 0; i < kTrickCount; ++i) {
    const Pose a = trick_primitive((Trick)i, 0.0f, 1.0f, 1.0f, t);
    CHECK(near(a.x, {1, 0, 0}) && near(a.y, {0, 1, 0}) && near(a.origin, {0, 0, 0}));
  }
  for (Trick k : {Trick::Kickflip, Trick::Heelflip, Trick::Impossible}) {
    const Pose b = trick_primitive(k, 1.0f, 1.0f, 1.0f, t);
    CHECK(near(b.x, {1, 0, 0}) && near(b.y, {0, 1, 0}) && near(b.origin, {0, 0, 0}));
  }
  // Half way through a kickflip the deck is upside down, rolled about its length.
  const Pose half = trick_primitive(Trick::Kickflip, 0.5f, 1.0f, 1.0f, t);
  CHECK(near(half.x, {1, 0, 0}) && near(half.y, {0, -1, 0}));
  // Heelflip is the kickflip the other way.
  const Pose k = trick_primitive(Trick::Kickflip, 0.3f, 1.0f, 1.0f, t), h = trick_primitive(Trick::Heelflip, 0.3f, 1.0f, 1.0f, t);
  CHECK(near(k.y.z, -h.y.z) && near(k.y.y, h.y.y));
  // The impossible turns about the tail: the tail does not move.
  const Pose imp = trick_primitive(Trick::Impossible, 0.37f, 1.0f, 1.0f, t);
  const Vec3 tail{-t.board_length * 0.5f, 0, 0};
  CHECK(near(imp.apply(tail), tail));
  // Shove-it 180: flat spin, the nose ends up where the tail was.
  const Pose shove = trick_primitive(Trick::Shoveit, 1.0f, 1.0f, 0.5f, t);
  CHECK(near(shove.x, {-1, 0, 0}) && near(shove.y, {0, 1, 0}));
  // Varial: flat half turn plus a full flip.
  const Pose var = trick_primitive(Trick::Varial, 1.0f, 1.0f, 1.0f, t);
  CHECK(near(var.x, {-1, 0, 0}) && near(var.y, {0, 1, 0}));

  // Timing: pop from frame 1, flip across the active frames, catch by the end.
  const ActiveWindow w{0.25f, 0.5f, true};
  CHECK(trick_time(0.0f, w).phase == Phase::Pop && trick_time(0.3f, w).phase == Phase::Flip && trick_time(0.8f, w).phase == Phase::Catch);
  CHECK(near(trick_time(0.375f, w).s, 0.5f));
  const TrickRow row{Trick::Kickflip, 1.0f, 1.0f, Source::Hand};
  CHECK(near(trick_pose(row, 0.0f, w, t).origin, {0, 0, 0}));                          // on the feet at frame 1
  CHECK(near(trick_pose(row, 0.25f, w, t).origin, {0, t.pop_height, 0}));             // popped by the active frames
  CHECK(near(trick_pose(row, 1.0f, w, t).origin, {0, 0, 0}) && near(trick_pose(row, 1.0f, w, t).y, {0, 1, 0}));   // caught

  // Stumble skid starts sideways and settles.
  const Pose sk0 = stumble_pose(0, 1.0f), sk60 = stumble_pose(60, 1.0f);
  CHECK(!near(sk0.x, {1, 0, 0}, 0.05f) && near(sk60.x, {1, 0, 0}, 0.02f));
}

static void test_sampler() {
  // A body spinning about the forward axis, 360 degrees over 10 frames: varial, its direction.
  auto spin = [](Vec3 axis, float total, float facing) {
    TrickSampler s;
    s.begin(facing);
    for (int i = 0; i <= 10; ++i) {
      const float a = total * (float)i / 10.0f;
      const float c = std::cos(a), n = std::sin(a), C = 1 - c;
      const Vec3 u = normalize(axis);
      float m[3][4] = {{c + u.x * u.x * C, u.x * u.y * C - u.z * n, u.x * u.z * C + u.y * n, 3.0f},
                       {u.y * u.x * C + u.z * n, c + u.y * u.y * C, u.y * u.z * C - u.x * n, 10.0f},
                       {u.z * u.x * C - u.y * n, u.z * u.y * C + u.x * n, c + u.z * u.z * C, 0.0f}};
      for (auto& r : m) for (int k = 0; k < 3; ++k) r[k] *= 1.3f;   // a scaled joint: the scale must not matter
      s.add(m, Vec3{3.0f, 5.0f, 0.0f});
    }
    return s.classify(7.0f);
  };
  const float two_pi = 6.2831853f;
  TrickRow r = spin({1, 0, 0}, two_pi * 0.999f, 1.0f);
  CHECK(r.trick == Trick::Varial && r.direction > 0 && near(r.rotations, 1.0f));
  r = spin({1, 0, 0}, -two_pi * 0.999f, 1.0f);
  CHECK(r.trick == Trick::Varial && r.direction < 0);
  r = spin({1, 0, 0}, two_pi * 0.999f, -1.0f);   // same world spin, facing left: the other way round for the board
  CHECK(r.trick == Trick::Varial && r.direction < 0);
  r = spin({0, 1, 0}, 3.1f, 1.0f);
  CHECK(r.trick == Trick::Shoveit && near(r.rotations, 0.5f));
  r = spin({0, 0, -1}, two_pi * 0.99f, 1.0f);    // facing +x, nose down: a front flip
  CHECK(r.trick == Trick::Kickflip);
  r = spin({0, 0, 1}, two_pi * 0.99f, 1.0f);
  CHECK(r.trick == Trick::Heelflip);
  r = spin({0, 1, 0}, 0.2f, 1.0f);
  CHECK(r.trick == Trick::Grab);
  // Feet driving down with no turn: stomp.
  TrickSampler s;
  s.begin(1.0f);
  float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 10}, {0, 0, 1, 0}};
  for (int i = 0; i < 6; ++i) s.add(m, Vec3{0, 6.0f - i * 1.0f, 0});
  CHECK(s.classify(7.0f).trick == Trick::Stomp);
}

static void test_tables() {
  TrickTable t;
  std::string err, warn;
  CHECK(t.from_json(R"({"rows":[{"character":"Falco","aerial":"fair","trick":"varial","direction":"auto","rotations":1,"source":"hand"},
                               {"character":"Fox","aerial":"nair","trick":"grab","source":"default"},
                               {"character":"Nobody","aerial":"nair","trick":"grab"}]})", err, warn));
  CHECK(t.size() == 2 && warn.find("unknown character") != std::string::npos);
  CHECK(t.get(22, 1).trick == Trick::Varial && t.get(22, 1).direction_auto && t.get(22, 1).source == Source::Hand);
  CHECK(t.get(0, 4).trick == Trick::Stomp);   // no row: the per-aerial default
  // The sampler fills default rows, then leaves them; a hand row only takes its direction.
  TrickRow sampled{Trick::Kickflip, -1.0f, 1.0f, Source::Auto};
  CHECK(t.learn(1, 0, sampled) && t.get(1, 0).trick == Trick::Kickflip && t.get(1, 0).source == Source::Auto);
  sampled.trick = Trick::Heelflip;
  CHECK(!t.learn(1, 0, sampled) && t.get(1, 0).trick == Trick::Kickflip);
  CHECK(t.learn(22, 1, sampled) && t.get(22, 1).trick == Trick::Varial && t.get(22, 1).direction < 0 && !t.get(22, 1).direction_auto);
  // Round trip.
  TrickTable back;
  CHECK(back.from_json(t.to_json(), err, warn) && back.size() == t.size() && back.get(22, 1).direction < 0 && back.get(22, 1).source == Source::Hand);
  // The reference row is added when a file lacks it, and a hand-set one is left alone.
  TrickTable empty;
  empty.ensure_reference();
  CHECK(empty.get(22, 1).trick == Trick::Varial && empty.get(22, 1).source == Source::Hand && near(empty.get(22, 1).rotations, 1.0f));
  CHECK(kind_from_name("falco") == 22 && kind_from_name("22") == 22 && aerial_from_name("dair") == 4);

  FrameDataTable fd;
  fd.observe(22, 1, 6, 22, 0);   // landed early: no length
  CHECK(fd.find(22, 1) == nullptr || fd.find(22, 1)->end >= 22);
  fd.observe(22, 1, 6, 24, 49);
  const FrameData* d = fd.find(22, 1);
  CHECK(d && d->first_active == 6 && d->last_active == 24 && d->end == 49 && d->samples == 2);
  const ActiveWindow w = active_window(fd, 22, 1, 0.2f, 0.55f);
  CHECK(w.observed && near(w.start, 5.0f / 49.0f) && near(w.end, 24.0f / 49.0f));
  CHECK(!active_window(fd, 1, 1, 0.2f, 0.55f).observed);
  FrameDataTable fd2;
  CHECK(fd2.from_json(fd.to_json(), err) && fd2.find(22, 1) && fd2.find(22, 1)->end == 49);
}

static void test_projection() {
  const Tunables t;
  Camera cam;
  // Camera at (0, 10, 100) looking down -z, no rotation: view = translate(0, -10, -100).
  cam.view[0][0] = cam.view[1][1] = cam.view[2][2] = 1.0f;
  cam.view[1][3] = -10.0f;
  cam.view[2][3] = -100.0f;
  cam.fov_deg = 30.0f;
  cam.aspect = 73.0f / 60.0f;
  cam.near_z = 1.0f;
  cam.valid = true;
  const Rect r = image_rect(1920, 1080, 73.0f / 60.0f);
  CHECK(near(r.h, 1080.0f) && near(r.x, (1920.0f - 1080.0f * 73.0f / 60.0f) * 0.5f));
  const Projected c = project(cam, r, {0, 10, 0});
  CHECK(c.ok && near(c.x, r.x + r.w * 0.5f, 0.01f) && near(c.y, r.y + r.h * 0.5f, 0.01f) && near(c.depth, 100.0f));
  const Projected up = project(cam, r, {0, 20, 0}), right = project(cam, r, {10, 10, 0});
  CHECK(up.ok && up.y < c.y && right.ok && right.x > c.x);   // world up is screen up, world right is screen right
  CHECK(!project(cam, r, {0, 10, 150}).ok);                   // behind the camera
  // A board on flat ground facing right: nose to screen right, deck faces drawn, wheels too.
  const Pose p = rest_pose({0, 0, 0}, 1.0f, 0.0f, 1.0f, t);
  CHECK(near(p.x, {1, 0, 0}) && near(p.y, {0, 1, 0}) && near(p.origin.y, t.wheel_size + t.board_thickness * 0.5f));
  const Pose pl = rest_pose({0, 0, 0}, -1.0f, 0.0f, 1.0f, t);
  CHECK(near(pl.x, {-1, 0, 0}) && near(pl.z, cross(pl.x, pl.y)));
  // On a slope the board lies along it.
  const Pose ps = rest_pose({0, 0, 0}, 1.0f, -0.28f, 0.96f, t);
  CHECK(near(dot(ps.x, ps.y), 0.0f) && ps.x.y > 0.2f);
  const std::vector<Box> mesh = board_mesh(t);
  CHECK(mesh.size() == 7);   // deck, 2 trucks, 4 wheels
  std::vector<Face> faces;
  append_board_faces(faces, mesh, p, cam, r, 0);
  CHECK(!faces.empty() && faces.size() <= 7 * 3);   // at most three faces of a box face a camera
  sort_faces(faces);
  CHECK(faces.front().depth >= faces.back().depth);
  bool grip = false;
  for (const Face& f : faces) grip |= f.part == Part::Grip;
  CHECK(grip);   // the camera is above the board: the grip tape shows
  // Blend: the catch snap stays a rotation.
  const Pose b = blend(p, compose(p, trick_primitive(Trick::Kickflip, 0.4f, 1, 1, t)), 0.5f);
  CHECK(near(length(b.x), 1.0f) && near(dot(b.x, b.y), 0.0f));
}

int main() {
  test_tunables();
  test_rules();
  test_tricks();
  test_sampler();
  test_tables();
  test_projection();
  std::printf("skate_core_test: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
