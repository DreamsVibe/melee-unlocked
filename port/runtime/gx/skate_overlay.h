// Skateboard mod: what it puts on screen. The boards themselves, and (spec step 12) the standalone
// skate debug overlay with its hotkeys. Nothing here reads guest memory: it draws the snapshot the
// simulation thread publishes (skate::snapshot) with Dear ImGui, between NewFrame and Render.
//
// Separate from the Lab view on purpose (the spec: no dependency on it in either direction).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef SKATE_MOD

namespace skate_ui {

// Render thread, once per presented frame from settings_frame. `image_aspect` is the aspect the
// game's picture is shown at (gx::presented_aspect), so the boards land on the letterboxed image.
void frame(float window_w, float window_h, float image_aspect);

}  // namespace skate_ui

#endif  // SKATE_MOD
