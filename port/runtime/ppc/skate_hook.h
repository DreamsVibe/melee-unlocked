// Skateboard mod hooks, emitted by port/recomp/emit.py into the translated game (SKATE_NOTES.md).
//
// Like gx::RenderObserver: an object at the top of a guest function whose constructor runs at the
// function's entry, with the arguments still in the registers, and whose destructor runs when it
// returns. Only emitted inside #ifdef SKATE_MOD, so a build without the mod never sees this file.
// With the mod switched off at run time a hook costs one load and a branch.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

#include "ppc.h"

namespace skate {

enum class Site : uint8_t {
  ProcUpdate,   // Fighter_procUpdate(gobj): once per fighter per frame, before its physics
  GroundMove,   // ftCommon_SetSelfMovementFromGroundedMovement(gobj): gr_vel -> self_vel
  LandingAir,   // ftCo_LandingAir_EnterWithMsidLag(gobj, msid, f1 = lag)
};

// True while the mod is enabled and offline; refreshed once per frame from HLE(PADRead).
extern bool g_hooks_live;
void hook_enter(ppc::Context& c, uint8_t* m, Site site);

class Hook {
 public:
  Hook(ppc::Context& c, uint8_t* m, Site site) {
    // c.entry != 0 is a dispatch thunk resuming mid-function, not a call: the hook already ran.
    if (g_hooks_live && c.entry == 0) hook_enter(c, m, site);
  }
  Hook(const Hook&) = delete;
  Hook& operator=(const Hook&) = delete;
};

}  // namespace skate
