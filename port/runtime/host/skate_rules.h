// Skateboard mod: the rules, as pure functions of numbers read from the fighter.
//
// Which action states count as what (mountable, rolling, attack, hit...), and the momentum rule
// that decides the board's velocity each grounded frame. No guest memory here: skate.cpp reads the
// fighter, calls these, and writes the results back. tests/skate_core_test.cpp checks the spec's
// acceptance items that are pure arithmetic (dash speed kept, standing stays still, never faster
// than you came in, back brakes at normal deceleration).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

#include "skate_tunables.h"

namespace skate {

// ftCommon_MotionState values (decomp melee/ft/kinds/ftCommon/forward.h), NTSC 1.02.
namespace ms {
constexpr int DeadFirst = 0, Sleep = 11, Rebirth = 12, RebirthWait = 13;
constexpr int Wait = 14, WalkSlow = 15, WalkFast = 17, Turn = 18, TurnRun = 19, Dash = 20, Run = 21,
              RunDirect = 22, RunBrake = 23, KneeBend = 24;
constexpr int AirFirst = 25, AirLast = 38;                 // JumpF .. DamageFall
constexpr int Squat = 39, SquatRv = 41, Landing = 42, LandingFallSpecial = 43;
constexpr int GroundAttackFirst = 44, GroundAttackLast = 64;   // Attack11 .. AttackLw4
constexpr int AttackAirN = 65, AttackAirLw = 69, LandingAirN = 70, LandingAirLw = 74;
constexpr int DamageFirst = 75, DamageLast = 91;
constexpr int ItemFirst = 92, ItemLast = 177;              // item pick up, throws, swings, guns, lift
constexpr int GuardOn = 178, GuardReflect = 182;
constexpr int DownFirst = 183, DownLast = 204;              // DownBound .. PassiveCeil (techs)
constexpr int ShieldBreakFirst = 205, Furafura = 211;
constexpr int CatchFirst = 212, CatchLast = 218, ThrowFirst = 219, ThrowLast = 222;
constexpr int CaptureFirst = 223, CaptureLast = 232;
constexpr int EscapeF = 233, EscapeN = 235, EscapeAir = 236;
constexpr int ReboundStop = 237, Rebound = 238;
constexpr int ThrownFirst = 239, ThrownLast = 243, Pass = 244, Ottotto = 245, OttottoWait = 246;
constexpr int FlyReflectWall = 247, MissFoot = 251;
constexpr int CliffFirst = 252, CliffLast = 263;
constexpr int AppealSR = 264, AppealSL = 265;
constexpr int GrabbedFirst = 266, GrabbedLast = 340;       // shouldered, captures, thrown, buried, ice...
constexpr int CharacterSpecificFirst = 341;                // specials and anything a character adds
}  // namespace ms

enum class Group : uint8_t {
  Dead,        // dead, sleeping, respawning: the board goes away
  Locomotion,  // standing, walking, dashing, running, turning, jumpsquat, teetering, taunting
  Crouch,
  Landing,     // plain landing
  Waveland,    // LandingFallSpecial: the wavedash / waveland slide
  LandingAir,  // landing out of an aerial (the L-cancel check)
  Aerial,      // nair..dair in the air: board tricks
  Air,         // any other airborne common state
  Attack,      // grounded attacks, grabs and character specials: they slide if slide_ground_attacks
  Shield,
  Escape,      // rolls and spot dodge: the game moves the character itself
  Hit,         // hitstun, knockdown, techs, shield break: the board is dropped
  Captured,    // grabbed, thrown, buried, frozen...: the board is dropped
  Cliff,       // ledge: the board is dropped
  Other,       // grounded state nothing above knows (items, rebound...): the game has full control
};
Group classify(int motion);
const char* group_name(Group g);

// Spec, "Mount from": standing, walking, dashing, running, crouching, wavedash/waveland slide and
// landing, grounded only.
bool can_mount(int motion, bool grounded);
// Getting hit, grabbed, grabbing a ledge or dying drops the board.
bool drops_board(Group g);
bool is_aerial(int motion);
inline int aerial_index(int motion) { return motion - ms::AttackAirN; }        // 0 nair .. 4 dair
inline int landing_aerial_index(int motion) { return motion - ms::LandingAirN; }

// How the board treats the ground this frame.
enum class Mode : uint8_t {
  Roll,         // friction skate_friction, velocity = carried (never grows)
  Brake,        // stick held back: the character's own friction, applied to the carried velocity
  Push,         // skate_push: stick forward from (nearly) stopped, up to walk speed, never above
  Vanilla,      // the game's own friction and speed, but never faster than the board was going
  Free,         // the game moves the character itself (attacks, specials); friction as skate_friction
  PassThrough,  // same with the character's own friction (rolls, items, anything unknown)
};
const char* mode_name(Mode m);

struct Frame {
  Group group = Group::Other;
  bool stumbling = false;    // in the missed-L-cancel landing
  float stick_x = 0.0f;      // fp->input.lstick.x, -1..1
  float carried = 0.0f;      // board velocity along the ground before this frame (signed)
  float base_friction = 0.0f;// the character's own co_attrs.ground_friction
  float walk_max = 0.0f;     // the character's co_attrs.walk_max_vel
};
Mode pick_mode(const Frame& f, const Tunables& t);
// The value to write into co_attrs.ground_friction for this frame, before the game's physics runs.
float friction_for(Mode m, Group g, float base_friction, const Tunables& t);

struct Move {
  float write_vel;    // gr_vel to hand to the game this frame
  float carried;      // the board's velocity from now on
};
// game_vel: what the game's physics produced (with friction_for already in effect).
// entry_speed: |carried| when the current Free/PassThrough state began; nothing that happens inside
// such a state can leave the board faster than that.
Move board_move(Mode m, const Frame& f, float game_vel, float entry_speed, const Tunables& t);

// Moves v toward target by at most step.
float approach(float v, float target, float step);
// Never faster than `limit` and never reversed against it.
float no_gain(float v, float limit);

}  // namespace skate
