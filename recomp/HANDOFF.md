# Falcon 3.0 Native Host — Technical Handoff & Reverse-Engineering Reference

This document captures all architectural decisions, reverse-engineered binary/memory facts, solved low-level pitfalls, and the prioritized roadmap so any agent or engineer can seamlessly resume development.

> **Multi-Title Companion Guide:** See [HORNET_MIG_PORTING_GUIDE.md](HORNET_MIG_PORTING_GUIDE.md) for the exact step-by-step playbook to bring **Hornet: Naval Strike (`HORNETCD.EXE`)** and **MiG-29: Deadly Adversary (`MIGCD.EXE`)** to 100% native 3D, flight-model, HUD, and HOTAS parity.

---

## 1. Project Goals & User Preferences
* **Core Goal:** Decouple Falcon 3.0 (`FALCONCD.EXE`) from DOSBox, upgrade rendering to modern hardware-accelerated HD Retro 3D, and build toward **OpenXR VR** support — using a **Hybrid Native Host ("Ship of Theseus")** architecture in C++20 (SDL3 + OpenGL + Dear ImGui), keeping Falcon's original HUD/cockpit, radar, weapons, AI, and dynamic campaign 100% intact.
* **Multi-Title Scope:** Keep engine systems signature-scanned and modular so `HORNETCD.EXE` and `MIGCD.EXE` (both already booting in our host!) share the same 3D renderer, audio proxy, HOTAS mapper, SAM mod, and UDP multiplayer.
* **Controls:** Full **multi-HOTAS** support (unlimited simultaneous USB sticks, throttles, rudder pedals, button boxes — verified with WinWing Orion 2 + Virpil MongoosT-50CM3 + MFG Crosswind V2/3) with deadzone/invert/curve tuning, delta (rising-edge) button/hat capture (ignoring always-ON HOTAS toggle switches), pinky shift layer, JSON profiles (`falcon3_hotas.json`), and automatic **zero-calibration bypass** of Falcon's 1991 joystick wizard.
* **Audio:** Primary **Roland CM-32L / MT-32** via embedded Munt `libmt32emu` using the user's own valid ROMs (`--rom-dir`, `MT32_ROM_DIR`, or `F12 -> Audio`) + **64-Voice Pre-Baked CM-32L Sample Proxy** (zero cutouts in combat, zero copyrighted audio shipped) + **Sound Blaster 16** (8-bit DMA & PIT0 Direct DAC voice comms + legacy OPL3 FM option).
* **Multiplayer:** Replace legacy Novell IPX / serial transport with modern **UDP/IP** networking + Host/Join Lobby UI (`src/net/net_udp.cpp` bridged into `INT 7Ah`).

---

## 2. Executable & Binary Architecture Findings
All three Falcon Gold executables live in `game_data/`:
* `FALCONCD.EXE` (1,373,671 B): 118,609 B resident MZ image + 1,243,798 B overlays, `CS:IP=1AA3:076C`, `SS:SP=752C:0800`, 2,717 MZ relocations.
* `HORNETCD.EXE` (1,412,635 B): `CS:IP=1A99:076C`.
* `MIGCD.EXE` (1,369,383 B): `CS:IP=1A4E:076C`.

Key binary traits:
* **16-bit Real-Mode MS-DOS MZ** linked with **Pocket Soft `.RTLink/Plus`** (`INT ABh` dynamic overlay thunks, reads `argv[0]` from PSP environment block to open its own EXE for overlay paging, caches overlays in EMS).
* **Graphics Driver:** Loads `VGRAPH` at runtime via `INT 21h AX=4B03h` (DOS Load Overlay) into conventional RAM.
* **Video Modes:**
  * Unchained 256 KB 4-plane **VGA Mode X** (`Sequencer reg 4 bit 3 = 0`, `Map Mask reg 2`, `Read Map GC reg 4`, CRTC `0x0C/0x0D` page flipping across `0x0000`/`0x4000`/`0x8000`).
  * **Title / Credits screen uses `320×400`** (clears double-scan bit in CRTC `reg 0x09 = 0x40`); War Room, Config menus, and 3D Cockpit use **`320×200`** (`reg 0x09 = 0x41`).
* **FPU:** Executes `FINIT` (`9B DB E3`) + `FSTCW` (`9B D9 7E 00`) during startup to detect an 8087/287/387. Currently `INT 06h` decodes and skips 8087 ESC opcodes (`D8..DF`) cleanly so Falcon uses its built-in software flight math (or our optional 240 Hz C++ Native F-16C Flight Model Mod).
* **Interrupts Hooked by Guest:**
  * `INT 00h` (`107C:342B`), `INT 08h` (`107C:343F` game timer / `79E:C57` MIDI timer / `79E:2E0` flight & SB DAC timer), `INT 09h` (`107C:5C7D` keyboard ISR), `INT ABh` (`.RTLink/Plus`).
  * Mouse uses **`INT 33h AX=000Ch`** real-mode FAR event callback (`0110:002C`, mask `0x7F`), NOT just polling `0003h`.

---

## 3. Critical Low-Level Pitfalls Already Solved (Do Not Regress!)

1. **`libx86emu` Upstream Bug in `outs()` (`OUTSB` read `ES:SI` instead of `DS:SI`):**
   * Upstream `libx86emu/prim_ops.c` hardcoded `R_ES_INDEX` in `outs()`. Falcon's `VGRAPH` uses `OUTSB` to port `0x3C9` for cockpit palettes.
   * **Permanent fix:** `recomp/compat/x86emu_prim_ops.c` replaces upstream `prim_ops.c` in `CMakeLists.txt`.
2. **`libx86emu` `x86emu_intr_raise()` Swallows Guest `INT 21h`:**
   * Raising an external hardware IRQ right before a guest `INT 21h` silently dropped `INT 21h`, causing `.RTLink/Plus` `eov0003`.
   * **Permanent fix:** `DosMachine::deliver_hw_irq()` manually pushes `FLAGS/CS/IP`, clears `IF/TF`, and sets `CS:IP` directly from the IVT between instruction slices when `IF == 1`. **Never use `x86emu_intr_raise()` for hardware IRQs.**
3. **Gameport `0x201` Timer MUST Use `emu_->x86.R_TSC`, NOT `total_insns_`:**
   * `total_insns_` only updates at the end of an `x86emu_run()` slice, so measuring `total_insns_ - joy_trigger_insn_` stayed `0` throughout Falcon's 4-instruction `IN AL, DX / STOSB / AND AL, AH / LOOPNZ` loop (`0x3DDD`), pegging all 4 axes at `1339..2048` (`+9772`).
   * **Permanent fix:** `port_in8(0x201)` and `port_out8(0x201)` measure elapsed instructions using live `emu_->x86.R_TSC`, giving 1:1 deterministic counts (`1..254`, center `127 -> norm 0`).
4. **Gameport Axis 3 (Throttle) Polarity & RAM Enable Flags:**
   * Falcon's in-flight throttle routine (`0x2C55E -> 0x15022 -> 0x2C583`) checks **both** `[joy_base + 0x71] >= 3` **and** `[joy_base + 0x74] != 0`, and **negates** Axis 3 (`DX`): `raw = 254 (+2048)` = `0% IDLE`, `raw = 127 (0)` = `MIL (183)`, `raw = 1 (-2048)` = `AB 5 (305)`.
   * **Permanent fix:** `HotasMapper::gameport_axis(3)` emits `1.0f - throttle * 2.0f`, and `DosMachine::apply_auto_joystick_calibration()` writes `joystick.def` (100 B) **and** patches `[base+0x04..0x67]` plus `[base+0x71]=mode`, `[base+0x72]=mode`, `[base+0x74]=0xFF`, `[base+0x75]=0xFF`.
5. **HOTAS Always-ON Toggle Switches During Bind Capture:**
   * Real HOTAS throttles (Virpil, Warthog, WinWing) hold multiple physical toggle switches `true` at rest.
   * **Permanent fix:** `HotasMapper::start_button_capture()` and `start_pinky_capture()` snapshot live button/hat baselines and only bind on a **new rising-edge delta** (`cur && !baseline`), clearing a toggle's baseline if flipped OFF.
6. **DOS File Handles & I/O Semantics:**
   * Always reuse the lowest free handle `>= 5` (`alloc_file_handle()`), backed by `std::FILE*`. `INT 21h AH=41h` (delete), `AH=45h/46h` (dup/dup2), and `AX=4B03h` (load overlay) are all implemented.
7. **Real-Time Timing Decoupling, Auto-Turbo, Fast-Boot & Audio:**
   * `DosMachine::step_realtime(dt_sec)` drives CPU (`3.5 MIPS` menus, `20 MIPS` in-flight Auto-Turbo with `11 ms` guardrail), PIT IRQ0 (up to `512` ticks/frame for `11,256.4 Hz` SB Direct DAC `0x10` voice comms via `INT 08h 079E:02E0`), BDA `0x046C` (`18.2065 Hz`), and VGA `0x3DA` vblank (`70.08 Hz`) strictly from wall-clock `dt`.
   * **CM-32L SysEx SFX Bank (`MUSIC\Y.MID`):** Streams **83 Roland DT1 SysEx messages** right before `WARROOM`. Never drop SysEx or Program/CC messages during `fast_boot_mute_` — apply SysEx immediately via `playSysexNow()` and cache in `sysex_history_` for ROM reloads and the Sample Proxy baker.
   * **War Room `A.MID` Pan Leak:** `A.MID` leaves `CC 10` Pan on Ch 2 (`Jetengine`) at `22/127` (hard left). `AudioEngine::set_in_flight(true)` resets `chan_pan_[1..7] = 64` (center), `chan_expr_[1..7] = 127`, and `chan_bend_semi_[1..7] = 0`.
   * **Fast-Boot Skip Intro** (`--test-fastboot`): ~1.8 s cold boot to War Room with `SysEx=83`.
8. **3D Model File Prefix (`objects` & `mapobj`):**
   * Every model record in BOTH `game_data/objects` and `game_data/mapobj` starts with a **2-byte record-length prefix** (`u16 @ o0 == mlen - 2`, so `base = o0 + 2`!), articulated/swing-wing models use LOD indirection (`w1 == 0 → lod_off = w0`), local axes map to `(OpenGL X = +vx, Y = -vy, Z = -vz)`, and all **8 octant face lists** must be walked deduplicated by offset.
9. **Safe Deferred Input, 4:3 Mouse Mapping & Segment Registers:**
   * Queue keyboard scancodes and mouse callbacks and dispatch strictly inside `step()`/`step_realtime()` when `IF == 1 && !in_mouse_cb_`. Always use `x86emu_set_seg_register()` when changing `CS/DS/ES/SS` from C++.

---

## 4. Verified Reverse-Engineered Memory & Asset Map (`FALCONCD.EXE`)

### Live F-16 6-DOF Flight State Struct (Physical RAM `0x47364`, Stride `0x1BA`)
| Physical Addr | Offset | Type | Units / Notes |
|---|---|---|---|
| `0x47364` | `+0x00` | `int32_t` | World X — North (`0.5 ft` units) |
| `0x47368` | `+0x04` | `int32_t` | World Y — East (`0.5 ft` units) |
| `0x4736C` | `+0x08` | `uint16_t` | Altitude (`0.5 ft` units) |
| `0x47370` | `+0x0C` | `uint16_t` | Heading / Yaw — 14-bit BAM (`(val & 0x3FFF) * 360 / 16384`) |
| `0x47372` | `+0x0E` | `uint16_t` | Pitch — 14-bit signed BAM |
| `0x47374` | `+0x10` | `uint16_t` | Roll / Bank — 14-bit signed BAM |
| `0x47376` | `+0x12` | `uint16_t` | Mach × 100 |
| `0x47378` | `+0x14` | `uint16_t` | Indicated Airspeed (KIAS) |
| `0x4737C` | `+0x18` | `uint16_t` | Engine RPM % (`0..100`) |
| `0x47382` | `+0x1E` | `uint16_t` | Afterburner stage (`0..5`) |
| `0x47392` | `+0x2E` | `uint16_t` | Real Throttle command (`0..183` = IDLE..MIL, `183..305` = MIL..AB5) |
| `0x4739C` | `+0x38` | `int16_t` | Roll rate / lateral angular velocity |
| `0x4739E` | `+0x3A` | `uint16_t` | Flight-path Yaw (14-bit BAM, drives HUD FPM) |
| `0x473A0` | `+0x3C` | `uint16_t` | Flight-path Pitch (14-bit signed BAM, drives HUD FPM) |
| `0x473A2` | `+0x3E` | `uint16_t` | MSL Altitude (`val * 4` ft) |
| `0x473A4` | `+0x40` | `uint16_t` | AGL Altitude (`val * 4` ft) |
| `0x473A6` | `+0x42` | `uint16_t` | Gross Weight (lbs) |
| `0x473B0` | `+0x4C` | `uint16_t` | TAS (ft/s); `+0x50/+0x52/+0x54` = `vn/ve/vu * 0.25` |
| `0x47432` | `+0xCE` | `int16_t` | G-load × 10 |
| `0x47435` | `+0xD1` | `uint8_t` | Afterburner active flag (`0xFF` = AB lit, `0x00` = MIL/dry) |

### Other Verified RAM Addresses & Signatures
* **`0x45C10` (20 B):** Live Active 3D Camera Struct (`cam_x, cam_y, cam_z`, `cam_yaw/pitch/roll` 14-bit BAM) — tracks Cockpit, Side/Rear, Padlock, Chase, Orbit, Flyby, Enemy, Missile views.
* **`0x13E28` (`uint16_t`):** Active View Mode (`0` = Forward Cockpit `'3'`, `2` = Left Console `'4'`, `4` = Right Console `'6'`, `6` = Rear 6-o'clock `'5'`, `18` = External/Chase/Ground/Track, `26` = Padlock `'8'`).
* **`0x46190..0x4619C` & `0x46228`:** Live Padlock target vectors (`0x46190` = shift; `0x46192/94/96` = body-frame `Right/Down/Forward`; `0x46198/9A/9C` = world-space `dE/-dZ/dN`) and ASCII target callsign (`"SU25"`, `"SAM (R)"`, etc.), used to resolve unclamped 3D Padlock targets at any heading and range (0.05–80+ NM).
* **`0x43AB4` (stride `0x2C = 44 B`, `16` slots `0..15`):** Live 3D Aircraft Array (slot `0` = player `flags=0x4154`, slots `1..15` = AI aircraft `flags=0x4114`; `+0x26 = 0x8000 | type_id`).
* **`0x4403E` (stride `0x36 = 54 B`, `100` slots `0..99`):** Live 3D Ground Units Array — armored columns, tanks, APCs, ZSU-57 AAA, and SAM batteries (`flags=0x4014`, `+0x1E = HP`, `+0x26 = 0x8121..0x8130`).
* **`0x42170` (stride `0x2A = 42 B`, `104` slots = pool slots `40..143`):** Active In-Flight Missile & SAM Array (`flags=0x4015`, `+0x00..0x0B = wx/wy/wz`, `+0x0C/+0x0E/+0x10 = yaw/pitch/roll`, `+0x16 = age_ticks`). Rendered with long-range blazing rocket flares + persistent billboarded 3D smoke trail ribbons visible out to 25+ NM.
* **`0x46CF8` (`uint16_t`, mirrored at `0x4640A` / `0x4A09A`):** Selected stores type ID, indexing the EXE's stores name table (`AIM9P=0, AIM9M=1, AIM120=2, M61=3, M61=4, LAU=5, AGM88=6, AGM45=7, AGM65B=8, GBU15=9, AGM65D=10, DUR=11, MK82HD=12, MK82=13, ...`). M61A1 cannon selected ⇔ ID `3` or `4`. Verified via headless RAM diffs while cycling with `Enter` / `Backspace`. (The older `0x68D96 == 0 && 0x68D9A == 0` check was wrong: both stay `0` for every weapon.)
* **SAM / 16-Missile Table (Signature-Scanned Across All 3 EXEs):**
  * Signature `"AIM-9P  AIM-9M  "` (`FALCONCD`: `0x5DA0A`, `HORNETCD`: `0x5F9DA`, `MIGCD`: `0x61B04`).
  * `sig - 0x2A6`: 16 × 18 B missile records (`sig - 0x1CE` = `SAM (R)`, `sig - 0x1BC` = `SAM (IR)`).
  * `sig - 0xB6`: 16 × 2 B countermeasure resistance table (`sig - 0x9E` = `SAM (R)`, `sig - 0x9C` = `SAM (IR)`).
  * Patched non-destructively in RAM via `DosMachine::apply_sam_mod_in_ram()` (default preset: Historical F3MSL SA-7B + Gulf War Radar SAMs, fixes vanilla's broken `0x6400` 80× SA-7 agility bug).
* **Joystick Driver Segment (Signature-Scanned Across All 3 EXEs):**
  * Signature `"KENR\xCC\x00\xCC\x00"` preceded by axis-mask table `{01 03 07 0B 0F}` at `base + 0x6C` (`FALCONCD`: `0x3D00`, `HORNETCD`: `0x3D10`, `MIGCD`: `0x3CD4`).
  * `base + 0x04..0x67` = exact 100-byte `joystick.def` (`4 × 22 B` axis structs + coolie thresholds + mode byte `98`), `base + 0x71/0x72` = active mode (`1=XY, 2=+Yaw, 3=+Thr, 4=All 4`), `base + 0x74 = 0xFF` (initialized), `base + 0x75 = 0xFF` (polling active).

### Verified Native 3D Asset Formats (`game_data/`, Shared Across All 8 Theaters & 3 Titles)
All 8 theaters (`BOS`, `HOK` Japan, `IRQ` Kuwait, `ISR` Israel, `KOR` Korea, `NEL` Nellis, `PAK` India/Pakistan, `PAN` Panama) use identical formats:
* **`ISLDAT.*`:** Native 3D layered surface polygons in exact `0.5 ft` world coordinates; face-group header `w1` (`gp+2`) selects exact painter's layer: `w1 == 1` = **Water** (bays, channels, lakes), `w1 == 0` = **Land / Islands** on top of water (Bubiyan & Failaka Islands), `w1 == 2` = **Urban / City / Concrete Aprons** on top of land (Kuwait City, Bubiyan base, Las Vegas, Tel Aviv).
* **`ALT.*`:** `0x0000..0x0FFF` = `128×128` 2-bit cloud bitmap; `0x1000..0x17FF` = `32×32` tile index grid (`0` & `1` = open water, `6+` = mountain tiles); `0x1800..EOF` = `16×16` elevation tiles.
* **`ALTPAL.*` (840 B = 4 × 70 6-bit RGB entries):** Maps 1:1 onto VGA DAC **`186..255`** for Dawn (`0..69`), Midday (`70..139`), Sunset (`140..209`), and Night (`210..279`): `186..194` = 9 upper sky bands, `195..199` = 5 water shades, `200..204` = 5 lower horizon bands, `208..255` = 6 terrain shading ramps. `ModernRenderer` renders all 14 live sky/horizon bands (`204, 203..200, 194..186`) plus matching desert, water, cloud, and horizon-fog shading + night stars, runway lights, and city glow.
* **`objects` (163,147 B) & `mapobj` (52,322 B):** 71 aircraft/vehicle/missile models + 128 ground building models (2-byte length prefix, `0xFFFF 0x4000` LOD 0 + articulated indirection, 8 octants).
* **`MAPOBJ.*`:** 4-level sparse grid of rivers, highways (connected into continuous ladder-free ribbons via 8-connected cell adjacency; `lo_mask` low nibble = `0x01 S / 0x02 E / 0x04 W / 0x08 N`), and placed 3D ground structures.
* **`ILS.*` (152 B):** `2`-byte size header + `10 × 15 B` airbase runway records (`wx, wy, z, hdg_bam14`).

### Modern Native Subsystems Built & Active
* **3-Layer Telemetry-Driven & 3D Spatial CM-32L Audio Engine (`src/audio/audio_engine.h/.cpp`):**
  * Strictly requires the user's own valid CM-32L/MT-32 ROMs (`mt32_ready_ == true`). Pre-bakes all 19 core Falcon 3.0 `Y.MID` timbres into 44.1 kHz stereo PCM buffers upfront on flight entry (~150 ms once).
  * **Layer 1 (Zero-Choppiness Telemetry Loops):** Drives `Jetengine (39)`, `JetOutside (100)`, `Afterburn2 (87)`, `Wind (108)` + high-G buffet, and **3D Doppler Nearby Jet Flybys** directly from live RAM (`RPM`, `AB1..5`, `KIAS`, `G-load`, camera/entity 3D positions & closing speed), suppressing Falcon's choppy MIDI engine notes.
  * **Layer 2 (Zero-Drop Combat Events):** Direct non-blocking triggers for `Chaff (104)` & `Flare (109)` (alternating ALE-40 stereo pan), `machinegun (43)` (~30 Hz left-wing-root pan), and spatialized explosions/AAA/wingtip missile launches (`42/46/63/81/94/97/110`) on a 64-voice mixer — immune to Falcon's 3-slot DOS MIDI dropouts.
  * **Layer 3 (Per-Bus Mixing & Polish):** Independent volume buses (`Engine/AB`, `Weapons/CM`, `Avionics/RWR`, `SB16 Radio Comms`, `Live CM-32L Menu Music`), automatic SB16 radio ducking (~3.5 dB engine dip), and toggleable warm cockpit bass body.
* **360° Retro Pixel-Art 3D F-16C Virtual Cockpit & Unclamped 3D Padlock (`src/render/cockpit_3d.h/.cpp`, `modern_renderer.cpp`):**
  * On flight entry, `DosMachine::harvest_cockpit_views()` silently captures all 4 native 320×200 Spectrum HoloByte cockpit sprites (`Forward '3'`, `Left Console '4'`, `Right Console '6'`, `Rear 6-o'clock '5'`) as raw palette indices (re-paletted live with Day/Sunset/Night DAC, zero copyrighted art in git).
  * Merges all 4 native sprites into a watertight 360° 3D cockpit tub completed with matching 1991 Bayer-dithered pixel-art floor/rudder pedals, bulkhead sill bridges, radome nose + M61 gun fairing, LERX strakes, swept F-16C delta wings, wingtip AIM-9M Sidewinders, horizontal stabilators, vertical tailfin, and canopy bow arch.
  * **Host-Driven Unclamped 3D Padlock (`8`) with Live Real 2D HUD + MFD/RWR:** Intercepts `'8'` (keyboard & HOTAS) in Hybrid/Modern/VR modes while keeping DOS Falcon in Forward Cockpit (`'3'`, `view_mode == 0`). Falcon's **100% real live 2D pixel-art HUD** (pitch ladder, velocity vector, EEGS gun funnel, AIM-9 seeker, TD box) + **live MFD radar + live RWR** map directly onto the 3D Virtual Cockpit forward glass at any bank/pitch angle, paired with a matching `320×35` `GL_NEAREST` retro pixel-art top Padlock data strip (`draw_native_padlock_strip()`). Pressing `'8'` locks the highest-priority live threat (incoming SAMs/AAMs → nearest bandits → nearest ground units) and cycles threats (`Shift+8` reverses); `'3'` or any other view key exits cleanly. Tracks targets unclamped through `±86°` pitch with a post-cockpit **Red X-Ray Padlock Target Box** whenever the target drops beneath the canopy sill. Also supports smooth **Right-Mouse Drag Free-Look** inside the 3D cockpit (Middle-Click or `'3'` recenters).
* **Why485 / Falcon BMS Smart Scaling & 3D Combat FX (`src/render/modern_renderer.h/.cpp`):**
  * Live-selectable `Off (1:1)`, `Serfoss 2003`, `Why485 / Chihirobelmo (FOV + Resolution Aware)`, and `Legacy Linear` scaling across both 3D air and ground structures (true 1:1 close-range merge/landing, runways & player jet always 1:1).
  * World-space 3D fluttering metallic **Chaff clouds**, incandescent **IR Flares** with smoke streaks, and **20mm M61 Vulcan tracer rounds** synchronized with audio triggers.
* **Authoritative Dual HOTAS + Keyboard Override (`FlightInputMode::AutoSeamless`):**
  * Overrides Falcon's legacy 1991 `CONFIG` screen: moving physical HOTAS stick/throttle immediately activates HOTAS; tapping Arrows or `+`/`-`/`/`/`,`/`.` immediately hands off to Keyboard seamlessly (plus `Force HOTAS` and `Force Keyboard Only` overrides).
* **Native F-16C Block 30/40 Flight Model Mod (`src/sim/f16_flight_model.h/.cpp`):**
  * Toggleable live in `F12 → Flight Model Mod`; 240 Hz C++ 6-DOF model synced back into `0x47364`, `0x43AB4`, `0x45C10`, and throttle/AB registers `0x47392/0x47382/0x47435/0x4737C`.
* **Built-In SHA-256 Verification & GitHub Release Setup:**
  * `DosMachine::load_exe()` and `scripts/verify_game_data.py` verify user-supplied Falcon Gold CD executables (`FALCONCD.EXE`, `HORNETCD.EXE`, `MIGCD.EXE`) against official SHA-256 hashes; `.gitignore` keeps all copyrighted game data & ROMs out of git.

---

## 5. Prioritized Next Milestones (Where to Pick Up Next)

1. **Hornet (`HORNETCD.EXE`) & MiG-29 (`MIGCD.EXE`) Full Native Parity:**
   * Follow [HORNET_MIG_PORTING_GUIDE.md](HORNET_MIG_PORTING_GUIDE.md) to add a `TitleProfile` table in `DosMachine` + `ModernRenderer` (per-title RAM addresses, player model IDs, Hornet carrier decks & F/A-18C FM, MiG-29 HUD/cockpit & Klimov RD-33 FM).
2. **Real OpenXR VR Integration (Phase 5):**
   * Add `KhronosGroup/OpenXR-SDK` via CMake `FetchContent`.
   * Feed OpenXR `XrView` left/right projection & pose matrices into `ModernRenderer::render_3d_view()`, submit OpenGL swapchains, wire headset 6-DOF head tracking into cockpit look angles, and render a true 3D F-16/F-18/MiG-29 cockpit canopy + floating HUD/MFD frame.
3. **3D Combat Visual Polish (Missiles, Explosions, Contrails):**
   * Read active AIM-9/AIM-120 missiles and explosion plumes from RAM to render 3D missile exhaust trails and fireball explosions in `ModernRenderer`.
4. **Multiplayer Live Testing (UDP / IPX Bridge):**
   * End-to-end 2-instance local & LAN dogfight test through `INT 7Ah` -> `NetUdp` (`src/net/net_udp.cpp`).
