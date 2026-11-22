# Falcon 3.0 Native Host (`recomp`)

Standalone Windows x64 C++20 native host & progressive modern 3D/VR reimplementation for **Falcon Gold** (`FALCONCD.EXE`, `HORNETCD.EXE`, `MIGCD.EXE`) — completely decoupled from DOSBox.

## Directory Layout
```text
falcon30_recomp/
├── game_data/                    # Optional local copy of Falcon Gold files (gitignored; or point --game-dir at your GOG install)
└── recomp/                       # Native C++20 project
    ├── CMakeLists.txt            # Fetches SDL3, Dear ImGui, libx86emu, Munt libmt32emu, nlohmann/json
    ├── README.md                 # This file
    ├── HANDOFF.md                # Full agent handoff, RE memory map, solved pitfalls, and roadmap
    ├── compat/
    │   ├── sys/io.h              # Windows shim for libx86emu mem.c
    │   └── x86emu_prim_ops.c     # Patched libx86emu prim_ops.c (fixes OUTSB DS:SI bug)
    ├── tools/
    │   └── analyze_exe.py        # MZ / .RTLink/Plus header & interrupt analyzer
    └── src/
        ├── main.cpp              # SDL3 + OpenGL + ImGui host loop & --headless test harness
        ├── dos/
        │   ├── dos_machine.h/.cpp # Embedded libx86emu + DOS/EMS 4.0/IPX HLE + real-time PIT + telemetry hook
        │   └── vga.h/.cpp        # 256KB 4-plane VGA Mode X (320x200 & 320x400) + DAC + 70.08Hz vblank
        ├── render/
        │   └── modern_renderer.h/.cpp # OpenGL 3D theater terrain, vector HUD, Hybrid & VR Stereo Preview
        ├── audio/
        │   └── audio_engine.h/.cpp # Munt libmt32emu (CM-32L) + SB16 8-bit DMA SFX + OPL3 hook
        ├── input/
        │   └── hotas_mapper.h/.cpp # SDL3 multi-HOTAS discovery, curves, pinky shift layer, JSON profiles
        ├── net/
        │   └── net_udp.h/.cpp    # Modern WinSock2 UDP Host/Join transport bridging Novell IPX (INT 7Ah)
        └── ui/
            └── overlay_ui.h/.cpp # Dear ImGui overlay (Title switcher, 3D/VR, HOTAS, Audio, Multiplayer, Debugger)
```

## Build & Run

Requires CMake 3.20+, Ninja, and MinGW-W64 GCC (`g++` 13+ in PATH).

```powershell
# Configure (fetches SDL3, Dear ImGui, libx86emu, Munt libmt32emu, nlohmann/json automatically)
cmake -S recomp -B recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build (produces standalone static-runtime recomp/build/falcon30_native.exe)
cmake --build recomp/build --parallel

# Run interactively (auto-detects ../game_data)
.\recomp\build\falcon30_native.exe
```

### Runtime Controls
| Key | Action |
|---|---|
| `F12` | Toggle Dear ImGui overlay (Title Switcher, 3D/VR Renderer, HOTAS Mapper, Audio, Multiplayer, Debugger) |
| `F11` | Cycle Render Mode: **Classic VGA** → **Modern 3D OpenGL + Vector HUD** → **Hybrid (3D + Classic Cockpit/MFDs)** → **VR Stereo Preview** |

### Headless Automated Testing & RE Harness
`falcon30_native.exe` includes a deterministic headless CLI for automated regression tests, screenshot capture (`headless_frame.ppm`), and 640 KB RAM dumps:

```powershell
# Boot FALCONCD.EXE -> Skip intro & credits -> Click Instant Action -> 3D F-16 Cockpit
.\recomp\build\falcon30_native.exe .\game_data --headless 420000000 `
  --key 20000000 0x39 32 `
  --key 50000000 0x01 27 `
  --click 270000000 295 25 `
  --key 330000000 0x39 32 `
  --key 360000000 0x39 32

# Boot Hornet or MiG-29
.\recomp\build\falcon30_native.exe .\game_data --exe HORNETCD.EXE --headless 30000000 --key 15000000 0x39 32
.\recomp\build\falcon30_native.exe .\game_data --exe MIGCD.EXE    --headless 30000000 --key 15000000 0x39 32

# Dump 640KB RAM at any checkpoint for reverse engineering
.\recomp\build\falcon30_native.exe .\game_data --headless 420000000 --dump-ram ram_t1.bin ...
```
