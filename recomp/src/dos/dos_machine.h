#pragma once

#include "audio/audio_engine.h"
#include "dos/vga.h"
#include "input/hotas_mapper.h"
#include "net/net_udp.h"
#include "render/modern_renderer.h"
#include "sim/f16_flight_model.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct x86emu_s;
typedef struct x86emu_s x86emu_t;

namespace falcon {

struct CpuSnapshot {
    uint16_t cs = 0, ip = 0, ss = 0, sp = 0, ds = 0, es = 0;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, esi = 0, edi = 0, ebp = 0;
    uint32_t eflags = 0;
    uint64_t total_insns = 0;
    bool halted = false;
    bool exited = false;
    uint8_t exit_code = 0;
    int ems_pages_used = 0;
    int ems_pages_total = 0;
};

enum class SamModPreset : int {
    Vanilla = 0,         // Stock Falcon 3.0 (broken 80x SA-7 Strela & super-G radar SAMs)
    HistoricalF3Msl = 1, // Bill White F3MSL SA-7B Strela-2M + realistic Gulf War radar SAMs
    EarlyStrela = 2,     // SA-7A Strela-2A + relaxed radar SAMs
    LowThreat = 3        // Training / low SAM lethality
};

enum class FlightInputMode : int {
    AutoSeamless = 0,    // Both HOTAS & Keyboard work seamlessly on demand (overrides Falcon's 1991 CONFIG!)
    ForceHotas = 1,      // Always use pre-calibrated HOTAS axes
    ForceKeyboard = 2    // Always use Keyboard Only (+/- throttle & arrows)
};

enum class FalconTitle : int {
    Falcon30 = 0, // FALCONCD.EXE (F-16C)
    Hornet   = 1, // HORNETCD.EXE (F/A-18C)
    MiG29    = 2  // MIGCD.EXE    (MiG-29A)
};

// Verified per-executable RAM layout & cockpit geometry across all 3 Falcon 3.0 Gold titles
struct TitleProfile {
    FalconTitle title           = FalconTitle::Falcon30;
    const char* display_name    = "Falcon 3.0 (F-16C)";
    uint32_t player_6dof_addr   = 0x47364; // Base of player 6-DOF flight struct
    uint32_t camera_addr        = 0x45C10; // Active 3D camera struct (x,y,z,yaw,pitch,roll)
    uint32_t entity_array_addr  = 0x43AB4; // Active 3D aircraft array (16 slots, stride 0x2C)
    uint32_t ground_unit_addr   = 0x4403E; // Active ground units (100 slots, stride 0x36 = ent + 0x58A)
    uint32_t missile_array_addr = 0x42170; // Active missiles (104 slots, stride 0x2A = ent - 0x1944)
    uint32_t view_mode_addr     = 0x13E28; // Active cockpit/external view mode (0=Fwd, 2=L, 4=R, 6=Rear, 18=Ext, 26=Padlock)
    uint32_t stores_id_addr     = 0x46CF8; // Selected stores type ID (3 or 4 == cannon)
    uint32_t padlock_vec_addr   = 0x46190; // Unclamped padlock target vector (0 if unmapped)
    uint32_t padlock_name_addr  = 0x46228; // Padlock target ASCII name (0 if unmapped)
    // Intra-struct offsets inside player_6dof_addr (Hornet shifts +2 bytes starting at MSL/4)
    uint16_t msl4_off           = 0x3E;
    uint16_t agl4_off           = 0x40;
    uint16_t weight_off         = 0x42;
    uint16_t tas_off            = 0x4C;
    uint16_t vel_off            = 0x50;    // vn/ve/vu (3 x int16) + z_copy (+6)
    uint16_t g_load_off         = 0xCE;
    uint16_t ab_flag_off        = 0xD1;
    // 3D rendering defaults
    uint16_t player_model_id    = 3;       // game_data/objects index: 3=F16, 4=FA18, 17=MIG29
    float    fwd_cockpit_cy_vga = 53.0f;   // Forward cockpit optical horizon row (0..200)
};

struct SamModStatus {
    bool table_found = false;
    uint32_t sig_addr = 0;
    SamModPreset preset = SamModPreset::Vanilla;
    uint16_t sam_r_burn = 250;
    uint16_t sam_r_turn = 4096;
    uint16_t sam_r_blast = 256;
    uint16_t sam_r_cm = 750;
    uint16_t sam_ir_burn = 202;
    uint16_t sam_ir_turn = 6144;
    uint16_t sam_ir_agi = 25600;
    uint16_t sam_ir_cm = 1;
};

class DosMachine {
public:
    static constexpr uint32_t MEM_SIZE = 0x110000; // 1MB + 64KB HMA
    static constexpr uint16_t EMS_FRAME_SEG = 0xD000;
    static constexpr uint32_t EMS_FRAME_ADDR = 0xD0000;
    static constexpr int EMS_PAGE_SIZE = 16384;
    static constexpr int EMS_TOTAL_PAGES = 512; // 8 MB EMS

    DosMachine(AudioEngine& audio, HotasMapper& hotas, NetUdp& net);
    ~DosMachine();

    bool load_exe(const std::string& game_dir, const std::string& exe_name);
    const std::string& current_exe() const { return exe_name_; }
    const std::string& game_dir() const { return game_dir_; }
    const TitleProfile& title_profile() const { return profile_; }
    void reset();

    // Read live flight telemetry from active title RAM (Falcon 3.0 / Hornet / MiG-29 via TitleProfile)
    FlightTelemetry read_flight_telemetry() const;
    void read_player_fm_extras(float& weight_lbs, float& throttle_01) const;
    void read_flight_audio_extras(float& rpm_pct, float& throttle_01, int& ab_stage, float& g_load) const;
    // Selected stores type ID (3 or 4 == M61/ACAN cannon across Falcon / Hornet / MiG-29)
    uint16_t selected_stores_id() const {
        uint32_t a = profile_.stores_id_addr;
        if (a == 0 || a + 1 >= MEM_SIZE) return 3;
        return static_cast<uint16_t>(mem_[a] | (mem_[a + 1] << 8));
    }
    bool is_cannon_selected() const {
        uint16_t id = selected_stores_id();
        return id == 3 || id == 4;
    }
    const std::string& loaded_exe_sha256() const { return loaded_exe_sha256_; }
    bool loaded_exe_sha_verified() const { return loaded_exe_sha_verified_; }
    // Auto-detected active 3-letter theater extension ("IRQ", "BOS", "ISR", "KOR", "PAN", "NEL", "HOK", "PAK")
    const std::string& detected_theater_ext() const { return detected_theater_ext_; }
    // Write native C++ F-16C flight model state back into Falcon 3.0 RAM (0x47364 + 0x43AB4 + 0x45C10)
    void write_player_flight_state(const F16SimState& st, bool sync_forward_cam, bool write_throttle = false);

    // Silently capture all 4 native 320x200 cockpit sprites (Forward, Left, Right, Rear) directly from
    // Falcon's own VRAM on flight entry (~15 ms, zero flicker) to build the 360-deg 3D Virtual Cockpit!
    bool harvest_cockpit_views(Cockpit3D& out);

    // Live non-destructive SAM Realism Mod (patches 16-missile table in DOS RAM, never modifies EXE on disk!)
    void set_sam_mod_preset(SamModPreset preset);
    SamModPreset sam_mod_preset() const { return sam_mod_preset_; }
    SamModStatus read_sam_mod_status() const;
    bool apply_sam_mod_in_ram();

    // Authoritative host flight-input control (overrides Falcon 3.0's legacy 1991 CONFIG & calibration wizard!):
    void set_flight_input_mode(FlightInputMode mode) { flight_input_mode_ = mode; }
    FlightInputMode flight_input_mode() const { return flight_input_mode_; }
    bool auto_bypass_joy_cal() const { return flight_input_mode_ != FlightInputMode::ForceKeyboard; }
    bool apply_auto_joystick_calibration(bool write_def_file = true, int force_mode = -1);
    uint8_t active_dos_joystick_mode() const;

    // Execute up to max_instructions (headless / deterministic stepping)
    uint64_t step(uint64_t max_instructions);

    // Real-time wall-clock stepping: decouples game timing (PIT IRQ0, BDA 18.2Hz, VGA 70Hz, CPU budget)
    // completely from host monitor FPS (60/120/144/240Hz VRR)!
    uint64_t step_realtime(double dt_sec);

    // Configurable emulated CPU speeds (instructions per second)
    // Auto-turbo boosts from menu speed (~3.5 MIPS) to flight speed (~20 MIPS, ~= DOSBox cycles=20000)
    // while keeping PIT IRQ0 & VGA 70Hz strictly locked to real wall-clock time!
    void set_cpu_speed_ips(uint64_t ips) { cpu_speed_ips_ = ips; }
    uint64_t cpu_speed_ips() const { return cpu_speed_ips_; }
    void set_flight_cpu_speed_ips(uint64_t ips) { flight_cpu_speed_ips_ = ips; }
    uint64_t flight_cpu_speed_ips() const { return flight_cpu_speed_ips_; }
    void set_auto_turbo(bool enabled) { auto_turbo_in_flight_ = enabled; }
    bool auto_turbo() const { return auto_turbo_in_flight_; }
    uint64_t effective_cpu_speed_ips() const;

    // Skip intro video & credits automatically (fast-boots straight to War Room menu!)
    void set_skip_intro(bool enabled) { skip_intro_enabled_ = enabled; }
    bool skip_intro() const { return skip_intro_enabled_; }
    bool is_fast_booting() const { return fast_boot_active_; }

    // Per-host-frame tick (for headless mode)
    void on_frame_tick();

    // Input injection from SDL3 / HOTAS mapper
    void push_key(uint8_t scancode, uint8_t ascii, bool pressed);
    void set_mouse_state(int x_320, int y_200, uint8_t buttons_mask);

    // Render current VGA framebuffer to RGBA32 (320x400, handling both 320x200 & 320x400 Mode X!)
    void render_vga_rgba_400(uint32_t* out_rgba) const;

    CpuSnapshot snapshot() const;
    const VgaDevice& vga() const { return vga_; }
    const std::deque<std::string>& log_lines() const { return log_lines_; }
    const uint8_t* raw_memory() const { return mem_.data(); }

    void set_paused(bool p) { paused_ = p; }
    bool paused() const { return paused_; }

    // Internal callbacks invoked by libx86emu
    uint32_t handle_memio(x86emu_t* emu, uint32_t addr, uint32_t* val, unsigned type);
    int handle_intr(x86emu_t* emu, uint8_t num, unsigned type);

private:
    struct McbBlock {
        uint16_t seg = 0;   // data segment (header is seg - 1)
        uint16_t paras = 0; // size in paragraphs
        bool free = true;
    };

    struct EmsHandle {
        bool in_use = false;
        std::string name;
        std::vector<uint8_t> data; // pages * 16KB
    };

    struct OpenFile {
        std::FILE* fp = nullptr;
        std::string host_path;
        bool is_console = false;
        ~OpenFile() {
            if (fp) {
                std::fclose(fp);
                fp = nullptr;
            }
        }
    };

    struct KeyEvent {
        uint8_t scancode;
        uint8_t ascii;
    };

    AudioEngine& audio_;
    HotasMapper& hotas_;
    NetUdp& net_;
    VgaDevice vga_;

    x86emu_t* emu_ = nullptr;
    std::vector<uint8_t> mem_;
    std::string game_dir_;
    std::string exe_name_;
    TitleProfile profile_;
    std::string loaded_exe_sha256_;
    bool loaded_exe_sha_verified_ = false;

    // Memory allocator (conventional RAM)
    std::vector<McbBlock> mcbs_;
    uint16_t psp_seg_ = 0x0100;
    uint16_t dta_seg_ = 0x0100;
    uint16_t dta_off_ = 0x0080;

    // EMS 4.0 state
    std::array<EmsHandle, 64> ems_handles_{};
    struct EmsSlot {
        int handle = -1;
        int logical_page = -1;
    };
    std::array<EmsSlot, 4> ems_slots_{};
    std::unordered_map<int, std::array<EmsSlot, 4>> ems_saved_maps_;
    int ems_pages_used_ = 0;

    // DOS file table (handles 0..4 standard, 5+ reused lowest-free just like real MS-DOS!)
    std::unordered_map<uint16_t, std::unique_ptr<OpenFile>> files_;
    uint16_t alloc_file_handle() const;

    // FindFirst/FindNext state
    std::vector<std::filesystem::directory_entry> find_results_;
    size_t find_index_ = 0;

    // Keyboard & Mouse state
    std::deque<KeyEvent> key_queue_;
    std::deque<uint8_t> pending_hw_scancodes_;
    uint8_t last_scancode_ = 0;
    uint8_t kbd_shift_flags_ = 0;
    int mouse_x_ = 160;
    int mouse_y_ = 100;
    uint8_t mouse_buttons_ = 0;
    int mouse_min_x_ = 0, mouse_max_x_ = 639;
    int mouse_min_y_ = 0, mouse_max_y_ = 199;
    uint16_t mouse_cb_mask_ = 0;
    uint16_t mouse_cb_seg_ = 0;
    uint16_t mouse_cb_off_ = 0;
    uint16_t pending_mouse_mask_ = 0;
    bool in_mouse_cb_ = false;

    void invoke_mouse_callback(uint16_t event_mask);
    void deliver_hw_irq(uint8_t vec);

    // PIT / Joystick / Real-Time Decoupled Timing
    uint32_t bda_ticks_ = 0;
    uint64_t total_insns_ = 0;
    uint64_t cpu_speed_ips_ = 3500000ULL;
    uint64_t flight_cpu_speed_ips_ = 20000000ULL;
    bool auto_turbo_in_flight_ = true;
    double pit_time_accum_ = 0.0;
    double bda_time_accum_ = 0.0;
    double vga_time_accum_ = 0.0;
    double insn_budget_accum_ = 0.0;
    uint64_t joy_trigger_insn_ = 0;
    uint16_t pit_counter0_ = 0;
    uint16_t pit_reload0_ = 0; // 0 means 65536 (18.2 Hz)
    bool pit_latch_hi_ = false;
    bool pit_write_hi_ = false;
    uint64_t pit_insn_accum_ = 0;

    // 8237 DMA Channel 1 (Sound Blaster)
    uint16_t dma1_addr_ = 0;
    uint16_t dma1_count_ = 0;
    uint8_t dma1_page_ = 0;
    bool dma_flipflop_ = false;
    uint64_t sb_irq_due_insn_ = 0;
    bool sb_irq_pending_ = false;

    bool paused_ = false;
    bool exited_ = false;
    uint8_t exit_code_ = 0;
    bool skip_intro_enabled_ = true;
    bool fast_boot_active_ = false;
    int fast_boot_stage_ = 0; // 0=start, 1=saw Intro1, 2=saw Credits, 3=done (WARROOM)
    uint64_t fast_boot_key_at_ = 0;
    SamModPreset sam_mod_preset_ = SamModPreset::HistoricalF3Msl;
    mutable uint32_t sam_sig_addr_ = 0;
    std::array<uint8_t, 36> sam_vanilla_structs_{};
    std::array<uint8_t, 4> sam_vanilla_cm_{};
    bool sam_vanilla_saved_ = false;
    FlightInputMode flight_input_mode_ = FlightInputMode::AutoSeamless;
    mutable uint32_t joy_struct_base_ = 0;
    mutable int padlock_sticky_kind_ = 0; // 0=none, 1=aircraft, 2=missile, 3=ground
    mutable int padlock_sticky_slot_ = -1;
    mutable char padlock_sticky_name_[16]{};
    std::string detected_theater_ext_;
    bool in_3d_flight_session_ = false;
    double disk_turbo_sec_ = 0.0; // Temporary 25-MIPS burst during mission/theater/overlay disk loads
    std::deque<std::string> log_lines_;

    void log(const std::string& msg);
    uint8_t* ems_translate(uint32_t addr);
    uint8_t mem_read8(uint32_t addr);
    void mem_write8(uint32_t addr, uint8_t val);
    uint16_t mem_read16(uint32_t addr);
    void mem_write16(uint32_t addr, uint16_t val);

    uint8_t port_in8(uint16_t port);
    void port_out8(uint16_t port, uint8_t val);

    // HLE Interrupt handlers
    void int_10h();
    void int_11h();
    void int_12h();
    void int_15h();
    void int_16h();
    void int_1ah();
    void int_21h();
    void int_33h();
    void int_67h();
    void int_7ah();

    // Pending IPX Listen ECBs (for UDP -> IPX bridge)
    struct IpxListenEcb {
        uint16_t es = 0;
        uint16_t si = 0;
        uint16_t socket = 0;
    };
    std::vector<IpxListenEcb> ipx_listen_ecbs_;
    void poll_ipx_udp();

    // Helpers
    std::string read_dos_string(uint16_t seg, uint16_t off, char term = '\0', size_t max_len = 256);
    std::string resolve_dos_path(const std::string& dos_path);
    uint16_t dos_alloc(uint16_t paras, uint16_t& max_avail);
    bool dos_free(uint16_t seg);
    bool dos_resize(uint16_t seg, uint16_t new_paras, uint16_t& max_avail);
    void populate_find_dta(const std::filesystem::directory_entry& entry);
    void set_cf(bool carry);
};

} // namespace falcon
