// Skateboard mod: guest bindings. See skate.h and SKATE_NOTES.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef SKATE_MOD
#include "skate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "ppc.h"
#include "skate_data.h"
#include "skate_hook.h"
#include "skate_rules.h"
#include "slippi_online.h"
#include "window.h"

#ifdef _WIN32
#include <windows.h>
#endif

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
constexpr uint32_t kFtAnimFrame = 0x894;           // float cur_anim_frame
constexpr uint32_t kFtHitboxes = 0x914, kHitboxStride = 0x138, kHitboxCount = 4;   // HitCapsule x914[4], +0 state
constexpr uint32_t kFnAnimEndFrame = 0x8006F484;  // float ftAnim_8006F484(gobj): current animation's end frame
constexpr uint32_t kFnPlaySfx = 0x80088148;       // void ft_PlaySFX(Fighter*, int id, u8 volume, u8 pan)
constexpr uint32_t kFtFloorNormal = 0x844;         // coll_data.floor.normal (x, y)
constexpr uint32_t kFtParts = 0x5E8;               // FighterBone* parts, 0x10 each, +0 HSD_JObj*
constexpr uint32_t kFtPartsTable = 0x804D6544;     // FighterPartsTable** ftPartsTable, by kind
constexpr uint32_t kPartsToJoint = 0x4;            // FighterPartsTable::part_to_joint (u8*)
constexpr int kPartHipN = 4, kPartLFootJ = 10, kPartRFootJ = 15;
constexpr uint32_t kJObjMtx = 0x44;                // HSD_JObj::mtx, 3x4 world matrix
constexpr uint32_t kGameCamera = 0x80452C68;       // Camera game_camera; +0 its GObj
constexpr uint32_t kGObjHsdObj = 0x28;             // HSD_GObj::hsd_obj -> HSD_CObj
constexpr uint32_t kCObjNear = 0x38, kCObjFov = 0x40, kCObjAspect = 0x44, kCObjView = 0x54;
constexpr uint32_t kPFtCommonData = 0x804D6554;   // ftCommonData* p_ftCommonData
constexpr uint32_t kFcLcWindow = 0xE4;            // int: frames an L/R/Z press counts for (7)

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
  // The aerial in progress (tracked whether or not the fighter is on the board, so frame data fills
  // in from ordinary play): spec step 7.
  struct Aerial {
    bool live = false;
    int index = -1;             // 0 nair .. 4 dair
    int frames = 0;             // frames the move has advanced (hitlag does not count)
    float last_anim = -1.0f;
    float end_anim = 0.0f;      // the animation's end frame
    int first_active = 0, last_active = 0;
    TrickSampler sampler;       // the hip and feet over the active frames (spec step 10)
  } aerial;
  // What the board looked like last published frame, for the landing snap and the stumble skid.
  struct Visual {
    bool was_air = false;
    Pose last;
    Pose snap_from;
    int snap_left = 0;
    int stumble_frame = 0;
  } visual;
  Frame frame;                  // this frame's inputs to the momentum rule, from ProcUpdate
  bool moved = false;           // GroundMove already stepped the board this frame
  int roll_counter = 0;         // frames since the last roll tick
  float last_write = 0.0f;      // what it wrote, for a second GroundMove in the same frame
};
Player g_players[kSlots];
std::mutex g_snapshot_lock;
Snapshot g_snapshot;             // guarded by g_snapshot_lock
uint64_t g_frame = 0;
TrickTable g_tricks;             // skate_tricks.json, plus what the sampler learns
bool g_tricks_dirty = false;
FrameDataTable g_framedata;
bool g_framedata_dirty = false;

// ---- per controller port (spec step 4): D-pad left edges, and the mount lag.
constexpr uint16_t kPadDpadLeft = 0x0001;
struct Port {
  bool dpad_down = false;       // D-pad left held last frame (for the rising edge)
  bool toggle = false;          // pressed this frame; the fighter on this port consumes it
  int freeze = 0;               // frames of mount lag still to serve
  host::PadState last{};        // the pad the game read last frame
};
Port g_ports[4];

// Mount lag, as the spec's "actionable-after lag": for mount_frames frames the game reads the pad as
// it was, with only buttons that were already held staying down, so nothing new can start. The
// fighter keeps its state and velocity. (There is no new action state; see SKATE_NOTES.md.)
void freeze_pad(host::PadState& pad, const host::PadState& last) {
  host::PadState frozen = last;
  frozen.button = (uint16_t)(last.button & pad.button);   // releases go through, presses do not
  frozen.err = pad.err;
  pad = frozen;
}

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

bool write_file(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    if (!f) return false;
  }
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void load_framedata();
void save_framedata();
void load_tricks();
void save_tricks();
void publish();

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

namespace {
void load_framedata() {
  const std::string path = g_dir + "/skate_framedata.json";
  std::string text, error;
  if (!read_file(path, text)) return;   // nothing recorded yet
  FrameDataTable t;
  if (!t.from_json(text, error)) { host::log("skate: %s: %s", path.c_str(), error.c_str()); return; }
  g_framedata = t;
  host::log("skate: frame data for %zu aerials from %s", g_framedata.size(), path.c_str());
}

void load_tricks() {
  const std::string path = g_dir + "/skate_tricks.json";
  std::string text, error, warnings;
  TrickTable t;
  if (!read_file(path, text)) host::log("skate: %s not found, every aerial uses its default trick", path.c_str());
  else if (!t.from_json(text, error, warnings)) host::log("skate: %s: %s", path.c_str(), error.c_str());
  else {
    if (!warnings.empty()) host::log("skate: %s: skipped %s", path.c_str(), warnings.c_str());
    host::log("skate: %zu trick rows from %s", t.size(), path.c_str());
  }
  t.ensure_reference();   // Falco fair, hand-set (spec step 10)
  g_tricks = t;
  g_tricks_dirty = false;
}

void save_tricks() {
  const std::string path = g_dir + "/skate_tricks.json";
  if (write_file(path, g_tricks.to_json())) host::log("skate: wrote %zu trick rows to %s", g_tricks.size(), path.c_str());
  else host::log("skate: could not write %s", path.c_str());
  g_tricks_dirty = false;
}

void save_framedata() {
  const std::string path = g_dir + "/skate_framedata.json";
  if (write_file(path, g_framedata.to_json())) host::log("skate: wrote frame data for %zu aerials to %s", g_framedata.size(), path.c_str());
  else host::log("skate: could not write %s", path.c_str());
  g_framedata_dirty = false;
}
std::atomic<bool> g_save_requested{false};
}  // namespace

void request_save() { g_save_requested.store(true); }

// ---------------- frame advance
namespace {
std::atomic<bool> g_frame_advance{false}, g_step{false};

// F6 / F7 rising edges, read straight from the keyboard: while the game is held the render thread
// may never get to ImGui's key handling (without the threaded renderer it is this same thread).
void poll_frame_keys() {
#ifdef _WIN32
  static bool f6 = false, f7 = false;
  DWORD pid = 0;
  const HWND fg = GetForegroundWindow();
  if (fg) GetWindowThreadProcessId(fg, &pid);
  const bool focused = pid == GetCurrentProcessId();
  const bool f6_now = focused && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
  const bool f7_now = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
  if (f6_now && !f6) { const bool on = !g_frame_advance.load(); g_frame_advance.store(on); host::log("skate: frame advance %s", on ? "on (F7 steps)" : "off"); }
  if (f7_now && !f7 && g_frame_advance.load()) g_step.store(true);
  f6 = f6_now;
  f7 = f7_now;
#endif
}
}  // namespace

void set_frame_advance(bool on) { g_frame_advance.store(on); }
bool frame_advance() { return g_frame_advance.load(); }
void request_step() { g_step.store(true); }

void frame_gate() {
  if (!g_hooks_live) { g_frame_advance.store(false); return; }
  // Once per frame: PADRead can run twice in one retrace, and one F7 is one frame.
  static uint32_t last_retrace = 0xFFFFFFFFu;
  const uint32_t retrace = host::retrace_count();
  if (retrace == last_retrace) return;
  last_retrace = retrace;
  for (;;) {
    poll_frame_keys();
    if (!g_frame_advance.load()) return;
    if (g_step.exchange(false)) return;
    if (host::exit_requested()) return;
    // Held: keep the window alive (a no-op when another thread owns it) and wait for a key.
    host::window_pump();
#ifdef _WIN32
    Sleep(4);
#endif
    if (!active()) { g_frame_advance.store(false); return; }
  }
}

void set_enabled(bool on) {
  if (g_enabled.exchange(on) != on) host::log("skate: mod %s", on ? "on" : "off");
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
bool active() {
  return enabled() && !slippi::online::is_online_match() && !slippi::online::in_online_menus();
}

void begin_frame() {
  // File access only here, between frames, and only when asked for (hot-reload, F8).
  // F5: the tunables and the trick table, live, without restarting the match.
  if (g_reload_requested.exchange(false)) { load_tunables(); load_tricks(); }
  if (g_save_requested.exchange(false)) { save_framedata(); save_tricks(); }
  const bool live = active();
  if (!live && g_hooks_live) {
    // Switched off (or an online session opened) mid-match: give every fighter its friction back.
    for (Player& p : g_players) drop_board(p, "mod off");
  }
  g_hooks_live = live;
  if (live) track_slots();
  publish();
}

void apply_pads(host::PadState pads[4]) {
  static uint32_t last_retrace = 0xFFFFFFFFu;
  const uint32_t retrace = host::retrace_count();
  const bool new_frame = retrace != last_retrace;   // PADRead can run twice in one retrace
  last_retrace = retrace;
  for (int i = 0; i < 4; ++i) {
    Port& port = g_ports[i];
    if (!g_hooks_live || pads[i].err != 0) { port = Port{}; port.last = pads[i]; continue; }
    if (new_frame) {
      const bool down = (pads[i].button & kPadDpadLeft) != 0;
      port.toggle = down && !port.dpad_down;   // an unconsumed press from last frame is dropped
      port.dpad_down = down;
    }
    if (port.freeze > 0) {
      freeze_pad(pads[i], port.last);
      if (new_frame) --port.freeze;
    }
    port.last = pads[i];
  }
}

namespace {
// D-pad left on the port driving `p`: on the board or off it (spec: "Mount and dismount").
void handle_toggle(Player& p, uint32_t fp) {
  if (p.port < 0) return;
  Port& port = g_ports[p.port];
  if (!port.toggle) return;
  port.toggle = false;
  const Tunables& t = g_tunables;
  if (!p.on_board) {
    if (!can_mount(p.motion, p.grounded)) return;
    p.on_board = true;
    p.base_friction = rdf(fp + kFtGroundFriction);
    p.carried = rdf(fp + kFtGrVel);             // velocity is kept through the mount
    p.entry_speed = std::fabs(p.carried);
    p.stumbling = false;
    p.frames_on_board = 0;
    p.visual = Player::Visual{};
    port.freeze = t.mount_frames;
    if (host::options.trace_calls) host::log("skate: fighter %08X on the board at %.3f", fp, p.carried);
  } else {
    drop_board(p, "D-pad left");
    port.freeze = t.mount_frames;
  }
}
}  // namespace

namespace {
bool free_mode(Mode m) { return m == Mode::Free || m == Mode::PassThrough; }

// Spec step 5, before the game's physics runs: decide how the board treats the ground this frame
// and put the matching friction into the fighter's own attributes, so every one of the game's
// ground states (idle, crouch, shield, attacks...) slides with no code of its own changed.
void prepare_momentum(Player& p, uint32_t fp) {
  const Tunables& t = g_tunables;
  p.moved = false;
  if (!p.grounded) return;
  // Landing (from a jump, an aerial, a wavedash): the game has just turned the air speed into
  // ground speed, and that is what the board carries from here.
  if (!p.was_grounded) { p.carried = rdf(fp + kFtGrVel); p.entry_speed = std::fabs(p.carried); }
  if (p.group != Group::LandingAir) p.stumbling = false;
  Frame f;
  f.group = p.group;
  f.stumbling = p.stumbling;
  f.stick_x = rdf(fp + kFtLStickX);
  f.carried = p.carried;
  f.base_friction = p.base_friction;
  f.walk_max = rdf(fp + kFtWalkMax);
  const Mode mode = pick_mode(f, t);
  // Anything that happens inside an attack or a roll may move the character as the game likes, but
  // cannot leave the board faster than it was going when that state began.
  if (free_mode(mode) && (!free_mode(p.mode) || p.motion != p.prev_motion)) p.entry_speed = std::fabs(p.carried);
  p.mode = mode;
  p.frame = f;
  wrf(fp + kFtGroundFriction, friction_for(mode, f.group, p.base_friction, t));
  p.friction_written = true;
}

// Spec step 5, where the game turns gr_vel into movement: the board's velocity replaces whatever
// the state computed, so stick input never accelerates and the board never adds speed.
void ground_move(Player& p, uint32_t fp) {
  const Tunables& t = g_tunables;
  if (p.moved) { wrf(fp + kFtGrVel, p.last_write); return; }   // a second call in the same frame
  const float game_vel = rdf(fp + kFtGrVel);
  const Move mv = board_move(p.mode, p.frame, game_vel, p.entry_speed, t);
  wrf(fp + kFtGrVel, mv.write_vel);
  if (!free_mode(p.mode)) {
    // procUpdate adds these to gr_vel after the physics callback: a state's own acceleration.
    wrf(fp + kFtGroundAccel1, 0.0f);
    wrf(fp + kFtGroundAccel2, 0.0f);
  }
  p.carried = mv.carried;
  p.frame.carried = mv.carried;
  p.last_write = mv.write_vel;
  p.moved = true;
}
}  // namespace

namespace {
// Calls a guest function from inside a hook and puts every register back afterwards, so the function
// the hook sits in starts exactly as it would have. Only ever at a hook's entry: the callee's frame
// goes below the current stack pointer, which the hooked function has not used yet.
double call_guest_f(ppc::Context& c, uint8_t* m, uint32_t addr, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
  static ppc::Context saved;   // simulation thread only; big, so not on the host stack
  saved = c;
  c.r[3] = r3; c.r[4] = r4; c.r[5] = r5; c.r[6] = r6;
  c.lr = 0;
  ppc::call(c, m, addr);
  const double result = c.f[1].ps0;
  const uint64_t tb = c.tb;
  c = saved;
  c.tb = tb;
  ppc::update_mxcsr(c);
  return result;
}

// A fighter bone's HSD_JObj, by body part (ftPartsTable[kind]->part_to_joint), or 0.
uint32_t part_jobj(uint32_t fp, int kind, int part) {
  const uint32_t tables = rd32(kFtPartsTable);
  if (kind < 0 || kind >= kKinds || !mapped(tables, (uint32_t)(kKinds * 4))) return 0;
  const uint32_t table = rd32(tables + (uint32_t)kind * 4);
  if (!mapped(table, 12)) return 0;
  const uint32_t part_to_joint = rd32(table + kPartsToJoint);
  if (!mapped(part_to_joint, (uint32_t)part + 1)) return 0;
  const uint32_t joint = rd8(part_to_joint + (uint32_t)part);
  const uint32_t parts = rd32(fp + kFtParts);
  if (!mapped(parts, (joint + 1) * 0x10)) return 0;
  const uint32_t jobj = rd32(parts + joint * 0x10);
  return mapped(jobj, kJObjMtx + 0x30) ? jobj : 0;
}

bool joint_matrix(uint32_t fp, int kind, int part, float out[3][4]) {
  const uint32_t jobj = part_jobj(fp, kind, part);
  if (!jobj) return false;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) {
      out[r][k] = rdf(jobj + kJObjMtx + (uint32_t)(r * 16 + k * 4));
      if (!std::isfinite(out[r][k])) return false;
    }
  return true;
}

// A bone's world position (its matrix's translation).
bool bone_position(uint32_t fp, int kind, int part, Vec3& out) {
  float mtx[3][4];
  if (!joint_matrix(fp, kind, part, mtx)) return false;
  out = {mtx[0][3], mtx[1][3], mtx[2][3]};
  return true;
}

// Spec step 11: the board's sounds are the game's own, played through the fighter so they pan and
// duck like everything else it makes. Ids and volumes are tunables; 0 turns a sound off.
void play_sfx(ppc::Context& c, uint8_t* m, uint32_t fp, int id, int volume) {
  if (id <= 0 || volume <= 0) return;
  call_guest_f(c, m, kFnPlaySfx, fp, (uint32_t)id, (uint32_t)std::min(volume, 127), 64u);
}

bool any_hitbox(uint32_t fp) {
  for (uint32_t i = 0; i < kHitboxCount; ++i)
    if (rd32(fp + kFtHitboxes + i * kHitboxStride) != 0) return true;
  return false;
}

// Spec step 7, run-time half: watch every aerial, note the frames a hitbox is out, and when the move
// ends by itself (into a fall) record how long it lasted. Landing or being hit cuts a move short,
// so those observations add hitbox frames but not the length.
void track_aerial(ppc::Context& c, uint8_t* m, Player& p, uint32_t gobj, uint32_t fp) {
  Player::Aerial& a = p.aerial;
  const bool aerial = is_aerial(p.motion);
  if (a.live && (!aerial || aerial_index(p.motion) != a.index)) {
    const bool natural = p.motion >= ms::AirFirst + 4 && p.motion <= ms::AirFirst + 9;   // Fall .. FallAerialB
    g_framedata.observe(p.kind, a.index, a.first_active, a.last_active, natural ? a.frames : 0);
    g_framedata_dirty = true;
    // Spec step 10: what the body did over the active frames picks this move's trick (a default row
    // is replaced, a hand row only takes its direction, and only when it asks for it).
    if (a.sampler.samples() >= 2 && g_tricks.learn(p.kind, a.index, a.sampler.classify(g_tunables.board_length))) {
      g_tricks_dirty = true;
      const TrickRow row = g_tricks.get(p.kind, a.index);
      host::log("skate: %s %s -> %s (dir %+g, %g turns, %s)", kind_name(p.kind), aerial_name(a.index),
                trick_name(row.trick), row.direction, row.rotations, source_name(row.source));
    }
    a.live = false;
  }
  if (!aerial) return;
  const float anim = rdf(fp + kFtAnimFrame);
  if (!a.live) {
    a = Player::Aerial{};
    a.live = true;
    a.index = aerial_index(p.motion);
    a.end_anim = (float)call_guest_f(c, m, kFnAnimEndFrame, gobj);
    a.sampler.begin(rdf(fp + kFtFacing));
    if (p.on_board) play_sfx(c, m, fp, g_tunables.sfx_pop, g_tunables.sfx_volume);   // the pop
    if (!(a.end_anim > 0.0f && a.end_anim < 1000.0f)) a.end_anim = 30.0f;
  }
  if (anim == a.last_anim && a.frames > 0) return;   // hitlag: the move is frozen
  a.last_anim = anim;
  ++a.frames;
  if (any_hitbox(fp)) {
    if (a.first_active == 0) a.first_active = a.frames;
    a.last_active = a.frames;
    float hip[3][4];
    Vec3 l, r;
    if (joint_matrix(fp, p.kind, kPartHipN, hip) && bone_position(fp, p.kind, kPartLFootJ, l) &&
        bone_position(fp, p.kind, kPartRFootJ, r))
      a.sampler.add(hip, (l + r) * 0.5f);
  }
}

int lcancel_window() {
  const uint32_t common = rd32(kPFtCommonData);
  if (!mapped(common, kFcLcWindow + 4)) return 7;
  const int32_t w = (int32_t)rd32(common + kFcLcWindow);
  return w > 0 && w <= 60 ? w : 7;
}

// Spec step 6, at ftCo_LandingAir_EnterWithMsidLag(gobj, msid, lag): the lag has been decided
// (halved or not) and is about to become the landing animation's rate. A clean L-cancel keeps the
// board rolling; a miss adds stumble_extra_frames and switches to the character's own friction
// until the landing is over. Either way the fighter stays on the board, with no knockdown.
void landing_check(ppc::Context& c, uint8_t* m, Player& p, uint32_t fp) {
  const int msid = (int)c.r[4];
  if (msid < ms::LandingAirN || msid > ms::LandingAirLw) return;   // some other caller
  const bool clean = rd8(fp + kFtX67F) < lcancel_window();          // the game's own test
  if (clean) {
    p.last_landing = Landed::Clean;
    p.stumbling = false;
    play_sfx(c, m, fp, g_tunables.sfx_catch, g_tunables.sfx_volume);    // the clack of a clean catch
  } else {
    p.last_landing = Landed::Stumble;
    p.stumbling = true;
    c.f[1].ps0 += (double)g_tunables.stumble_extra_frames;
    play_sfx(c, m, fp, g_tunables.sfx_scrape, g_tunables.sfx_volume);   // the scrape
  }
}
}  // namespace

namespace {
// ---- spec step 8: where the board is, read after the frame is simulated.
bool read_camera(Camera& cam) {
  cam.valid = false;
  const uint32_t gobj = rd32(kGameCamera);
  if (!mapped(gobj, kGObjHsdObj + 4)) return false;
  const uint32_t cobj = rd32(gobj + kGObjHsdObj);
  if (!mapped(cobj, kCObjView + 0x30)) return false;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) cam.view[r][k] = rdf(cobj + kCObjView + (uint32_t)(r * 16 + k * 4));
  cam.fov_deg = rdf(cobj + kCObjFov);
  cam.aspect = rdf(cobj + kCObjAspect);
  cam.near_z = rdf(cobj + kCObjNear);
  cam.valid = cam.fov_deg > 1.0f && cam.fov_deg < 179.0f && cam.aspect > 0.1f && cam.aspect < 10.0f;
  return cam.valid;
}

const char* landing_name(Landed l) { return l == Landed::Clean ? "clean" : l == Landed::Stumble ? "stumble" : "-"; }

void board_view(Player& p, BoardView& v) {
  const Tunables& t = g_tunables;
  v = BoardView{};
  if (!p.fp) return;
  const uint32_t fp = p.fp;
  v.present = true;
  v.on_board = p.on_board;
  v.port = p.port;
  v.kind = p.kind;
  v.motion = (int)rd32(fp + kFtMotion);
  v.grounded = rd32(fp + kFtGroundAir) == 0;
  v.landing = landing_name(p.last_landing);
  const float facing = rdf(fp + kFtFacing);
  const Vec3 pos{rdf(fp + kFtPos), rdf(fp + kFtPos + 4), rdf(fp + kFtPos + 8)};
  v.ground = pos;
  v.velocity = v.grounded ? rdf(fp + kFtGrVel) : rdf(fp + kFtSelfVel);
  if (!p.on_board) { v.state = "off"; return; }
  v.state = p.stumbling ? "stumble" : v.grounded ? mode_name(p.mode) : "air";
  Player::Visual& vis = p.visual;
  if (v.grounded) {
    const Pose rest = rest_pose(pos, facing, rdf(fp + kFtFloorNormal), rdf(fp + kFtFloorNormal + 4), t);
    // Down from the air (mid-trick or not): snap to the catch pose over catch_snap_frames.
    if (vis.was_air) { vis.snap_from = vis.last; vis.snap_left = t.catch_snap_frames; }
    if (p.stumbling) {
      v.pose = compose(rest, stumble_pose(vis.stumble_frame++, facing));
    } else {
      vis.stumble_frame = 0;
      v.pose = rest;
    }
    if (vis.snap_left > 0) {
      v.pose = blend(vis.snap_from, v.pose, 1.0f - (float)(vis.snap_left - 1) / (float)t.catch_snap_frames);
      --vis.snap_left;
    }
  } else {
    Vec3 l, r, feet = pos;
    if (bone_position(fp, p.kind, kPartLFootJ, l) && bone_position(fp, p.kind, kPartRFootJ, r)) feet = (l + r) * 0.5f;
    v.pose = air_pose(feet, facing, t);
    vis.snap_left = 0;
    // Spec step 9: an aerial on the board plays its trick, timed to the move's own frames.
    const Player::Aerial& a = p.aerial;
    if (a.live && is_aerial(v.motion) && a.index == aerial_index(v.motion)) {
      const float u = std::clamp(rdf(fp + kFtAnimFrame) / a.end_anim, 0.0f, 1.0f);
      const TrickRow row = g_tricks.get(p.kind, a.index);
      const ActiveWindow w = active_window(g_framedata, p.kind, a.index, t.default_active_start, t.default_active_end);
      v.pose = compose(v.pose, trick_pose(row, u, w, t));
      v.trick = trick_name(row.trick);
      v.phase = phase_name(trick_time(u, w).phase);
      v.phase_t = u;
      v.state = "trick";
    }
  }
  vis.was_air = !v.grounded;
  vis.last = v.pose;
}

// Builds the snapshot for the renderer from the state the last frame left behind.
void publish() {
  Snapshot s;
  s.frame = ++g_frame;
  s.active = g_hooks_live;
  s.tunables = g_tunables;
  s.framedata_rows = g_framedata.size();
  s.trick_rows = g_tricks.size();
  s.frame_advance = g_frame_advance.load();
  if (g_hooks_live) {
    read_camera(s.camera);
    for (int i = 0; i < kSlots; ++i) board_view(g_players[i], s.boards[i]);
  }
  std::lock_guard<std::mutex> lock(g_snapshot_lock);
  g_snapshot = s;
}
}  // namespace

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(g_snapshot_lock);
  return g_snapshot;
}

void hook_enter(ppc::Context& c, uint8_t* m, Site site) {
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
      track_aerial(c, m, p, gobj, fp);
      handle_toggle(p, fp);
      if (p.on_board) {
        p.group = classify(p.motion);
        // Hit, grabbed, ledge or death: the board goes, with no penalty beyond the hit itself. A
        // percent that went up outside the shield catches hits that skip the usual damage states.
        if (drops_board(p.group)) drop_board(p, group_name(p.group));
        else if (percent > p.percent + 0.01f && p.group != Group::Shield) drop_board(p, "took damage");
      }
      p.percent = percent;
      if (p.on_board) {
        ++p.frames_on_board;
        prepare_momentum(p, fp);
        // The roll: a tick every roll_interval_frames while the board is rolling along the ground.
        const Tunables& t = g_tunables;
        const bool rolling = p.grounded && !p.stumbling &&
                             (p.mode == Mode::Roll || p.mode == Mode::Brake || p.mode == Mode::Push);
        if (rolling && std::fabs(p.carried) >= t.roll_min_speed) {
          if (++p.roll_counter >= t.roll_interval_frames) { p.roll_counter = 0; play_sfx(c, m, fp, t.sfx_roll, t.roll_volume); }
        } else {
          p.roll_counter = t.roll_interval_frames;   // the first tick comes as soon as it rolls again
        }
      }
      break;
    }
    case Site::GroundMove:
      if (p.on_board && p.grounded) ground_move(p, fp);
      break;
    case Site::LandingAir:
      if (p.on_board) landing_check(c, m, p, fp);
      break;
  }
}

void set_data_dir(const std::string& dir) { g_dir = dir.empty() ? "skate" : dir; }
const std::string& data_dir() { return g_dir; }
void load_files() {
  load_tunables();
  load_framedata();
  load_tricks();
}
void request_reload() { g_reload_requested.store(true); }
const Tunables& tunables() { return g_tunables; }

}  // namespace skate
#endif  // SKATE_MOD
