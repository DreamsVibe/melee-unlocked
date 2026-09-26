// SPDX-License-Identifier: GPL-2.0-or-later
#include "skate_rules.h"

#include <cmath>

namespace skate {

Group classify(int m) {
  using namespace ms;
  if (m < 0) return Group::Other;
  if (m <= RebirthWait) return Group::Dead;
  if (m <= KneeBend) return Group::Locomotion;
  if (m <= AirLast) return Group::Air;
  if (m <= SquatRv) return Group::Crouch;
  if (m == Landing) return Group::Landing;
  if (m == LandingFallSpecial) return Group::Waveland;
  if (m <= GroundAttackLast) return Group::Attack;
  if (m <= AttackAirLw) return Group::Aerial;
  if (m <= LandingAirLw) return Group::LandingAir;
  if (m <= DamageLast) return Group::Hit;
  if (m <= ItemLast) return Group::Other;
  if (m <= GuardReflect) return Group::Shield;
  if (m <= Furafura) return Group::Hit;          // knockdowns, techs, shield break, dizzy
  if (m <= ThrowLast) return Group::Attack;      // grabs and throws
  if (m <= CaptureLast) return Group::Captured;
  if (m <= EscapeN) return Group::Escape;
  if (m == EscapeAir) return Group::Air;
  if (m <= Rebound) return Group::Other;
  if (m <= ThrownLast) return Group::Captured;
  if (m == Pass) return Group::Other;
  if (m <= OttottoWait) return Group::Locomotion;
  if (m <= MissFoot) return Group::Hit;          // wall/ceiling bounces, tripping
  if (m <= CliffLast) return Group::Cliff;
  if (m <= AppealSL) return Group::Locomotion;   // taunts
  if (m <= GrabbedLast) return Group::Captured;
  return Group::Attack;                          // character-specific: specials
}

const char* group_name(Group g) {
  switch (g) {
    case Group::Dead: return "dead";
    case Group::Locomotion: return "move";
    case Group::Crouch: return "crouch";
    case Group::Landing: return "landing";
    case Group::Waveland: return "waveland";
    case Group::LandingAir: return "landing-air";
    case Group::Aerial: return "aerial";
    case Group::Air: return "air";
    case Group::Attack: return "attack";
    case Group::Shield: return "shield";
    case Group::Escape: return "escape";
    case Group::Hit: return "hit";
    case Group::Captured: return "captured";
    case Group::Cliff: return "ledge";
    case Group::Other: return "other";
  }
  return "?";
}

const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Roll: return "rolling";
    case Mode::Brake: return "braking";
    case Mode::Push: return "pushing";
    case Mode::Vanilla: return "vanilla";
    case Mode::Free: return "free";
    case Mode::PassThrough: return "pass-through";
  }
  return "?";
}

bool can_mount(int motion, bool grounded) {
  if (!grounded) return false;
  switch (classify(motion)) {
    case Group::Locomotion: case Group::Crouch: case Group::Landing:
    case Group::Waveland: case Group::LandingAir:
      return true;
    default:
      return false;
  }
}

bool drops_board(Group g) {
  return g == Group::Dead || g == Group::Hit || g == Group::Captured || g == Group::Cliff;
}

bool is_aerial(int motion) { return motion >= ms::AttackAirN && motion <= ms::AttackAirLw; }

float approach(float v, float target, float step) {
  if (step <= 0.0f) return v;
  if (v < target) return v + step > target ? target : v + step;
  if (v > target) return v - step < target ? target : v - step;
  return v;
}

float no_gain(float v, float limit) {
  if (limit == 0.0f || v * limit < 0.0f) return 0.0f;
  return std::fabs(v) > std::fabs(limit) ? limit : v;
}

namespace {
float sign(float v) { return v > 0.0f ? 1.0f : v < 0.0f ? -1.0f : 0.0f; }

// Stick held against the direction of travel: the brake.
bool braking(const Frame& f, const Tunables& t) {
  return f.carried != 0.0f && f.stick_x * sign(f.carried) <= -t.brake_stick;
}

// Stick held in some direction from (nearly) stopped, with the optional push on.
bool pushing(const Frame& f, const Tunables& t) {
  if (!t.skate_push || std::fabs(f.stick_x) < t.brake_stick) return false;
  if (std::fabs(f.carried) >= f.walk_max) return false;
  return f.carried == 0.0f || sign(f.stick_x) == sign(f.carried);
}
}  // namespace

Mode pick_mode(const Frame& f, const Tunables& t) {
  if (f.stumbling) return Mode::Vanilla;
  switch (f.group) {
    case Group::Locomotion:
      if (braking(f, t)) return Mode::Brake;
      if (pushing(f, t)) return Mode::Push;
      return Mode::Roll;
    case Group::Crouch: case Group::Landing: case Group::LandingAir: case Group::Shield:
      return braking(f, t) ? Mode::Brake : Mode::Roll;
    case Group::Waveland:
      return Mode::Vanilla;
    case Group::Attack:
      return t.slide_ground_attacks ? Mode::Free : Mode::PassThrough;
    case Group::Escape: case Group::Other:
      return Mode::PassThrough;
    default:
      return Mode::Roll;   // airborne or dropping the board: nothing on the ground to decide
  }
}

float friction_for(Mode m, Group g, float base, const Tunables& t) {
  switch (m) {
    case Mode::Roll: case Mode::Push: case Mode::Free:
      return t.skate_friction;
    case Mode::Vanilla:
      return g == Group::Waveland && t.airdodge_land_friction >= 0.0f ? t.airdodge_land_friction : base;
    case Mode::Brake: case Mode::PassThrough:
      return base;
  }
  return base;
}

Move board_move(Mode m, const Frame& f, float game_vel, float entry_speed, const Tunables& t) {
  float v = 0.0f;
  switch (m) {
    case Mode::Roll:
      v = approach(f.carried, 0.0f, t.skate_friction);
      break;
    case Mode::Brake:
      v = approach(f.carried, 0.0f, f.base_friction);
      break;
    case Mode::Push: {
      v = approach(f.carried, sign(f.stick_x) * f.walk_max, f.base_friction);
      if (std::fabs(v) > f.walk_max) v = sign(v) * f.walk_max;
      break;
    }
    case Mode::Vanilla:
      v = no_gain(game_vel, f.carried);
      break;
    case Mode::Free: case Mode::PassThrough: {
      // The move itself is left exactly as the game plays it; only what the board keeps afterwards
      // is capped at the speed it came in with.
      const float keep = std::fabs(game_vel) > entry_speed ? sign(game_vel) * entry_speed : game_vel;
      return Move{game_vel, keep};
    }
  }
  if (std::fabs(v) < 1e-4f) v = 0.0f;
  return Move{v, v};
}

}  // namespace skate
