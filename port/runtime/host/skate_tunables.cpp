// SPDX-License-Identifier: GPL-2.0-or-later
#include "skate_tunables.h"

#include <type_traits>

#include "nlohmann/json.hpp"

namespace skate {
namespace {

using json = nlohmann::json;

// One table drives both directions, so a field added to the struct is added here once.
template <typename F>
void for_each_field(Tunables& t, F&& f) {
  f("mount_frames", t.mount_frames);
  f("skate_friction", t.skate_friction);
  f("stumble_extra_frames", t.stumble_extra_frames);
  f("slide_ground_attacks", t.slide_ground_attacks);
  f("airdodge_land_friction", t.airdodge_land_friction);
  f("skate_push", t.skate_push);
  f("brake_stick", t.brake_stick);
  f("board_length", t.board_length);
  f("board_width", t.board_width);
  f("board_thickness", t.board_thickness);
  f("wheel_size", t.wheel_size);
  f("foot_height", t.foot_height);
  f("pop_height", t.pop_height);
  f("catch_snap_frames", t.catch_snap_frames);
  f("default_active_start", t.default_active_start);
  f("default_active_end", t.default_active_end);
  f("sfx_pop", t.sfx_pop);
  f("sfx_catch", t.sfx_catch);
  f("sfx_roll", t.sfx_roll);
  f("sfx_scrape", t.sfx_scrape);
  f("sfx_volume", t.sfx_volume);
  f("roll_interval_frames", t.roll_interval_frames);
  f("roll_min_speed", t.roll_min_speed);
}

bool read_value(const json& v, int& out) { if (!v.is_number()) return false; out = v.get<int>(); return true; }
bool read_value(const json& v, float& out) { if (!v.is_number()) return false; out = v.get<float>(); return true; }
bool read_value(const json& v, bool& out) {
  if (v.is_boolean()) { out = v.get<bool>(); return true; }
  if (v.is_number()) { out = v.get<double>() != 0.0; return true; }
  if (v.is_string()) {   // the spec's table writes "on"/"off"
    const std::string s = v.get<std::string>();
    if (s == "on" || s == "true") { out = true; return true; }
    if (s == "off" || s == "false") { out = false; return true; }
  }
  return false;
}

// Keeps a tuned value inside what the logic can cope with, whatever a hand-edited file says.
void sanitise(Tunables& t) {
  auto clampi = [](int& v, int lo, int hi) { v = v < lo ? lo : v > hi ? hi : v; };
  auto clampf = [](float& v, float lo, float hi) { v = v < lo ? lo : v > hi ? hi : v; };
  clampi(t.mount_frames, 0, 60);
  clampf(t.skate_friction, 0.0f, 1.0f);
  clampi(t.stumble_extra_frames, 0, 120);
  if (t.airdodge_land_friction >= 0.0f) clampf(t.airdodge_land_friction, 0.0f, 1.0f);
  clampf(t.brake_stick, 0.05f, 1.0f);
  clampf(t.board_length, 0.5f, 60.0f);
  clampf(t.board_width, 0.2f, 20.0f);
  clampf(t.board_thickness, 0.02f, 5.0f);
  clampf(t.wheel_size, 0.0f, 5.0f);
  clampf(t.foot_height, -5.0f, 5.0f);
  clampf(t.pop_height, 0.0f, 20.0f);
  clampi(t.catch_snap_frames, 1, 30);
  clampf(t.default_active_start, 0.0f, 0.95f);
  clampf(t.default_active_end, t.default_active_start + 0.01f, 1.0f);
  clampi(t.sfx_volume, 0, 127);
  clampi(t.roll_interval_frames, 1, 600);
  clampf(t.roll_min_speed, 0.0f, 10.0f);
}

}  // namespace

bool parse_tunables(const std::string& text, Tunables& out, std::string& error, std::string& warnings) {
  error.clear();
  warnings.clear();
  const json doc = json::parse(text, nullptr, false);   // no exceptions (nlohmann 3.4: no comment support)
  if (doc.is_discarded() || !doc.is_object()) { error = "not a JSON object"; return false; }
  Tunables t = out;
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    bool known = false;
    for_each_field(t, [&](const char* name, auto& field) {
      if (it.key() != name) return;
      known = true;
      // "vanilla" for the wavedash friction, as the spec's table writes it.
      if constexpr (std::is_same_v<std::decay_t<decltype(field)>, float>) {
        if (it.value().is_string() && it.value().get<std::string>() == "vanilla") { field = -1.0f; return; }
      }
      if (!read_value(it.value(), field)) error += (error.empty() ? "" : "; ") + std::string(name) + ": wrong type";
    });
    if (!known && it.key().rfind("_", 0) != 0) warnings += (warnings.empty() ? "" : ", ") + it.key();
  }
  sanitise(t);
  out = t;
  return true;
}

std::string tunables_to_json(const Tunables& in) {
  Tunables t = in;
  json doc = json::object();
  for_each_field(t, [&](const char* name, auto& field) { doc[name] = field; });
  if (t.airdodge_land_friction < 0.0f) doc["airdodge_land_friction"] = "vanilla";
  return doc.dump(2) + "\n";
}

}  // namespace skate
