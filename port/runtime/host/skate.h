// Skateboard mod for competitive 1v1 (spec: "Melee Skateboard Mod — Build Spec", SKATE_NOTES.md).
//
// D-pad left puts a board under any character. The board carries the speed the character already
// has (ground friction is zero while mounted) and never adds any; aerials are shown as board tricks;
// a clean L-cancel keeps you rolling and a missed one stumbles.
//
// This changes the simulation, so it only ever runs offline: an online session (or its menus) turns
// it off, the way Slippi would need both clients to run it. Everything is behind SKATE_MOD at compile
// time and skate::set_enabled at run time; with either off the game is byte for byte vanilla.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef SKATE_MOD
#include <string>

#include "host.h"
#include "skate_board.h"
#include "skate_tunables.h"

namespace skate {

// ---- run-time switch (PC settings "Skateboard mod", port-settings.ini "skatemod", --skate/--no-skate)
void set_enabled(bool on);
bool enabled();
// Whether the mod is doing anything right now: enabled, and no online session anywhere near.
bool active();

// ---- data files (skate/ next to the working directory unless --skate-dir says otherwise)
void set_data_dir(const std::string& dir);
const std::string& data_dir();
// Loads skate_tunables.json over the compiled defaults. Called once at start-up; F5 asks for it again
// through request_reload, which the simulation thread picks up at the start of its next frame.
void load_files();
void request_reload();
// F8: writes what has been recorded to skate_framedata.json (spec step 7's export) at the start of the
// next simulation frame.
void request_save();
// The tunables the simulation is using. Only the simulation thread may hold the reference across a
// reload; the overlay copies what it shows.
const Tunables& tunables();

// Simulation thread, once per frame from HLE(PADRead) before the game runs the frame: picks up a
// requested reload, switches the hooks on or off, and notices new, finished or respawned fighters.
void begin_frame();
// Same place, with the freshly polled pads: D-pad left presses, and the mount lag (the pad frozen
// for mount_frames frames after a mount or dismount). Local pads only; the mod is never online.
void apply_pads(host::PadState pads[4]);

// ---- frame advance (spec: debug tools). F6 holds the game and lets it go; F7 steps one frame while
// it is held. Polled on the simulation thread (the render thread may be the one waiting), and only
// while this window has focus. Nothing else about the game changes; audio simply stops while held.
// Called from HLE(PADRead) before the pads are polled: blocks while the game is held.
void frame_gate();
void set_frame_advance(bool on);
bool frame_advance();
void request_step();

// ---- what the renderer draws and the debug overlay shows. Built on the simulation thread at the
// start of each frame from the state the previous frame left (so the camera and the fighters match
// the picture being drawn), copied out under a lock by the render thread.
struct BoardView {
  bool present = false;       // a fighter is in this slot
  bool on_board = false;      // and has a board to draw
  int port = -1;              // controller port (0..3), -1 for a CPU or none
  int kind = -1;
  Pose pose;                  // where the board is
  bool grounded = false;
  float velocity = 0.0f;      // board (grounded) or horizontal (air) velocity
  const char* state = "";     // skate state: rolling, braking, trick, stumble...
  const char* landing = "";   // last landing: clean / stumble / -
  const char* trick = "";     // trick being shown, if any
  const char* phase = "";     // pop / flip / catch while a trick plays
  float phase_t = 0.0f;       // 0..1 through the aerial
  int motion = -1;
  Vec3 ground{};              // the fighter's ground point, for the debug marker
};
struct Snapshot {
  uint64_t frame = 0;
  bool active = false;
  Camera camera;
  BoardView boards[6];
  Tunables tunables;
  size_t framedata_rows = 0, trick_rows = 0;
  bool frame_advance = false;
};
// Render thread: a copy of the latest snapshot.
Snapshot snapshot();

}  // namespace skate
#endif  // SKATE_MOD
