// Skateboard mod: the two data tables, skate_framedata.json and skate_tricks.json.
//
// Frame data: for every character and aerial, the frame its first hitbox comes out, the frame its
// last one goes away, and the frame the move ends. Melee keeps none of this as a table (hitboxes
// are switched by each move's script), so skate.cpp records it while the game runs and F8 writes it
// out; see SKATE_NOTES.md.
//
// Trick table: one row per character per aerial saying which board trick it shows. Pure data and
// JSON, testable anywhere.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace skate {

constexpr int kKinds = 33;      // FighterKind: Mario 0 .. Sandbag 32
constexpr int kAerials = 5;     // nair, fair, bair, uair, dair (AttackAirN + index)
const char* kind_name(int kind);                 // "Falco", or "kind 40" for out of range
int kind_from_name(const std::string& name);     // -1 when unknown; accepts the number too
const char* aerial_name(int aerial);             // "nair".."dair"
int aerial_from_name(const std::string& name);   // -1 when unknown

// ---- frame data
struct FrameData {
  int first_active = 0;   // 1-based frame the first hitbox is out (the move's "startup")
  int last_active = 0;    // last frame any hitbox is out
  int end = 0;            // frames the aerial lasts when nothing cuts it short
  int samples = 0;        // how many times it has been observed
  bool valid() const { return first_active > 0 && last_active >= first_active && end >= last_active; }
};

class FrameDataTable {
 public:
  const FrameData* find(int kind, int aerial) const;
  // Merges one observation: the widest active window seen, the longest natural end.
  void observe(int kind, int aerial, int first_active, int last_active, int end);
  // Merges, keeping hitbox windows observed live over whatever the file said.
  void set(int kind, int aerial, const FrameData& d) { rows_[key(kind, aerial)] = d; }
  size_t size() const { return rows_.size(); }
  std::string to_json() const;
  bool from_json(const std::string& text, std::string& error);
  void clear() { rows_.clear(); }
 private:
  static int key(int kind, int aerial) { return kind * kAerials + aerial; }
  std::map<int, FrameData> rows_;
};

// Where the flip happens, as fractions of the move: from the frame data when the move has been seen,
// otherwise the tunable default window.
struct ActiveWindow { float start, end; bool observed; };
ActiveWindow active_window(const FrameDataTable& table, int kind, int aerial, float default_start, float default_end);

// ---- trick table
enum class Trick : uint8_t { Kickflip, Heelflip, Shoveit, Varial, Impossible, Stomp, Grab };
constexpr int kTrickCount = 7;
const char* trick_name(Trick t);
bool trick_from_name(const std::string& name, Trick& out);

enum class Source : uint8_t { Default, Auto, Hand };   // hand-set rows are never overwritten
const char* source_name(Source s);

struct TrickRow {
  Trick trick = Trick::Grab;
  float direction = 1.0f;   // +1 or -1: which way the board turns, matched to the body's spin
  float rotations = 1.0f;   // full turns (a 180 shove-it is 0.5)
  Source source = Source::Default;
};

class TrickTable {
 public:
  // The row for this character and aerial; the per-aerial default when there is none.
  TrickRow get(int kind, int aerial) const;
  bool has(int kind, int aerial) const;
  // Writes a row unless the one there has a higher-ranked source (hand > auto > default).
  bool offer(int kind, int aerial, const TrickRow& row);
  void set(int kind, int aerial, const TrickRow& row) { rows_[key(kind, aerial)] = row; }
  size_t size() const { return rows_.size(); }
  std::string to_json() const;
  bool from_json(const std::string& text, std::string& error, std::string& warnings);
  void clear() { rows_.clear(); }
  // What an aerial shows before anything better is known: nair shove-it, fair kickflip, bair
  // heelflip, uair impossible, dair stomp.
  static TrickRow default_row(int aerial);
 private:
  static int key(int kind, int aerial) { return kind * kAerials + aerial; }
  std::map<int, TrickRow> rows_;
};

}  // namespace skate
