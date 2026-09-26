// SPDX-License-Identifier: GPL-2.0-or-later
#include "skate_data.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "nlohmann/json.hpp"

namespace skate {
namespace {
using json = nlohmann::json;

// FighterKind order (decomp melee/ft/forward.h), with the names players use.
const char* const kKindNames[kKinds] = {
    "Mario", "Fox", "Captain Falcon", "Donkey Kong", "Kirby", "Bowser", "Link", "Sheik", "Ness",
    "Peach", "Popo", "Nana", "Pikachu", "Samus", "Yoshi", "Jigglypuff", "Mewtwo", "Luigi", "Marth",
    "Zelda", "Young Link", "Dr. Mario", "Falco", "Pichu", "Mr. Game & Watch", "Ganondorf", "Roy",
    "Master Hand", "Crazy Hand", "Male Wireframe", "Female Wireframe", "Giga Bowser", "Sandbag"};
const char* const kAerialNames[kAerials] = {"nair", "fair", "bair", "uair", "dair"};
const char* const kTrickNames[kTrickCount] = {"kickflip", "heelflip", "shoveit", "varial", "impossible", "stomp", "grab"};

std::string lower(std::string s) {
  for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}
}  // namespace

const char* kind_name(int kind) {
  if (kind >= 0 && kind < kKinds) return kKindNames[kind];
  static thread_local char buf[16];
  std::snprintf(buf, sizeof buf, "kind %d", kind);
  return buf;
}

int kind_from_name(const std::string& name) {
  const std::string n = lower(name);
  for (int i = 0; i < kKinds; ++i)
    if (lower(kKindNames[i]) == n) return i;
  char* end = nullptr;
  const long v = std::strtol(name.c_str(), &end, 10);
  if (!name.empty() && end && *end == 0 && v >= 0 && v < kKinds) return (int)v;
  return -1;
}

const char* aerial_name(int aerial) { return aerial >= 0 && aerial < kAerials ? kAerialNames[aerial] : "?"; }
int aerial_from_name(const std::string& name) {
  const std::string n = lower(name);
  for (int i = 0; i < kAerials; ++i)
    if (n == kAerialNames[i]) return i;
  return -1;
}

const char* trick_name(Trick t) { return (int)t < kTrickCount ? kTrickNames[(int)t] : "?"; }
bool trick_from_name(const std::string& name, Trick& out) {
  const std::string n = lower(name);
  for (int i = 0; i < kTrickCount; ++i)
    if (n == kTrickNames[i]) { out = (Trick)i; return true; }
  return false;
}

const char* source_name(Source s) {
  switch (s) {
    case Source::Hand: return "hand";
    case Source::Auto: return "auto";
    default: return "default";
  }
}

// ---------------- frame data
const FrameData* FrameDataTable::find(int kind, int aerial) const {
  auto it = rows_.find(key(kind, aerial));
  return it == rows_.end() || !it->second.valid() ? nullptr : &it->second;
}

void FrameDataTable::observe(int kind, int aerial, int first_active, int last_active, int end) {
  if (kind < 0 || kind >= kKinds || aerial < 0 || aerial >= kAerials) return;
  FrameData& d = rows_[key(kind, aerial)];
  if (first_active > 0) {
    d.first_active = d.first_active > 0 ? std::min(d.first_active, first_active) : first_active;
    d.last_active = std::max(d.last_active, last_active);
  }
  if (end > 0) d.end = std::max(d.end, end);
  if (d.end < d.last_active) d.end = d.last_active;
  ++d.samples;
}

std::string FrameDataTable::to_json() const {
  json rows = json::array();
  for (const auto& [k, d] : rows_) {
    const int kind = k / kAerials, aerial = k % kAerials;
    rows.push_back({{"character", kind_name(kind)}, {"kind", kind}, {"aerial", aerial_name(aerial)},
                    {"startup", d.first_active}, {"active_end", d.last_active}, {"end", d.end},
                    {"samples", d.samples}});
  }
  json doc = {{"_comment", "Recorded in game by the skateboard mod: startup = first frame a hitbox is out, "
                           "active_end = last such frame, end = frames the aerial lasts. F8 writes this file."},
              {"version", 1}, {"rows", rows}};
  return doc.dump(2) + "\n";
}

bool FrameDataTable::from_json(const std::string& text, std::string& error) {
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.count("rows") || !doc["rows"].is_array()) {
    error = "expected an object with a \"rows\" array";
    return false;
  }
  for (const json& r : doc["rows"]) {
    if (!r.is_object()) continue;
    int kind = r.count("kind") && r["kind"].is_number() ? r["kind"].get<int>()
               : r.count("character") && r["character"].is_string() ? kind_from_name(r["character"].get<std::string>()) : -1;
    const int aerial = r.count("aerial") && r["aerial"].is_string() ? aerial_from_name(r["aerial"].get<std::string>()) : -1;
    if (kind < 0 || kind >= kKinds || aerial < 0) continue;
    FrameData d;
    auto num = [&](const char* k) { return r.count(k) && r[k].is_number() ? r[k].get<int>() : 0; };
    d.first_active = num("startup");
    d.last_active = num("active_end");
    d.end = num("end");
    d.samples = num("samples");
    if (d.valid()) set(kind, aerial, d);
  }
  return true;
}

ActiveWindow active_window(const FrameDataTable& table, int kind, int aerial, float default_start, float default_end) {
  if (const FrameData* d = table.find(kind, aerial)) {
    // Frame n (1-based) covers the interval [(n-1)/end, n/end) of the move.
    const float end = (float)d->end;
    return ActiveWindow{(float)(d->first_active - 1) / end, (float)d->last_active / end, true};
  }
  return ActiveWindow{default_start, default_end, false};
}

// ---------------- trick table
TrickRow TrickTable::default_row(int aerial) {
  TrickRow r;
  r.source = Source::Default;
  switch (aerial) {
    case 0: r.trick = Trick::Shoveit; r.rotations = 0.5f; break;
    case 1: r.trick = Trick::Kickflip; break;
    case 2: r.trick = Trick::Heelflip; break;
    case 3: r.trick = Trick::Impossible; break;
    case 4: r.trick = Trick::Stomp; break;
    default: r.trick = Trick::Grab; break;
  }
  return r;
}

bool TrickTable::has(int kind, int aerial) const { return rows_.count(key(kind, aerial)) != 0; }

TrickRow TrickTable::get(int kind, int aerial) const {
  auto it = rows_.find(key(kind, aerial));
  return it == rows_.end() ? default_row(aerial) : it->second;
}

bool TrickTable::offer(int kind, int aerial, const TrickRow& row) {
  if (kind < 0 || kind >= kKinds || aerial < 0 || aerial >= kAerials) return false;
  auto it = rows_.find(key(kind, aerial));
  if (it != rows_.end() && (int)it->second.source > (int)row.source) return false;
  rows_[key(kind, aerial)] = row;
  return true;
}

bool TrickTable::learn(int kind, int aerial, const TrickRow& sampled) {
  if (kind < 0 || kind >= kKinds || aerial < 0 || aerial >= kAerials) return false;
  auto it = rows_.find(key(kind, aerial));
  if (it == rows_.end() || it->second.source == Source::Default) {
    TrickRow r = sampled;
    r.source = Source::Auto;
    r.direction_auto = false;
    rows_[key(kind, aerial)] = r;
    return true;
  }
  if (it->second.direction_auto) {
    it->second.direction = sampled.direction;
    it->second.direction_auto = false;   // learned once; F8 saves it as a plain number
    return true;
  }
  return false;
}

void TrickTable::ensure_reference() {
  auto it = rows_.find(key(kFalco, kFair));
  if (it != rows_.end() && it->second.source == Source::Hand) return;   // the file already has it
  TrickRow r;
  r.trick = Trick::Varial;
  r.rotations = 1.0f;
  r.direction = 1.0f;
  r.direction_auto = true;
  r.source = Source::Hand;
  rows_[key(kFalco, kFair)] = r;
}

std::string TrickTable::to_json() const {
  json rows = json::array();
  for (const auto& [k, r] : rows_) {
    const int kind = k / kAerials, aerial = k % kAerials;
    json row = {{"character", kind_name(kind)}, {"aerial", aerial_name(aerial)}, {"trick", trick_name(r.trick)},
                {"direction", r.direction >= 0.0f ? 1 : -1}, {"rotations", r.rotations}, {"source", source_name(r.source)}};
    if (r.direction_auto) row["direction"] = "auto";
    rows.push_back(row);
  }
  json doc = {{"_comment", "Board trick per character per aerial. source: hand rows are never overwritten; auto rows come "
                           "from sampling the character's bones in game; default rows are placeholders. F5 reloads, F8 saves."},
              {"version", 1}, {"rows", rows}};
  return doc.dump(2) + "\n";
}

bool TrickTable::from_json(const std::string& text, std::string& error, std::string& warnings) {
  error.clear();
  warnings.clear();
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.count("rows") || !doc["rows"].is_array()) {
    error = "expected an object with a \"rows\" array";
    return false;
  }
  std::map<int, TrickRow> rows;
  int index = 0;
  for (const json& r : doc["rows"]) {
    ++index;
    auto warn = [&](const std::string& what) { warnings += (warnings.empty() ? "" : "; ") + ("row " + std::to_string(index) + ": " + what); };
    if (!r.is_object()) { warn("not an object"); continue; }
    const int kind = r.count("character") && r["character"].is_string() ? kind_from_name(r["character"].get<std::string>())
                     : r.count("character") && r["character"].is_number() ? r["character"].get<int>() : -1;
    const int aerial = r.count("aerial") && r["aerial"].is_string() ? aerial_from_name(r["aerial"].get<std::string>()) : -1;
    if (kind < 0 || kind >= kKinds) { warn("unknown character"); continue; }
    if (aerial < 0) { warn("unknown aerial"); continue; }
    TrickRow row;
    if (!r.count("trick") || !r["trick"].is_string() || !trick_from_name(r["trick"].get<std::string>(), row.trick)) { warn("unknown trick"); continue; }
    if (r.count("direction") && r["direction"].is_number()) row.direction = r["direction"].get<float>() < 0.0f ? -1.0f : 1.0f;
    if (r.count("direction") && r["direction"].is_string() && lower(r["direction"].get<std::string>()) == "auto") row.direction_auto = true;
    if (r.count("rotations") && r["rotations"].is_number()) row.rotations = std::clamp(r["rotations"].get<float>(), 0.0f, 4.0f);
    const std::string src = r.count("source") && r["source"].is_string() ? lower(r["source"].get<std::string>()) : "hand";
    row.source = src == "auto" ? Source::Auto : src == "default" ? Source::Default : Source::Hand;
    rows[key(kind, aerial)] = row;
  }
  rows_ = std::move(rows);
  return true;
}

}  // namespace skate
