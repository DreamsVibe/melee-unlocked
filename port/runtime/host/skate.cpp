// Skateboard mod: guest bindings. See skate.h and SKATE_NOTES.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef SKATE_MOD
#include "skate.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "slippi_online.h"

namespace skate {
namespace {

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

void set_data_dir(const std::string& dir) { g_dir = dir.empty() ? "skate" : dir; }
const std::string& data_dir() { return g_dir; }
void load_files() { load_tunables(); }
void request_reload() { g_reload_requested.store(true); }
const Tunables& tunables() { return g_tunables; }

}  // namespace skate
#endif  // SKATE_MOD
