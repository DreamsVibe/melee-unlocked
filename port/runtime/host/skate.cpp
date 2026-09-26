// Skateboard mod: guest bindings. See skate.h and SKATE_NOTES.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef SKATE_MOD
#include "skate.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "ppc.h"
#include "skate_hook.h"
#include "skate_rules.h"
#include "slippi_online.h"

namespace skate {

bool g_hooks_live = false;

namespace {

// ---- guest layout, NTSC 1.02 (names from the decomp, addresses from port/recomp/GALE01_symbols.txt;
// the full map is in SKATE_NOTES.md)
constexpr uint32_t kPlayerSlots = 0x80453080;   // StaticPlayer player_slots[6]
constexpr uint32_t kPlayerStride = 0xE90;
constexpr uint32_t kSpState = 0x00;     // 0 = not playing
constexpr uint32_t kSpType = 0x08;      // 0 = human
constexpr uint32_t kSpCtrl = 0x46;      // s8 controller index
constexpr uint32_t kSpEntity = 0xB0;    // HSD_GObj* player_entity[0]
constexpr uint32_t kGObjUserData = 0x2C;

constexpr uint32_t kFtGObj = 0x000, kFtKind = 0x004, kFtMotion = 0x010, kFtFacing = 0x02C;
constexpr uint32_t kFtSelfVel = 0x080, kFtPos = 0x0B0, kFtGroundAir = 0x0E0;
constexpr uint32_t kFtGroundAccel1 = 0x0E4, kFtGroundAccel2 = 0x0E8, kFtGrVel = 0x0EC;
constexpr uint32_t kFtWalkMax = 0x118, kFtGroundFriction = 0x128;   // co_attrs (the fighter's own copy)
constexpr uint32_t kFtLStickX = 0x620, kFtX67F = 0x67F;
constexpr uint32_t kFtPercent = 0x1830;
constexpr uint32_t kFighterSize = 0x2400;

constexpr int kSlots = 6;

// ---- guest reads that cannot take the process down: every pointer here comes out of guest memory
// that is garbage until a match has been set up (host::rd32 is fatal on a bad address).
bool mapped(uint32_t addr, uint32_t bytes) {
  if (addr < 0x80000000u) return false;
  const uint64_t off = (uint64_t)(addr & 0x3FFFFFFFu);
  return off + bytes <= (uint64_t)ppc::RAM_SIZE;
}
uint32_t rd32(uint32_t a) { return mapped(a, 4) ? host::rd32(a) : 0; }
uint8_t rd8(uint32_t a) { return mapped(a, 1) ? host::rd8(a) : 0; }
float rdf(uint32_t a) { const uint32_t v = rd32(a); float f; std::memcpy(&f, &v, 4); return f; }
void wrf(uint32_t a, float f) { if (!mapped(a, 4)) return; uint32_t v; std::memcpy(&v, &f, 4); host::wr32(a, v); }

// The Fighter behind a player slot, or 0: slot in use, entity mapped, and the Fighter points back
// at its own GObj (which is what tells a live fighter from a stale pointer).
uint32_t fighter_of_slot(int slot) {
  const uint32_t base = kPlayerSlots + (uint32_t)slot * kPlayerStride;
  if (rd32(base + kSpState) == 0) return 0;
  const uint32_t gobj = rd32(base + kSpEntity);
  if (!mapped(gobj, kGObjUserData + 4)) return 0;
  const uint32_t fp = rd32(gobj + kGObjUserData);
  if (!mapped(fp, kFighterSize) || rd32(fp + kFtGObj) != gobj) return 0;
  return fp;
}

// ---- per-player skate state (spec step 3). One per Melee player slot; the Fighter it belongs to is
// remembered so a new match, a new fighter in the slot or a character change starts it clean.
enum class Landed : uint8_t { None, Clean, Stumble };

struct Player {
  uint32_t fp = 0;              // Fighter this state belongs to; 0 = none
  int kind = -1;                // character, to notice Zelda/Sheik swapping attributes under us
  int port = -1;                // controller port driving it (humans only), -1 = none
  bool on_board = false;
  float carried = 0.0f;         // board velocity along the ground (signed, Melee units per frame)
  float entry_speed = 0.0f;     // |carried| when the current Free/PassThrough state began
  float base_friction = 0.0f;   // the character's own ground friction, restored on dismount
  bool friction_written = false;
  int motion = -1, prev_motion = -1;
  Group group = Group::Other;
  Mode mode = Mode::Roll;
  bool grounded = false, was_grounded = false;
  bool stumbling = false;
  Landed last_landing = Landed::None;
  float percent = 0.0f;
  uint32_t frames_on_board = 0;
};
Player g_players[kSlots];

void restore_friction(Player& p) {
  if (p.friction_written && p.fp && rd32(p.fp + kFtKind) == (uint32_t)p.kind) wrf(p.fp + kFtGroundFriction, p.base_friction);
  p.friction_written = false;
}

// Off the board, with the fighter's own friction back. Safe to call on a player already off it.
void drop_board(Player& p, const char* why) {
  if (!p.on_board) return;
  restore_friction(p);
  p.on_board = false;
  p.stumbling = false;
  p.carried = p.entry_speed = 0.0f;
  if (host::options.trace_calls) host::log("skate: slot fighter %08X off the board (%s)", p.fp, why);
}

// A fresh state for whatever now sits in the slot. `live` false when the old Fighter is gone
// (match over, slot emptied): then nothing is written back, its memory is no longer a fighter.
void reset_player(Player& p, uint32_t fp, bool live) {
  if (live) restore_friction(p);
  p = Player{};
  p.fp = fp;
  if (fp) p.kind = (int)rd32(fp + kFtKind);
}

// The slot whose fighter `fp` is, or -1 (Nana, a fighter being set up, anything else).
int slot_of(uint32_t fp) {
  for (int i = 0; i < kSlots; ++i)
    if (g_players[i].fp == fp && fp) return i;
  return -1;
}

// Keeps g_players in step with the slots. Cheap: six slots, a few reads each.
void track_slots() {
  for (int i = 0; i < kSlots; ++i) {
    Player& p = g_players[i];
    const uint32_t fp = fighter_of_slot(i);
    if (fp != p.fp) { reset_player(p, fp, false); continue; }   // match start / end, new fighter
    if (!fp) continue;
    if ((int)rd32(fp + kFtKind) != p.kind) { reset_player(p, fp, false); continue; }   // transformed: attributes were reloaded
    const uint32_t base = kPlayerSlots + (uint32_t)i * kPlayerStride;
    const int8_t ctrl = (int8_t)rd8(base + kSpCtrl);
    p.port = rd32(base + kSpType) == 0 && ctrl >= 0 && ctrl < 4 ? ctrl : -1;
  }
}

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_reload_requested{false};
std::string g_dir = "skate";
Tunables g_tunables;

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream s;
  s << f.rdbuf();
  out = s.str();
  return true;
}

void load_tunables() {
  Tunables t;   // compiled defaults, then whatever the file says
  const std::string path = g_dir + "/skate_tunables.json";
  std::string text, error, warnings;
  if (!read_file(path, text)) {
    host::log("skate: %s not found, using compiled defaults", path.c_str());
  } else if (!parse_tunables(text, t, error, warnings)) {
    host::log("skate: %s is not valid JSON (%s), using compiled defaults", path.c_str(), error.c_str());
  } else {
    if (!error.empty()) host::log("skate: %s: %s", path.c_str(), error.c_str());
    if (!warnings.empty()) host::log("skate: %s: unknown keys ignored: %s", path.c_str(), warnings.c_str());
    host::log("skate: tunables loaded from %s", path.c_str());
  }
  g_tunables = t;
}

}  // namespace

void set_enabled(bool on) {
  if (g_enabled.exchange(on) != on) host::log("skate: mod %s", on ? "on" : "off");
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
bool active() {
  return enabled() && !slippi::online::is_online_match() && !slippi::online::in_online_menus();
}

void begin_frame() {
  if (g_reload_requested.exchange(false)) load_files();
  const bool live = active();
  if (!live && g_hooks_live) {
    // Switched off (or an online session opened) mid-match: give every fighter its friction back.
    for (Player& p : g_players) drop_board(p, "mod off");
  }
  g_hooks_live = live;
  if (live) track_slots();
}

void hook_enter(ppc::Context& c, uint8_t* m, Site site) {
  (void)m;
  const uint32_t gobj = c.r[3];
  if (!mapped(gobj, kGObjUserData + 4)) return;
  const uint32_t fp = rd32(gobj + kGObjUserData);
  const int slot = slot_of(fp);
  if (slot < 0) return;
  Player& p = g_players[slot];
  switch (site) {
    case Site::ProcUpdate: {
      p.prev_motion = p.motion;
      p.motion = (int)rd32(fp + kFtMotion);
      p.was_grounded = p.grounded;
      p.grounded = rd32(fp + kFtGroundAir) == 0;
      const float percent = rdf(fp + kFtPercent);
      const bool dead = classify(p.motion) == Group::Dead;
      // Death, respawn: the state starts over (spec: "resets on death, respawn and match start").
      if (dead && (p.on_board || p.last_landing != Landed::None)) { drop_board(p, "died"); p.last_landing = Landed::None; }
      p.percent = percent;
      if (p.on_board) ++p.frames_on_board;
      break;
    }
    case Site::GroundMove:
    case Site::LandingAir:
      break;
  }
}

void set_data_dir(const std::string& dir) { g_dir = dir.empty() ? "skate" : dir; }
const std::string& data_dir() { return g_dir; }
void load_files() { load_tunables(); }
void request_reload() { g_reload_requested.store(true); }
const Tunables& tunables() { return g_tunables; }

}  // namespace skate
#endif  // SKATE_MOD
