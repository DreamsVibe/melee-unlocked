// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef SKATE_MOD
#include "skate_overlay.h"

#include <algorithm>
#include <vector>

#include "imgui.h"
#include "skate.h"

namespace skate_ui {
namespace {

// Melee's port colours on the deck, so every board reads as its player's at a glance.
ImVec4 port_colour(int port) {
  switch (port) {
    case 0: return ImVec4(0.92f, 0.26f, 0.24f, 1.0f);
    case 1: return ImVec4(0.28f, 0.47f, 1.00f, 1.0f);
    case 2: return ImVec4(1.00f, 0.80f, 0.18f, 1.0f);
    case 3: return ImVec4(0.26f, 0.78f, 0.36f, 1.0f);
    default: return ImVec4(0.65f, 0.65f, 0.68f, 1.0f);
  }
}

ImU32 shaded(ImVec4 c, float shade) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * shade, c.y * shade, c.z * shade, c.w));
}

ImVec4 part_colour(skate::Part part, int port) {
  switch (part) {
    case skate::Part::Grip: return ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    case skate::Part::Deck: return port_colour(port);
    case skate::Part::Truck: return ImVec4(0.72f, 0.73f, 0.76f, 1.0f);
    case skate::Part::Wheel: return ImVec4(0.96f, 0.94f, 0.86f, 1.0f);
  }
  return ImVec4(1, 1, 1, 1);
}

// Spec step 8: every mounted player's board, drawn on the background list so any window or overlay
// lands on top of it. Faces are painted back to front, each with a dark edge so the silhouette stays
// clear against any stage.
void draw_boards(const skate::Snapshot& s, const skate::Rect& image, ImDrawList* dl) {
  if (!s.active || !s.camera.valid) return;
  static std::vector<skate::Face> faces;
  faces.clear();
  const std::vector<skate::Box> mesh = skate::board_mesh(s.tunables);
  for (int i = 0; i < 6; ++i) {
    const skate::BoardView& b = s.boards[i];
    if (b.on_board) skate::append_board_faces(faces, mesh, b.pose, s.camera, image, i);
  }
  skate::sort_faces(faces);
  dl->PushClipRect(ImVec2(image.x, image.y), ImVec2(image.x + image.w, image.y + image.h), true);
  const ImU32 edge = IM_COL32(12, 12, 14, 220);
  for (const skate::Face& f : faces) {
    const ImVec2 p[4] = {{f.px[0], f.py[0]}, {f.px[1], f.py[1]}, {f.px[2], f.py[2]}, {f.px[3], f.py[3]}};
    dl->AddConvexPolyFilled(p, 4, shaded(part_colour(f.part, s.boards[f.owner].port), f.shade));
    dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, 1.25f);
  }
  dl->PopClipRect();
}

}  // namespace

void frame(float window_w, float window_h, float image_aspect) {
  const skate::Snapshot s = skate::snapshot();
  const skate::Rect image = skate::image_rect(window_w, window_h, image_aspect);
  draw_boards(s, image, ImGui::GetBackgroundDrawList());
}

}  // namespace skate_ui
#endif  // SKATE_MOD
