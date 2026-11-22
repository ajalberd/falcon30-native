# Porting Guide: Bringing Hornet (`HORNETCD.EXE`) & MiG-29 (`MIGCD.EXE`) to Full Native Parity

Both **`HORNETCD.EXE`** (*Hornet: Naval Strike*) and **`MIGCD.EXE`** (*MiG-29: Deadly Adversary*) already boot inside `falcon30_native.exe` (`Title → Hornet / MiG-29`, or `--exe HORNETCD.EXE` / `--exe MIGCD.EXE`) because all three Falcon Gold executables share the exact same **Pocket Soft `.RTLink/Plus` DOS engine**, **VGA Mode X `VGRAPH` driver**, **CM-32L `Y.MID` audio architecture**, and **3D theater file formats**.

This document maps out **what already works out of the box**, **what differs per executable**, and the **exact step-by-step playbook** to achieve 100% native 3D, HUD, HOTAS, audio, and flight-model parity for both expansion titles.

---

## 1. Compatibility Matrix — What Already Works vs. What Needs Per-Title Hooks

| Subsystem | Falcon 3.0 (`FALCONCD.EXE`) | Hornet (`HORNETCD.EXE`) | MiG-29 (`MIGCD.EXE`) | Why / What's Needed |
|---|---|---|---|---|
| **DOS HLE, EMS, Mode X VGA, Mouse, Auto-Turbo, Fast-Boot** | Works | Works | Works | Shared `.RTLink/Plus` + `VGRAPH` runtime |
| **64-Voice CM-32L Sample Proxy & SB16 Voice Comms** | Works | Mostly Works | Mostly Works | Shared `Y.MID` SysEx bank & PIT0 SB DAC; verify `is_cannon_selected()` address & any Hornet/MiG-only SFX programs |
| **Multi-HOTAS Mapper & Calibration Bypass (`KENR` + `0x201`)** | Works | **Works (Verified!)** | **Works (Verified!)** | Signature-scanned `KENR\xCC\x00\xCC\x00` + `{01 03 07 0B 0F}` verified in all 3 EXEs (`0x3D00` / `0x3D10` / `0x3CD4`)! |
| **Live SAM Realism Mod (`AIM-9P  AIM-9M  `)** | Works | **Works (Verified!)** | **Works (Verified!)** | Signature-scanned missile & CM table verified in all 3 EXEs (`0x5DA0A` / `0x5F9DA` / `0x61B04`)! |
| **3D Terrain, Water, Buildings, Roads, Runways, 14-Band Sunsets** | Works | Works (All 8 Theaters!) | Works (All 8 Theaters!) | All 8 theaters (`BOS/HOK/IRQ/ISR/KOR/NEL/PAK/PAN`) share identical `ALT/ISLDAT/MAPOBJ/ILS/ALTPAL` formats! |
| **Live Telemetry (`in_flight`, 6-DOF, Camera, Entities)** | Works (`0x47364` / `0x45C10` / `0x43AB4`) | **Needs TitleProfile** | **Needs TitleProfile** | RAM addresses shift slightly per EXE due to different resident/data sizes! |
| **Player 3D External Model (`'2'`/`'9'` Chase View)** | F-16 (`0x03`) | **Needs F/A-18 ID** | **Needs MiG-29 (`0x11`)** | Currently hardcoded `mid = ent.is_player ? 3 : ...` in `modern_renderer.cpp`! |
| **HUD / Cockpit Optical Horizon (`cy_vga`) & Palette Mask** | `cy = 53`, `idx 0..127` | **Needs Verification** | **Needs Verification** | Hornet & MiG-29 cockpits may have a slightly different vertical HUD center (`cy_vga`) or canopy frame height |
| **Cannon Selected Check (M61 / GSh-30-1 Anti-Drop)** | `0x68D96 / 0x68D9A` | **Needs TitleProfile** | **Needs TitleProfile** | Needed so Spacebar/Trigger only triggers gun anti-drop when cannon (not AIM-9/R-73/AMRAAM) is selected |
| **Optional Native 240 Hz Flight Model Mod** | F-16C Block 30/40 | **Needs F/A-18C Profile** | **Needs MiG-29A Profile** | Twin-engine thrust/aero/FLCS variants (`F404-GE-400` & `RD-33`) + Hornet carrier arrestor/catapult handoff |

---

## 2. Step-by-Step Porting Playbook

### Step 1: Introduce `TitleProfile` in `DosMachine` & `ModernRenderer`
Replace the hardcoded `FALCONCD.EXE` RAM constants in `recomp/src/dos/dos_machine.h/.cpp` with a clean `TitleProfile` struct selected automatically in `DosMachine::load_exe()` based on `exe_name_`:

```cpp
enum class FalconTitle { Falcon30, Hornet, MiG29 };

struct TitleProfile {
    FalconTitle title = FalconTitle::Falcon30;
    uint32_t player_6dof_addr   = 0x47364; // Base of player 6-DOF flight struct (stride 0x1BA)
    uint32_t camera_addr        = 0x45C10; // Active 3D camera struct (20 B: x,y,z,yaw,pitch,roll)
    uint32_t entity_array_addr  = 0x43AB4; // Active 3D world entities (64 slots, stride 0x2C)
    uint32_t weapon_sel_addr1   = 0x68D96; // == 0 when cannon is selected
    uint32_t weapon_sel_addr2   = 0x68D9A; // == 0 when cannon is selected
    uint16_t player_model_id    = 0x03;    // 0x03 = F-16C, F/A-18 = TBD, 0x11 = MiG-29
    float    fwd_cockpit_cy_vga = 53.0f;   // Forward cockpit optical horizon row (0..200)
    int      cockpit_world_ymax = 95;      // Default bottom row of out-the-window 3D view
};
```

> **Quick Cleanup Along the Way:** In `recomp/src/ui/overlay_ui.cpp` (`Switch Theater` combo), fix two theater file extensions that differ from their `.MAP` names:
> - `JAPAN.MAP` uses **`.HOK`** (Hokkaido: `ALT.HOK`, `ISLDAT.HOK`, `MAPOBJ.HOK`, `ILS.HOK`), not `.JAP`.
> - `INDIA.MAP` uses **`.PAK`** (`ALT.PAK`, `ISLDAT.PAK`, `MAPOBJ.PAK`, `ILS.PAK`), not `.IND`.

---

### Step 2: Discover Exact RAM Offsets for `HORNETCD.EXE` and `MIGCD.EXE` (~10 Minutes Each!)
You can locate all 5 RAM addresses for each title in minutes using our existing `--headless`, `--dump-ram`, and `--dump-vga` CLI tools — **zero manual debugging required**:

1. **Boot to War Room & Find Instant Action Click Coords:**
   ```powershell
   .\recomp\build\falcon30_native.exe --exe HORNETCD.EXE --test-fastboot --dump-vga scratch\hornet_warroom.ppm
   .\recomp\build\falcon30_native.exe --exe MIGCD.EXE --test-fastboot --dump-vga scratch\mig_warroom.ppm
   ```
   *(Verify fast-boot reaches War Room cleanly — if either title opens a different intro/menu filename before `WARROOM`, add it to `DosMachine`'s fast-boot stage detector in `dos_machine.cpp`.)*

2. **Enter Instant Action & Capture Two In-Flight RAM Snapshots (`T1` and `T2`):**
   ```powershell
   # Snapshot 1: Straight & level in Instant Action (dump VGA to read exact HUD ALT / KIAS / HDG!)
   .\recomp\build\falcon30_native.exe --exe HORNETCD.EXE --test-fastboot --headless 65000000 `
       --click 84500000 295 25 --key 125000000 0x39 0x20 `
       --dump-vga scratch\hornet_ia1.ppm --dump-ram scratch\hornet_ia1.ram

   # Snapshot 2: ~10M instructions later after cycling weapon (Enter) and banking right
   .\recomp\build\falcon30_native.exe --exe HORNETCD.EXE --test-fastboot --headless 75000000 `
       --click 84500000 295 25 --key 125000000 0x39 0x20 --key 135000000 0x1C 0x0D `
       --dump-vga scratch\hornet_ia2.ppm --dump-ram scratch\hornet_ia2.ram
   ```

3. **Auto-Locate Every Struct in Python (Exact Same Signatures as Falcon 3.0!):**
   * **`player_6dof_addr`:** Search `hornet_ia1.ram` for `(alt_ft * 2)` (`u16` at `+0x08`) followed within `+0x14` by `KIAS` (`u16` at `+0x14`), `Mach * 100` (`u16` at `+0x12`), `MSL / 4` (`u16` at `+0x3E`), and `AGL / 4` (`u16` at `+0x40`). Verify `world_x` (`+0x00`) and `world_y` (`+0x04`) move smoothly between `ia1.ram` and `ia2.ram`!
   * **`camera_addr`:** Search `hornet_ia1.ram` for the exact 10-byte prefix `(world_x, world_y, alt*2)` matching `player_6dof_addr` — you will get **exactly 3 hits** in conventional RAM:
     1. `player_6dof_addr` (stride `0x1BA`)
     2. `entity_array_addr` (slot 0, has flags `0x4154` at `+0x24` and `0x8000 | type_id` at `+0x26`!)
     3. `camera_addr` (20-byte struct with `cam_yaw/pitch/roll` at `+0x0C/+0x0E/+0x10`)!
   * **`weapon_sel_addr1/2`:** Diff `hornet_ia1.ram` (cannon selected) vs `hornet_ia2.ram` (after pressing `Enter` to cycle to AIM-9 / R-73) for bytes transitioning `0 -> 1` near `player_6dof_addr + 0x21A00`.
   * **Verify Throttle Offsets inside `player_6dof_addr`:** Confirm `+0x18` (RPM), `+0x1E` (AB stage), `+0x2E` (raw throttle `0..305`), and `+0xD1` (AB flag `0xFF`) match Falcon 3.0's struct layout (or adjust offsets in `TitleProfile`).

---

### Step 3: Verify 3D Aircraft Model IDs, Cockpits & HUD Alignment
1. **Player & Carrier 3D Model IDs (`game_data/objects`):**
   * Inspect slot 0's `type_id` (`entity_array_addr + 0x26 & 0x7FFF`) in `hornet_ia1.ram` and `mig_ia1.ram` — that immediately tells you the exact `objects` model index for **F/A-18 Hornet** and **MiG-29 Fulcrum**!
   * Check `ModernRenderer::resolve_model_index()` so all Hornet/MiG-29 theater aircraft (e.g., Su-27 Flanker, MiG-31, F-14, F/A-18, E-2C, Russian helicopters, ships/carriers) map to valid `objects` meshes without UFO triangles.
2. **Cockpit / HUD Optical Center (`cy_vga`) & Palette Separation (`vga.cpp`):**
   * Compare `Classic VGA` vs `Hybrid Cockpit` side-by-side screenshots for **both** Hornet and MiG-29:
     * Verify whether Hornet's F/A-18 HUD and MiG-29's Russian HUD still use VGA palette indices `0..127` for cockpit/HUD and `128..255` for out-the-window 3D world (if MiG-29 uses a different HUD green/amber index, add a per-title HUD color rule in `VgaDevice::render_overlay_layers_200()`).
     * Measure horizon alignment with the `HUD Vertical Trim (VGA px)` slider in `F12 -> 3D / VR Renderer` and record the exact `fwd_cockpit_cy_vga` in `TitleProfile` for each jet!

---

### Step 4: Hornet-Specific Upgrades (`HORNETCD.EXE`)
1. **Aircraft Carrier Deck & Naval Operations:**
   * Check how `HORNETCD.EXE` places the carrier in `entity_array_addr` (or `ILS.*` / `ISLDAT.*` / `MAPOBJ.*`):
     * Render the 3D carrier mesh at true 1:1 scale (`vis_scale = 1.0f`, never BVR-scaled!) so carrier approaches, meatballs/FLOLS, and traps align 1:1 with the HUD.
     * Add carrier deck lighting (stern drop-lights, deck edge lights, centerline) when `lit_.night_amount > 0.25f`!
2. **Optional Native F/A-18C Flight Model Profile (`src/sim/f16_flight_model.h/.cpp` -> generalize to `NativeFlightModel`):**
   * Twin **GE F404-GE-400** engines (`2 × 11,000 lbf` MIL / `2 × 17,700 lbf` AB = `35,400 lbf` total), higher gross weight (~`36,000 lbs` combat), higher wing area (`400 ft²`), **high-AOA slow-speed stability** (up to `35°+` nose-pointing authority with gentler pitch-rate command below `250 kts`!), and `7.5G` structural FLCS limit.
   * **Important for Carrier Ops:** Automatically yield control back to Hornet's DOS physics when `agl_ft < 90 ft && airspeed_kts < 165 kts` near a carrier (or add a toggle `Auto-Yield FM on Carrier Catapult/Trap`) so catapult launches and arrestor-wire traps work 100% natively!

---

### Step 5: MiG-29-Specific Upgrades (`MIGCD.EXE`)
1. **Optional Native MiG-29A (9-12 Fulcrum) Flight Model Profile:**
   * Twin **Klimov RD-33** engines (`2 × 11,100 lbf` MIL / `2 × 18,300 lbf` AB = `36,600 lbf` total), ~`33,000 lbs` combat weight, `409 ft²` lifting-body blended wing-fuselage (`1.10` T/W ratio!), classic **hydraulic/mechanical stick feel** (slight natural speed stability instead of F-16's 1G digital auto-trim, crisp `28°` AOA limiter with paddle override, `9G` limit).
2. **MiG-29 Audio & HOTAS Verification:**
   * Run `--exe MIGCD.EXE --test-fastboot --headless 65000000 --trace-audio` in Instant Action to confirm whether MiG-29 uses the same `Y.MID` program numbers (`39` jet, `87` AB, `43` GSh-30-1 30mm cannon, `98/99` R-73/R-60 IRST growl, `47` radar lock) — if GSh-30-1 uses a different program or slower real-life 1,500 RPM cadence (~25 Hz, heavier bass thump), add a per-title cadence entry in `AudioEngine::get_bake_spec()`!
   * Add MiG-29 / Hornet-specific cockpit commands (e.g., **IRST / Helmet-Mounted Sight**, **Tailhook**, **Launch Bar**, **Wing Fold**) to `HotasMapper::init_default_commands()` (verified against Hornet/MiG-29 manuals) so they can be bound to HOTAS buttons & hats.

---

## 3. Recommended Execution Order (Fastest Win First)
1. **Phase A (~30 min):** Refactor `DosMachine` + `ModernRenderer` to use `TitleProfile`, fix `.HOK` / `.PAK` theater names, and plug in verified `HORNETCD.EXE` & `MIGCD.EXE` RAM offsets + player model IDs. *(Instantly unlocks full HD 3D, sunsets/night mode, 64-voice CM-32L audio, 4-axis HOTAS bypass, and SAM Realism Mod for BOTH Hornet and MiG-29!)*
2. **Phase B (~20 min):** Verify Hornet & MiG-29 HUD/cockpit `cy_vga` alignment, palette separation, and `--trace-audio` gun/RWR/lock program IDs.
3. **Phase C (~45 min):** Add Hornet carrier 1:1 scaling + deck lights, MiG-29 & Hornet HOTAS key commands, and F/A-18C / MiG-29A Native Flight Model profiles.
