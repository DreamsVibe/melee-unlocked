// Skateboard mod: every number the mod uses, in one struct, so tuning never touches logic.
//
// Pure data and JSON (de)serialisation: no guest memory, no Windows, so the unit test
// (tests/skate_core_test.cpp) builds on any compiler. Defaults are the spec's; skate/skate_tunables.json
// overrides any subset of them on start-up and on hot-reload (F5), and a missing or broken file
// leaves the compiled defaults in place.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace skate {

struct Tunables {
  // ---- mechanics (spec: "Tunables (defaults)")
  int mount_frames = 3;                // lag to mount or dismount (input frozen for this many frames)
  float skate_friction = 0.0f;         // ground friction while mounted
  int stumble_extra_frames = 8;        // extra landing lag on a missed L-cancel
  bool slide_ground_attacks = true;    // keep sliding during grounded attacks
  float airdodge_land_friction = -1.0f;// friction during a wavedash landing on the board; < 0 = vanilla
  bool skate_push = false;             // stick pushes from a standstill, up to the character's walk speed
  // How far back the stick has to be held (0..1, Melee units) before it counts as braking.
  float brake_stick = 0.30f;

  // ---- board visuals (Melee world units; a character is roughly 10 to 15 units tall)
  float board_length = 7.0f;
  float board_width = 1.9f;
  float board_thickness = 0.3f;
  float wheel_size = 0.55f;
  float foot_height = 0.0f;            // extra lift: under the wheels on the ground, under the feet in the air
  float pop_height = 1.8f;             // how high a trick lifts the board off the feet
  int catch_snap_frames = 2;           // landing mid-trick snaps the board to the catch pose this fast
  // Where the flip happens in an aerial whose frame data has not been observed yet (fractions of the move).
  float default_active_start = 0.20f, default_active_end = 0.55f;

  // ---- sounds. Guest SFX ids passed to ft_PlaySFX; 0 turns one off. Placeholders from ids the
  // decomp shows the game already using. Swap them in skate_tunables.json and hot-reload with F5.
  int sfx_pop = 0x6E, sfx_catch = 0xE0, sfx_roll = 0x6E, sfx_scrape = 115;
  int sfx_volume = 110;                // 0..127
  int roll_volume = 45;                // the roll tick is quieter than the rest
  int roll_interval_frames = 14;       // the "loop" is a tick this often while rolling
  float roll_min_speed = 0.35f;        // below this the board is standing still: no roll sound
};

// Parses JSON text into `out`, overriding only the keys present. Unknown keys are ignored (and
// listed in `warnings`), wrong types are reported in `error` and leave that field alone. Returns
// false only when the text is not JSON at all; `out` is untouched then.
bool parse_tunables(const std::string& text, Tunables& out, std::string& error, std::string& warnings);
// The full struct as pretty JSON, every key present, for writing a fresh skate_tunables.json.
std::string tunables_to_json(const Tunables& t);

}  // namespace skate
