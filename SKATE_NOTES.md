# Skateboard mod — engineering notes

Working notes for the `skateboard-mod` branch (spec: "Melee Skateboard Mod — Build Spec").
Everything here is gated by the `SKATE_MOD` compile flag (CMake option `MELEE_SKATE_MOD`) and a
runtime switch (PC settings → Game → "Skateboard mod", `skatemod` in port-settings.ini,
`--skate` / `--no-skate` on the command line).

## 1. How this port differs from "a decomp project"

melee-unlocked does **not** build the game from decompiled C. `port/recomp/recomp.py` translates the
retail `main.dol` (plus Slippi's Gecko codes) into C++ at build time (`port/generated/`, not in git).
So there are no fighter structs or action-state tables to edit. Game behaviour can only be changed by:

1. **Host hooks emitted into the translated code.** `port/recomp/emit.py` already inserts an RAII
   object at the top of chosen guest functions (`gx::RenderObserver` for the renderer). Its
   constructor runs at function entry with the live `ppc::Context` (arguments in `c.r[3..]`,
   floats in `c.f[1..]`), its destructor at every exit. The skate mod adds a second list of this
   kind (`skate::Hook`, `port/runtime/ppc/skate_hook.h`), wrapped in `#ifdef SKATE_MOD` so the
   generated code compiles either way.
2. **Guest memory reads/writes** from host code (`host::rd32/wr32`), using NTSC 1.02 offsets from
   the decomp (github.com/doldecomp/melee) and addresses from `port/recomp/GALE01_symbols.txt`.
3. **Pad injection** in `HLE(PADRead)` (`port/runtime/hle/hle_pad.cpp`), which runs once per
   frame before the game reads input. The L-cancel helper already does this.

The recompiler must be re-run for hook changes to take effect: `build.bat` always does.

## 2. Map of the systems the spec touches

| Spec concept | Where it lives (NTSC 1.02) |
|---|---|
| Player slots | `player_slots` @ `0x80453080`, stride `0xE90`; `+0x00` state (0 = empty), `+0x08` slot type (0 human), `+0x46` controller index, `+0xB0` player_entity[0] (fighter GObj). See `lcancel.cpp`. |
| Fighter from GObj | `gobj+0x2C` user_data → `Fighter*`; a Fighter points back at its GObj at `fp+0x0`. |
| Action state | `fp+0x10` motion_id (`ftCommon_MotionState`, decomp `ft/kinds/ftCommon/forward.h`). Values used: Dead 0–10, Sleep 11, Rebirth 12–13, Wait 14, Walk 15–17, Turn 18, TurnRun 19, Dash 20, Run 21, RunDirect 22, RunBrake 23, KneeBend 24, Fall 29, Squat 39–41, Landing 42, LandingFallSpecial 43 (wavedash/waveland), Attack11 44 … AttackLw4 64, AttackAirN–Lw 65–69, LandingAirN–Lw 70–74, Damage 75–91, Guard 178–182, Down 183–199, ShieldBreak 205–211, Catch 212–218, Throw 219–222, Capture 223–232, Escape 233–236, Thrown 239–251, Cliff 252–263. |
| Dispatch | `Fighter_ChangeMotionState` @ `0x800693AC` swaps the state's callbacks. The per-frame fighter physics step is `Fighter_procUpdate` @ `0x8006B82C`: `phys_cb` → knockback decay → `gr_vel += ground accel` → `self_vel += accel` → `cur_pos += self_vel + kb`. |
| Ground friction / velocity | `fp+0xEC` `gr_vel` (signed speed along the floor). Friction comes from the fighter's **copy** of its attributes: `fp+0x128` `co_attrs.ground_friction` (`fp+0x110` is `co_attrs`, `+0x118` walk_max_vel). Every ground state reads that copy, e.g. `ft_80084F3C` → `ftCommon_CalcGroundAccel_Deaccel`. |
| Ground velocity → movement | `ftCommon_SetSelfMovementFromGroundedMovement` @ `0x8007CB74` converts `gr_vel` into `self_vel` along the floor normal (`fp+0x844`). Every grounded physics callback ends by calling it. |
| Air / ground | `fp+0xE0` ground_or_air (0 ground, 1 air). Position `fp+0xB0`, self velocity `fp+0x80`, facing `fp+0x2C` (±1). |
| Input seen by the fighter | `fp+0x620` lstick (float x, y); `fp+0x65C` held buttons; `fp+0x668` pressed. Raw pads are `host::PadState` in `HLE(PADRead)`; D-pad left is pad bit `0x0001`. |
| L-cancel | `fp+0x67F` counts frames since the last L/R/Z press. `ftCo_LandingAir_EnterWithLag` @ `0x8008D5FC` halves the aerial's landing lag when `x67F < ftCommonData.xE4` (7), then calls `ftCo_LandingAir_EnterWithMsidLag(gobj, msid, f1 = lag)` @ `0x8008D708`, which sets the landing animation rate to `(anim_end + 0.1) / lag`. So the lag is one float argument we can change at that entry. |
| Per-move frame data / hitbox timing | Not stored as tables: hitboxes are switched on and off by each move's subaction script. At run time `fp+0x914` holds 4 `HitCapsule`s (0x138 each, `+0x0` state, 0 = off). Animation frame `fp+0x894`, end frame from the joint's animation (`ftAnim_8006F484`). Active frames are therefore *observed*, see step 7. |
| Bones | `fp+0x5E8` → `FighterBone[]` (0x10 each, `+0x0` `HSD_JObj*`). Part → bone: `ftPartsTable` @ `0x804D6544` → `[kind]` → `part_to_joint[part]`. Parts used: HipN 4, LFootJ 10, RFootJ 15. `HSD_JObj+0x44` is the joint's world matrix (3×4), updated when the model is drawn. Character kind `fp+0x04`. |
| Camera | `game_camera` @ `0x80452C68` → `+0x0` GObj → `+0x28` `HSD_CObj*`: `+0x38` near, `+0x3C` far, `+0x40` fov (deg), `+0x44` aspect, `+0x54` view matrix (3×4). |
| Drawing | Both renderers call `settings_frame` (`port/runtime/gx/pc_settings.cpp`) between `ImGui::NewFrame` and `ImGui::Render` every presented frame. Overlays (L-cancel notice, input display, Lab view on its branch) draw there with Dear ImGui. |
| Sound | `ft_PlaySFX(Fighter*, int id, u8 vol, u8 pan)` @ `0x80088148`, callable with `ppc::call` from inside a hook (context saved and restored around it). |
| Netplay | Slippi simulates both players from exchanged inputs; any change not carried by inputs desyncs. The skate mod changes the simulation, so it is **forced off online** (`slippi::online::is_online_match()` / `in_online_menus()`), and it is out of scope for v1 anyway. Offline `.slp` recordings made with it on will not replay correctly in stock Slippi. |

## 3. Design as built (and where it departs from the spec's wording)

**Hooks (emit.py):**
- `Fighter_procUpdate` entry (`Site::ProcUpdate`): per fighter, per frame, before its physics. Runs
  the skate state machine: mount/dismount, friction attribute, dismount-on-hit/ledge/death,
  sounds, and board/trick bookkeeping.
- `ftCommon_SetSelfMovementFromGroundedMovement` entry (`Site::GroundMove`): the "never add speed"
  clamp. When mounted, `gr_vel` may shrink (brake, stumble, waveland friction) but never grow or
  flip past the board's carried velocity.
- `ftCo_LandingAir_EnterWithMsidLag` entry (`Site::LandingAir`): records clean/miss and adds
  `stumble_extra_frames` to `f1` (the lag) on a miss.

**Momentum.** Zero friction is done by writing `0.0` into the fighter's own `co_attrs.ground_friction`
while rolling (restored exactly on dismount, braking, stumbling, wavedash landing, etc.). This keeps
every one of the game's own ground states consistent without hooking each of them. The clamp hook
stops Dash/Walk/Run from *adding* speed, so "stick input does not accelerate" holds.

**Mount lag.** `mount_frames` of lag is implemented as input freezing: for those frames the pad the
game reads keeps the previous frame's stick and only buttons already held (no new presses).
The fighter keeps its state and velocity but can't start a new action. There is no new action state.

**SkateStumble.** Implemented as the vanilla missed-L-cancel landing with the lag float raised by
`stumble_extra_frames` (8), plus normal friction for the duration. It is not a separate action
state (adding real action states to the translated game is a much bigger project). The overlay
and the board visuals still show it as "stumble".

**Board visuals.** Drawn with Dear ImGui over the game image, projected with the game camera read
from guest memory. **No depth test**: the board is never hidden behind stage geometry or other
characters, and it updates at the 60 Hz simulation rate (the character is sub-frame interpolated
at high refresh rates, the board is not). Real 3D drawing inside the D3D12/D3D11 backends is the
natural v2.

**Frame data.** Collected at run time: every time an aerial is performed, the frames on which a
hitbox is live are observed and merged into the table; F8 writes `skate_framedata.json`.
The trick timing uses that table and falls back to a default window before the move has been seen.

**Trick table auto-generator.** Also at run time: during an aerial's active frames the hip and
foot bone world matrices are sampled; the accumulated rotation and foot travel pick the trick
(spin about the forward axis → varial, about the vertical axis → shove-it, flip → kick/heelflip,
downward foot travel → stomp, little motion → grab). Rows marked `"source": "hand"` are never
overwritten. The shipped `skate/skate_tricks.json` is seeded by `tools/skate_seed_tricks.py` with
per-aerial defaults (`"source": "default"`) and Falco fair hand-set to a 1-rotation varial.

## 4. Using it

Build as usual (`build.bat <iso>`; it re-runs the recompiler, which is what inserts the hooks) and
play with `play.bat`. The mod is on by default on this branch (PC settings → Game → Skateboard).

| Key | What it does |
|---|---|
| D-pad left | Mount / dismount (grounded; `mount_frames` of lag either way) |
| F4 | Skate debug overlay: per player on_board, velocity, state, last landing, trick/phase; board debug draw |
| F5 | Reload `skate/skate_tunables.json` and `skate/skate_tricks.json` live |
| F6 / F7 | Frame advance: hold the game / step one frame |
| F8 | Save learned tricks and recorded frame data (`skate_tricks.json`, `skate_framedata.json`) |

Files in `skate/` (working directory; `--skate-dir` overrides): `skate_tunables.json` (every
number), `skate_tricks.json` (seeded by `tools/skate_seed_tricks.py`), `skate_framedata.json`
(written by F8). Filling the trick table "for every row" happens as you play: the first time each
character's aerial is used with the mod on, its default row is replaced by what the bone sampler
found (the log says which). Press F8 afterwards to keep it.

Suggested first session: Falco on FD vs a still CPU, F4 open. Dash, D-pad left, let go of the stick
(should keep rolling at dash speed), hold back (brakes), full hop fair (varial; the log reports the
learned direction), land with and without L-cancel (clean / stumble in the overlay), shield, get hit.

## 5. Acceptance checklist status

| Item | Status |
|---|---|
| Builds and runs normally with `SKATE_MOD` off | Every call site and emitted hook is `#ifdef SKATE_MOD`; the emitted code was compiled both ways. Build with `-DMELEE_SKATE_MOD=OFF` to confirm on Windows. |
| D-pad left mounts from standing, dash, run, wavedash | Implemented; state list unit-tested. Needs in-game check. |
| Dash speed kept; standing stays still | Unit-tested (momentum rule). |
| Speed never above entry | Unit-tested, including dash/run/dash-attack trying to add speed. |
| Back brakes at normal deceleration | Unit-tested. |
| Aerials unchanged | Nothing touches attack data, hitboxes or frame data; only friction, `gr_vel` and landing lag on a miss. Compare with the mod off. |
| Clean L-cancel rolls, miss stumbles | Implemented at the game's own L-cancel test; needs in-game check. |
| Hit / ledge removes the board | Implemented (action-state groups + percent increase). |
| Shield normal while mounted | Shield states keep the board; nothing about shielding is changed. |
| Board visible, under feet, trick on every aerial | Implemented; projection unit-tested with a synthetic camera. Needs in-game check (see §6). |
| Falco fair varial in his spin direction | Hand row, direction learned from his hip bone on the first fair. |
| Hot-reload | F5, implemented. |
| Resets on death, respawn, new match | Implemented (fighter pointer / kind / death states). |
| No frame drops with two players | Per frame: a few dozen guest reads per fighter; one guest call per aerial; the board is ~20 ImGui quads. No allocation in the hooks; files only on F5/F8. |

## 6. Things I could not verify in this environment

This branch was written without a Windows toolchain, a GPU, or a Melee ISO. The pure logic
(tunables, rules, tables, tricks, sampler, projection) is unit-tested (`port/tests/skate_core_test.cpp`,
target `port_skate_core_test`, 102 checks, passing with GCC 13 and Clang). The emitted hook code was
generated with the real `emit.py` and compiled with and without `SKATE_MOD`.
The guest bindings were syntax-checked but never run. Expect to tune:

- The sign/axes of the camera projection (board may appear mirrored or offset if a convention is
  off — `board_debug` in the F4 overlay shows the projected ground point to check against).
- SFX ids in `skate/skate_tunables.json` are placeholders from ids the decomp shows in use.
- Motion-id ranges for dismount-on-hit, if a character-specific state slips through.
- The mount lag freezes the pad the game reads; if a held input feels wrong around a mount, that is
  where to look (`freeze_pad` in `skate.cpp`).
- Stick input still plays walk/dash animations in place when the board is stopped (the velocity is
  held, the action state is not). A real "on the board" idle state would need new action states.
- Ice Climbers: only Popo gets a board (Nana is a sub-fighter, not a player slot).
