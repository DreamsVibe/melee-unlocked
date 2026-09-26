// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef SKATE_MOD
#include "skate_overlay.h"

#include <algorithm>
#include <cstdio>
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

bool g_debug = false;   // F4

// Board debug draw: each board's centre, its ground point, and the trick phase, next to it.
void draw_board_debug(const skate::Snapshot& s, const skate::Rect& image, ImDrawList* dl) {
  if (!s.active || !s.camera.valid) return;
  for (int i = 0; i < 6; ++i) {
    const skate::BoardView& b = s.boards[i];
    if (!b.present) continue;
    const skate::Projected g = skate::project(s.camera, image, b.ground);
    if (g.ok) {   // the fighter's ground point: a small cross, whether or not they are on a board
      dl->AddLine(ImVec2(g.x - 5, g.y), ImVec2(g.x + 5, g.y), IM_COL32(255, 255, 255, 200), 1.5f);
      dl->AddLine(ImVec2(g.x, g.y - 5), ImVec2(g.x, g.y + 5), IM_COL32(255, 255, 255, 200), 1.5f);
    }
    if (!b.on_board) continue;
    const skate::Projected c = skate::project(s.camera, image, b.pose.origin);
    const skate::Projected nose = skate::project(s.camera, image, b.pose.apply({s.tunables.board_length * 0.5f, 0, 0}));
    if (!c.ok) continue;
    if (nose.ok) dl->AddLine(ImVec2(c.x, c.y), ImVec2(nose.x, nose.y), IM_COL32(255, 230, 60, 255), 2.0f);   // nose direction
    dl->AddCircle(ImVec2(c.x, c.y), 4.0f, IM_COL32(255, 230, 60, 255), 12, 2.0f);
    char label[96];
    if (*b.phase) std::snprintf(label, sizeof label, "%s  %s %.2f", b.trick, b.phase, b.phase_t);
    else std::snprintf(label, sizeof label, "%s", b.state);
    dl->AddText(ImVec2(c.x + 8, c.y - 22), IM_COL32(0, 0, 0, 200), label);
    dl->AddText(ImVec2(c.x + 7, c.y - 23), IM_COL32(255, 255, 255, 255), label);
  }
}

// The standalone skate debug overlay (F4): per player, on_board, velocity, skate state and the last
// landing, plus what the mod has loaded and the hotkeys.
void draw_overlay(const skate::Snapshot& s) {
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 16, 16), ImGuiCond_FirstUseEver, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.72f);
  if (!ImGui::Begin("Skate debug (F4)", &g_debug, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                                   ImGuiWindowFlags_NoNav)) {
    ImGui::End();
    return;
  }
  if (!skate::enabled()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Skateboard mod is off (PC settings > Game)");
  else if (!s.active) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Off: an online session is open");
  ImGui::Text("frame %llu   tricks %zu rows   frame data %zu aerials", (unsigned long long)s.frame, s.trick_rows, s.framedata_rows);
  if (s.frame_advance) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "FRAME ADVANCE: F7 steps, F6 resumes");
  if (ImGui::BeginTable("skate_players", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    for (const char* h : {"slot", "on_board", "velocity", "state", "landing", "trick", "motion"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (int i = 0; i < 6; ++i) {
      const skate::BoardView& b = s.boards[i];
      if (!b.present) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (b.port >= 0) ImGui::TextColored(port_colour(b.port), "P%d", b.port + 1); else ImGui::TextDisabled("CPU");
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.on_board ? "yes" : "no");
      ImGui::TableNextColumn(); ImGui::Text("%+.3f", b.velocity);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.state);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.landing);
      ImGui::TableNextColumn();
      if (*b.phase) ImGui::Text("%s %s", b.trick, b.phase); else ImGui::TextDisabled("-");
      ImGui::TableNextColumn(); ImGui::Text("%d", b.motion);
    }
    ImGui::EndTable();
  }
  ImGui::Separator();
  bool fa = skate::frame_advance();
  if (ImGui::Checkbox("Frame advance (F6)", &fa)) skate::set_frame_advance(fa);
  ImGui::SameLine();
  if (ImGui::Button("Step (F7)")) skate::request_step();
  if (ImGui::Button("Reload skate/*.json (F5)")) skate::request_reload();
  ImGui::SameLine();
  if (ImGui::Button("Save learned tricks + frame data (F8)")) skate::request_save();
  ImGui::TextDisabled("Files: %s/skate_tunables.json, skate_tricks.json, skate_framedata.json", skate::data_dir().c_str());
  ImGui::End();
}

}  // namespace

void frame(float window_w, float window_h, float image_aspect) {
  // Hotkeys. F6/F7 (frame advance) are read by the simulation thread itself (skate::frame_gate).
  if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) g_debug = !g_debug;
  if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { skate::request_reload(); }
  if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) { skate::request_save(); }
  if (!skate::enabled() && !g_debug) return;
  const skate::Snapshot s = skate::snapshot();
  const skate::Rect image = skate::image_rect(window_w, window_h, image_aspect);
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  draw_boards(s, image, bg);
  if (g_debug) {
    draw_board_debug(s, image, bg);
    draw_overlay(s);
  }
}

}  // namespace skate_ui
#endif  // SKATE_MOD
