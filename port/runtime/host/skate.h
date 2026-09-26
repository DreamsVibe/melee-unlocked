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
// The tunables the simulation is using. Only the simulation thread may hold the reference across a
// reload; the overlay copies what it shows.
const Tunables& tunables();

}  // namespace skate
#endif  // SKATE_MOD
