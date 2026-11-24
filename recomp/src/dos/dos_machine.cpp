#include "dos/dos_machine.h"

#include <x86emu.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace falcon {

namespace {
constexpr uint16_t kHleStubSeg = 0xF000;
constexpr uint16_t kEmmStubSeg = 0xE000;
constexpr uint16_t kRamTopSeg = 0x9FFF;

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool wildcard_match(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, match = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p; ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

unsigned s_memio_cb(x86emu_t* emu, u32 addr, u32* val, unsigned type) {
    auto* self = static_cast<DosMachine*>(emu->_private);
    return self->handle_memio(emu, addr, val, type);
}

int s_intr_cb(x86emu_t* emu, u8 num, unsigned type) {
    auto* self = static_cast<DosMachine*>(emu->_private);
    return self->handle_intr(emu, num, type);
}
} // namespace

DosMachine::DosMachine(AudioEngine& audio, HotasMapper& hotas, NetUdp& net)
    : audio_(audio), hotas_(hotas), net_(net), mem_(MEM_SIZE, 0) {
    reset();
}

DosMachine::~DosMachine() {
    if (emu_) {
        x86emu_done(emu_);
        emu_ = nullptr;
    }
}

void DosMachine::log(const std::string& msg) {
    log_lines_.push_back(msg);
    if (log_lines_.size() > 200) {
        log_lines_.pop_front();
    }
}

void DosMachine::reset() {
    if (emu_) {
        x86emu_done(emu_);
        emu_ = nullptr;
    }

    std::fill(mem_.begin(), mem_.end(), 0);
    vga_.reset();
    files_.clear();
    key_queue_.clear();
    pending_hw_scancodes_.clear();
    pending_mouse_mask_ = 0;
    in_mouse_cb_ = false;
    pit_time_accum_ = 0.0;
    bda_time_accum_ = 0.0;
    vga_time_accum_ = 0.0;
    insn_budget_accum_ = 0.0;
    pit_reload0_ = 0;
    pit_insn_accum_ = 0;
    ems_handles_.fill({});
    ems_slots_.fill({-1, -1});
    ems_saved_maps_.clear();
    ems_pages_used_ = 0;
    mcbs_.clear();
    total_insns_ = 0;
    bda_ticks_ = 0;
    exited_ = false;
    exit_code_ = 0;
    fast_boot_active_ = skip_intro_enabled_;
    fast_boot_stage_ = 0;
    fast_boot_key_at_ = 0;
    audio_.set_fast_boot_mute(fast_boot_active_);

    // Reserve EMS handle 0 (OS handle)
    ems_handles_[0].in_use = true;
    ems_handles_[0].name = "SYSTEM";

    // Create libx86emu instance
    emu_ = x86emu_new(0, 0);
    emu_->_private = this;
    x86emu_set_memio_handler(emu_, s_memio_cb);
    x86emu_set_intr_handler(emu_, s_intr_cb);

    // Populate IVT (0000:0000..03FF): point all 256 vectors to HLE stub at F000:(n*4)
    for (int i = 0; i < 256; ++i) {
        uint16_t seg = (i == 0x67) ? kEmmStubSeg : kHleStubSeg;
        uint16_t off = (i == 0x67) ? 0x0000 : static_cast<uint16_t>(i * 4);
        mem_write16(i * 4 + 0, off);
        mem_write16(i * 4 + 2, seg);
        // Put IRET (0xCF) at stub
        mem_write8((static_cast<uint32_t>(seg) << 4) + off, 0xCF);
    }

    // Place EMMXXXX0 device driver header at E000:000A (standard EMS detection!)
    const char* emm_sig = "EMMXXXX0";
    std::memcpy(&mem_[(static_cast<uint32_t>(kEmmStubSeg) << 4) + 0x000A], emm_sig, 8);

    // Populate BIOS Data Area (BDA at 0040:0000)
    fpu_.reset();
    mem_write16(0x0410, 0x0023); // Equipment word: VGA + 80387 Math Coprocessor present!
    mem_write16(0x0413, 640);    // 640 KB conventional memory
    mem_write8(0x0449, 0x03);    // Video mode 03h
    mem_write16(0x044A, 80);     // Columns
    mem_write16(0x0463, 0x03D4); // CRTC base port
    mem_write8(0x0484, 24);      // Rows - 1
    mem_write16(0x0485, 16);     // Char height
}

bool DosMachine::load_exe(const std::string& game_dir, const std::string& exe_name) {
    reset();
    game_dir_ = game_dir;
    exe_name_ = exe_name;

    fs::path full_path = fs::path(game_dir) / exe_name;
    std::ifstream f(full_path, std::ios::binary);
    if (!f) {
        log("ERROR: Cannot open " + full_path.string());
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 28 || data[0] != 'M' || data[1] != 'Z') {
        log("ERROR: Invalid MZ header in " + exe_name);
        return false;
    }

    // Verify user-supplied Falcon Gold executable SHA-256 (self-contained FIPS 180-4 SHA-256)
    {
        auto compute_sha256 = [](const uint8_t* msg, size_t len) -> std::string {
            static constexpr uint32_t k[64] = {
                0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
                0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
                0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
                0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
                0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
                0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
                0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
                0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
            };
            auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
            uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
            uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
            size_t padded_len = ((len + 9 + 63) / 64) * 64;
            std::vector<uint8_t> buf(padded_len, 0);
            std::memcpy(buf.data(), msg, len);
            buf[len] = 0x80;
            for (int i = 0; i < 8; ++i) buf[padded_len - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu);
            uint32_t w[64];
            for (size_t off = 0; off < padded_len; off += 64) {
                for (int i = 0; i < 16; ++i) {
                    w[i] = (static_cast<uint32_t>(buf[off + i*4 + 0]) << 24) |
                           (static_cast<uint32_t>(buf[off + i*4 + 1]) << 16) |
                           (static_cast<uint32_t>(buf[off + i*4 + 2]) << 8)  |
                           static_cast<uint32_t>(buf[off + i*4 + 3]);
                }
                for (int i = 16; i < 64; ++i) {
                    uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
                    uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
                    w[i] = w[i-16] + s0 + w[i-7] + s1;
                }
                uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
                for (int i = 0; i < 64; ++i) {
                    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                    uint32_t ch = (e & f) ^ ((~e) & g);
                    uint32_t t1 = hh + S1 + ch + k[i] + w[i];
                    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    uint32_t t2 = S0 + maj;
                    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
                }
                h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
            }
            char out[65];
            for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
            return std::string(out, 64);
        };
        loaded_exe_sha256_ = compute_sha256(data.data(), data.size());
        loaded_exe_sha_verified_ =
            (loaded_exe_sha256_ == "0c51f01bb167fe6bf48d8d386482258fba7dbab920625d589352321ce44298f3") || // FALCONCD.EXE
            (loaded_exe_sha256_ == "4322ca4cdd838b15fb5e4d5d8f4cee762b7eaaf75792bbd6ca6c80f5751c5d72") || // HORNETCD.EXE
            (loaded_exe_sha256_ == "ae67a76fb4b43856d701bd1018f2931f9b55b59926f320fb90b46ff47868a80a");   // MIGCD.EXE
        log(std::string("EXE SHA-256: ") + loaded_exe_sha256_.substr(0, 16) + "... [" +
            (loaded_exe_sha_verified_ ? "VERIFIED FALCON GOLD CD" : "UNVERIFIED VARIANT — offsets may differ") + "]");
    }

    // Select verified per-title RAM profile (Falcon 3.0 / Hornet / MiG-29)
    {
        std::string uexe = to_upper(exe_name);
        profile_ = TitleProfile{}; // Default: Falcon 3.0 (FALCONCD.EXE)
        if (uexe.find("HORNET") != std::string::npos) {
            profile_.title              = FalconTitle::Hornet;
            profile_.display_name       = "Hornet: Naval Strike (F/A-18C)";
            profile_.player_6dof_addr   = 0x450A2;
            profile_.camera_addr        = 0x442B0;
            profile_.entity_array_addr  = 0x4B754;
            profile_.ground_unit_addr   = 0x4BCDE; // entity_array + 0x58A
            profile_.missile_array_addr = 0x49E10; // entity_array - 0x1944
            profile_.view_mode_addr     = 0x13F48;
            profile_.stores_id_addr     = 0x449B2;
            profile_.padlock_vec_addr   = 0;
            profile_.padlock_name_addr  = 0;
            profile_.msl4_off           = 0x40;    // Hornet shifts +2 B from MSL/4 onward
            profile_.agl4_off           = 0x42;
            profile_.weight_off         = 0x44;
            profile_.tas_off            = 0x4E;
            profile_.vel_off            = 0x52;
            profile_.g_load_off         = 0x24;    // 10 = 1.0G in Hornet 6-DOF struct
            profile_.ab_flag_off        = 0xD1;
            profile_.player_model_id    = 4;       // game_data/objects[4] = FA18
            profile_.fwd_cockpit_cy_vga = 60.0f;
        } else if (uexe.find("MIG") != std::string::npos) {
            profile_.title              = FalconTitle::MiG29;
            profile_.display_name       = "MiG-29: Deadly Adversary";
            profile_.player_6dof_addr   = 0x48CC2;
            profile_.camera_addr        = 0x47440;
            profile_.entity_array_addr  = 0x452BA;
            profile_.ground_unit_addr   = 0x45844; // entity_array + 0x58A
            profile_.missile_array_addr = 0x43976; // entity_array - 0x1944
            profile_.view_mode_addr     = 0x139D8;
            profile_.stores_id_addr     = 0x4BA46;
            profile_.padlock_vec_addr   = 0;
            profile_.padlock_name_addr  = 0;
            profile_.msl4_off           = 0x3E;
            profile_.agl4_off           = 0x40;
            profile_.weight_off         = 0x42;
            profile_.tas_off            = 0x4C;
            profile_.vel_off            = 0x50;
            profile_.g_load_off         = 0xCE;
            profile_.ab_flag_off        = 0xD1;
            profile_.player_model_id    = 17;      // game_data/objects[17] = MIG29
            profile_.fwd_cockpit_cy_vga = 50.0f;
        }
        log(std::string("TITLE PROFILE: ") + profile_.display_name +
            " (6DOF=0x" + [&]{ char b[16]; std::snprintf(b, sizeof(b), "%X", profile_.player_6dof_addr); return std::string(b); }() +
            " CAM=0x" + [&]{ char b[16]; std::snprintf(b, sizeof(b), "%X", profile_.camera_addr); return std::string(b); }() +
            " ENT=0x" + [&]{ char b[16]; std::snprintf(b, sizeof(b), "%X", profile_.entity_array_addr); return std::string(b); }() + ")");
    }

    auto rd16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
    };

    uint16_t last_page = rd16(2);
    uint16_t pages = rd16(4);
    uint16_t reloc_cnt = rd16(6);
    uint16_t hdr_paras = rd16(8);
    uint16_t min_alloc = rd16(10);
    uint16_t init_ss = rd16(14);
    uint16_t init_sp = rd16(16);
    uint16_t init_ip = rd16(20);
    uint16_t init_cs = rd16(22);
    uint16_t reloc_off = rd16(24);

    uint32_t hdr_size = static_cast<uint32_t>(hdr_paras) * 16u;
    uint32_t mz_size = static_cast<uint32_t>(pages - 1) * 512u + (last_page ? last_page : 512u);
    if (mz_size > data.size() || hdr_size >= mz_size) {
        log("ERROR: Corrupt MZ sizes in " + exe_name);
        return false;
    }

    uint32_t img_size = mz_size - hdr_size;
    uint16_t env_seg = 0x00C0;
    psp_seg_ = 0x0100;
    uint16_t load_seg = psp_seg_ + 0x10; // 0x0110

    // Populate Environment Block at env_seg (00C0:0000)
    // .RTLink/Plus reads argv[0] from after the double-null environment string!
    uint32_t env_addr = static_cast<uint32_t>(env_seg) << 4;
    auto write_env = [&](const std::string& s) {
        std::memcpy(&mem_[env_addr], s.data(), s.size());
        env_addr += static_cast<uint32_t>(s.size());
        mem_[env_addr++] = 0;
    };
    write_env("PATH=C:\\");
    write_env("COMSPEC=C:\\COMMAND.COM");
    write_env("BLASTER=A220 I7 D1 T4");
    mem_[env_addr++] = 0; // End of environment strings
    mem_[env_addr++] = 1; // Word count = 1
    mem_[env_addr++] = 0;
    std::string argv0 = "C:\\" + to_upper(exe_name);
    write_env(argv0);

    // Populate PSP at psp_seg_ (0100:0000)
    uint32_t psp_addr = static_cast<uint32_t>(psp_seg_) << 4;
    mem_write8(psp_addr + 0x00, 0xCD); // INT 20h
    mem_write8(psp_addr + 0x01, 0x20);
    mem_write16(psp_addr + 0x02, kRamTopSeg); // Top of memory segment
    mem_write16(psp_addr + 0x2C, env_seg);    // Environment segment
    mem_write8(psp_addr + 0x80, 0);           // Command line length
    mem_write8(psp_addr + 0x81, '\r');

    // Copy MZ image to load_seg:0000
    uint32_t load_addr = static_cast<uint32_t>(load_seg) << 4;
    if (load_addr + img_size >= 0xA0000) {
        log("ERROR: MZ image exceeds conventional memory");
        return false;
    }
    std::memcpy(&mem_[load_addr], data.data() + hdr_size, img_size);

    // Apply MZ relocations
    for (uint16_t i = 0; i < reloc_cnt; ++i) {
        uint16_t r_off = rd16(reloc_off + i * 4);
        uint16_t r_seg = rd16(reloc_off + i * 4 + 2);
        uint32_t target = (static_cast<uint32_t>(load_seg + r_seg) << 4) + r_off;
        uint16_t cur = mem_read16(target);
        mem_write16(target, static_cast<uint16_t>(cur + load_seg));
    }

    // Initialize MCB allocator: program owns from psp_seg_ to kRamTopSeg initially
    uint16_t total_paras = kRamTopSeg - psp_seg_;
    mcbs_.push_back({psp_seg_, total_paras, false});

    // Set initial CPU registers (must use x86emu_set_seg_register so base/limit are updated!)
    x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, static_cast<uint16_t>(load_seg + init_cs));
    emu_->x86.R_IP = init_ip;
    x86emu_set_seg_register(emu_, emu_->x86.R_SS_SEL, static_cast<uint16_t>(load_seg + init_ss));
    emu_->x86.R_SP = init_sp;
    x86emu_set_seg_register(emu_, emu_->x86.R_DS_SEL, psp_seg_);
    x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, psp_seg_);
    emu_->x86.R_AX = 0;
    emu_->x86.R_BX = 0;
    emu_->x86.R_CX = 0x00FF;
    emu_->x86.R_DX = psp_seg_;
    emu_->x86.R_SI = init_ip;
    emu_->x86.R_DI = init_sp;
    emu_->x86.R_BP = 0x091C;
    emu_->x86.R_EFLG = 0x0202; // IF=1

    dta_seg_ = psp_seg_;
    dta_off_ = 0x0080;
    sam_sig_addr_ = 0;
    sam_vanilla_saved_ = false;
    joy_struct_base_ = 0;

    std::ostringstream oss;
    oss << "Loaded " << exe_name << " (" << img_size << " B resident, " << reloc_cnt
        << " relocs, min_alloc=" << min_alloc << "p) @ "
        << std::hex << std::uppercase << emu_->x86.R_CS << ":" << emu_->x86.R_IP;
    log(oss.str());
    if (flight_input_mode_ != FlightInputMode::ForceKeyboard && hotas_.has_any_axis_bound()) {
        apply_auto_joystick_calibration(true);
    }
    return true;
}

uint8_t* DosMachine::ems_translate(uint32_t addr) {
    if (addr >= EMS_FRAME_ADDR && addr < EMS_FRAME_ADDR + 4 * EMS_PAGE_SIZE) {
        int slot_idx = static_cast<int>((addr - EMS_FRAME_ADDR) / EMS_PAGE_SIZE);
        uint32_t page_off = (addr - EMS_FRAME_ADDR) % EMS_PAGE_SIZE;
        const auto& slot = ems_slots_[slot_idx];
        if (slot.handle >= 0 && slot.handle < static_cast<int>(ems_handles_.size()) &&
            ems_handles_[slot.handle].in_use) {
            auto& h = ems_handles_[slot.handle];
            size_t byte_off = static_cast<size_t>(slot.logical_page) * EMS_PAGE_SIZE + page_off;
            if (byte_off < h.data.size()) {
                return &h.data[byte_off];
            }
        }
    }
    return &mem_[addr % MEM_SIZE];
}

uint8_t DosMachine::mem_read8(uint32_t addr) {
    if (addr >= 0xA0000 && addr < 0xB0000) {
        return vga_.vram_read(static_cast<uint16_t>(addr - 0xA0000));
    }
    return *ems_translate(addr);
}

void DosMachine::mem_write8(uint32_t addr, uint8_t val) {
    if (addr >= 0xA0000 && addr < 0xB0000) {
        vga_.vram_write(static_cast<uint16_t>(addr - 0xA0000), val);
        return;
    }
    *ems_translate(addr) = val;
}

uint16_t DosMachine::mem_read16(uint32_t addr) {
    return static_cast<uint16_t>(mem_read8(addr) | (static_cast<uint16_t>(mem_read8(addr + 1)) << 8));
}

void DosMachine::mem_write16(uint32_t addr, uint16_t val) {
    mem_write8(addr, static_cast<uint8_t>(val & 0xFF));
    mem_write8(addr + 1, static_cast<uint8_t>(val >> 8));
}

uint8_t DosMachine::port_in8(uint16_t port) {
    if (port >= 0x3B0 && port <= 0x3DF) {
        return vga_.port_read(port);
    }
    if (port == 0x330 || port == 0x331) {
        return audio_.mpu401_read(port);
    }
    if (port >= 0x220 && port <= 0x22F) {
        return audio_.sb16_read(port);
    }
    if (port == 0x388 || port == 0x389) {
        return audio_.adlib_read(port);
    }
    switch (port) {
        case 0x40: {
            pit_counter0_ -= 17;
            pit_latch_hi_ = !pit_latch_hi_;
            return pit_latch_hi_ ? static_cast<uint8_t>(pit_counter0_ & 0xFF)
                                 : static_cast<uint8_t>(pit_counter0_ >> 8);
        }
        case 0x60:
            return last_scancode_;
        case 0x61:
            return 0x20;
        case 0x64:
            return key_queue_.empty() ? 0x00 : 0x01;
        case 0x201: {
            // Gameport joystick: bits 4..7 = buttons (active low), bits 0..3 = axis discharge timers
            // MUST use live emu_->x86.R_TSC (increments per x86 instruction inside x86emu_run), NOT total_insns_!
            uint8_t btns = static_cast<uint8_t>((~hotas_.gameport_buttons() & 0x0F) << 4);
            uint64_t cur_tsc = emu_ ? emu_->x86.R_TSC : total_insns_;
            uint64_t elapsed = (cur_tsc >= joy_trigger_insn_) ? (cur_tsc - joy_trigger_insn_) : 0;
            uint8_t axes = 0;
            for (int i = 0; i < 4; ++i) {
                // Falcon 3.0's polling loop (IN / STOSB / AND / LOOPNZ) is 4 instructions/iteration -> count == axis!
                uint64_t threshold = static_cast<uint64_t>(hotas_.gameport_axis(i)) * 4;
                if (elapsed < threshold) axes |= (1u << i);
            }
            return btns | axes;
        }
        case 0x20:
        case 0x21:
        case 0xA0:
        case 0xA1:
            return 0x00;
        default:
            return 0xFF;
    }
}

void DosMachine::port_out8(uint16_t port, uint8_t val) {
    if (port >= 0x3B0 && port <= 0x3DF) {
        vga_.port_write(port, val);
        return;
    }
    if (port == 0x330 || port == 0x331) {
        audio_.mpu401_write(port, val);
        return;
    }
    if (port >= 0x220 && port <= 0x22F) {
        uint32_t reload = (pit_reload0_ == 0) ? 65536u : static_cast<uint32_t>(pit_reload0_);
        double pit_hz = 1193182.0 / static_cast<double>(reload);
        audio_.sb16_write(port, val, pit_hz);
        int sr = 11025;
        uint32_t dma_len = audio_.consume_sb_dma_request(sr);
        if (dma_len > 0) {
            uint32_t phys = (static_cast<uint32_t>(dma1_page_) << 16) | dma1_addr_;
            std::vector<uint8_t> pcm(dma_len);
            for (uint32_t i = 0; i < dma_len; ++i) {
                pcm[i] = mem_read8((phys + i) & 0xFFFFF);
            }
            audio_.push_sb_pcm8(pcm.data(), pcm.size(), sr);
            // Schedule SB DMA completion IRQ after a realistic delay
            uint64_t delay_insns = std::max<uint64_t>(5000, (static_cast<uint64_t>(dma_len) * 3000000ULL) / std::max(1, sr));
            sb_irq_due_insn_ = total_insns_ + std::min<uint64_t>(delay_insns, 600000ULL);
            sb_irq_pending_ = true;
        }
        return;
    }
    if (port == 0x0C) {
        dma_flipflop_ = false;
        return;
    }
    if (port == 0x02) {
        if (!dma_flipflop_) dma1_addr_ = (dma1_addr_ & 0xFF00) | val;
        else dma1_addr_ = (dma1_addr_ & 0x00FF) | (static_cast<uint16_t>(val) << 8);
        dma_flipflop_ = !dma_flipflop_;
        return;
    }
    if (port == 0x03) {
        if (!dma_flipflop_) dma1_count_ = (dma1_count_ & 0xFF00) | val;
        else dma1_count_ = (dma1_count_ & 0x00FF) | (static_cast<uint16_t>(val) << 8);
        dma_flipflop_ = !dma_flipflop_;
        return;
    }
    if (port == 0x83) {
        dma1_page_ = val;
        return;
    }
    if (port == 0x388 || port == 0x389) {
        audio_.adlib_write(port, val);
        return;
    }
    if (port == 0x43) {
        if ((val & 0xC0) == 0) {
            uint8_t access = (val >> 4) & 0x03;
            if (access != 0) {
                pit_write_hi_ = false;
            }
            pit_latch_hi_ = false;
        }
        return;
    }
    if (port == 0x40) {
        if (!pit_write_hi_) {
            pit_reload0_ = (pit_reload0_ & 0xFF00) | val;
            pit_write_hi_ = true;
        } else {
            pit_reload0_ = (pit_reload0_ & 0x00FF) | (static_cast<uint16_t>(val) << 8);
            pit_write_hi_ = false;
        }
        return;
    }
    if (port == 0x201) {
        joy_trigger_insn_ = emu_ ? emu_->x86.R_TSC : total_insns_;
    }
}

uint32_t DosMachine::handle_memio(x86emu_t* /*emu*/, uint32_t addr, uint32_t* val, unsigned type) {
    unsigned op = type & ~0xFFu;
    unsigned size = type & 0xFFu;

    if (op == X86EMU_MEMIO_R || op == X86EMU_MEMIO_X) {
        if (size == X86EMU_MEMIO_8) {
            *val = mem_read8(addr);
        } else if (size == X86EMU_MEMIO_16) {
            *val = mem_read16(addr);
        } else if (size == X86EMU_MEMIO_32) {
            *val = static_cast<uint32_t>(mem_read16(addr)) |
                   (static_cast<uint32_t>(mem_read16(addr + 2)) << 16);
        }
        return 0;
    }
    if (op == X86EMU_MEMIO_W) {
        if (size == X86EMU_MEMIO_8) {
            mem_write8(addr, static_cast<uint8_t>(*val));
        } else if (size == X86EMU_MEMIO_16) {
            mem_write16(addr, static_cast<uint16_t>(*val));
        } else if (size == X86EMU_MEMIO_32) {
            mem_write16(addr, static_cast<uint16_t>(*val & 0xFFFF));
            mem_write16(addr + 2, static_cast<uint16_t>(*val >> 16));
        }
        return 0;
    }
    if (op == X86EMU_MEMIO_I) {
        uint16_t port = static_cast<uint16_t>(addr);
        if (size == X86EMU_MEMIO_8) {
            *val = port_in8(port);
        } else if (size == X86EMU_MEMIO_16) {
            *val = static_cast<uint32_t>(port_in8(port)) |
                   (static_cast<uint32_t>(port_in8(port + 1)) << 8);
        } else {
            *val = 0xFFFFFFFFu;
        }
        return 0;
    }
    if (op == X86EMU_MEMIO_O) {
        uint16_t port = static_cast<uint16_t>(addr);
        if (port == 0x3C8 && size != X86EMU_MEMIO_8) {
            std::ostringstream oss;
            oss << "NON-8BIT OUT to 0x3C8 size=" << size << " val=0x" << std::hex << *val;
            log(oss.str());
        }
        if (size == X86EMU_MEMIO_8) {
            port_out8(port, static_cast<uint8_t>(*val));
        } else if (size == X86EMU_MEMIO_16) {
            port_out8(port, static_cast<uint8_t>(*val & 0xFF));
            port_out8(port + 1, static_cast<uint8_t>((*val >> 8) & 0xFF));
        }
        return 0;
    }
    return 0;
}

int DosMachine::handle_intr(x86emu_t* /*emu*/, uint8_t num, unsigned /*type*/) {
    // Check if the guest installed its own real-mode ISR in the IVT!
    uint16_t ivt_off = mem_read16(num * 4 + 0);
    uint16_t ivt_seg = mem_read16(num * 4 + 2);
    if (ivt_seg != kHleStubSeg && ivt_seg != kEmmStubSeg) {
        // Let libx86emu dispatch to the guest's real-mode ISR (e.g. RTLink/Plus INT ABh!)
        return 0;
    }

    switch (num) {
        case 0x06: {
            // libx86emu raises INT 6 (#UD) on x87 FPU ESC opcodes (0xD8..0xDF).
            // Dispatch straight to our IEEE-754 80387 FPU emulator (unlocks HI-FIDELITY flight model!).
            uint32_t pc = ((static_cast<uint32_t>(emu_->x86.R_CS) << 4) + emu_->x86.R_IP) & 0xFFFFFu;
            uint8_t prev = mem_read8((pc - 1) & 0xFFFFFu);
            if (prev >= 0xD8 && prev <= 0xDF) {
                fpu_.execute(emu_, mem_.data(), MEM_SIZE, prev);
                return 1;
            }
            std::ostringstream oss;
            oss << "Invalid Opcode INT 0x06 @ " << std::hex << std::uppercase
                << emu_->x86.R_CS << ":" << emu_->x86.R_IP << " prev=" << static_cast<int>(prev);
            log(oss.str());
            return 1;
        }
        case 0x10: int_10h(); return 1;
        case 0x11: int_11h(); return 1;
        case 0x12: int_12h(); return 1;
        case 0x15: int_15h(); return 1;
        case 0x16: int_16h(); return 1;
        case 0x1A: int_1ah(); return 1;
        case 0x20:
            exited_ = true;
            exit_code_ = 0;
            x86emu_stop(emu_);
            return 1;
        case 0x21: int_21h(); return 1;
        case 0x2F:
            // Multiplex interrupt: AX=7A00h = Novell IPX installed check
            if (emu_->x86.R_AX == 0x7A00) {
                emu_->x86.R_AL = 0xFF;
                // Place IPX far entry stub (INT 7Ah; RETF) at F000:03E0
                constexpr uint16_t kIpxOff = 0x03E0;
                uint32_t stub = (static_cast<uint32_t>(kHleStubSeg) << 4) + kIpxOff;
                mem_write8(stub + 0, 0xCD);
                mem_write8(stub + 1, 0x7A);
                mem_write8(stub + 2, 0xCB); // RETF
                x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, kHleStubSeg);
                emu_->x86.R_DI = kIpxOff;
            } else {
                emu_->x86.R_AL = 0;
            }
            return 1;
        case 0x33: int_33h(); return 1;
        case 0x67: int_67h(); return 1;
        case 0x7A: int_7ah(); return 1;
        case 0xFF:
            // Internal trampoline sentinel for mouse callback return
            x86emu_stop(emu_);
            return 1;
        default: {
            std::ostringstream oss;
            uint32_t pc = (static_cast<uint32_t>(emu_->x86.R_CS) << 4) + emu_->x86.R_IP;
            oss << "Unhandled HLE INT 0x" << std::hex << std::uppercase << static_cast<int>(num)
                << " AX=" << emu_->x86.R_AX << " @ " << emu_->x86.R_CS << ":" << emu_->x86.R_IP
                << " bytes=[";
            for (int i = -4; i < 8; ++i) {
                if (i == 0) oss << "*";
                oss << std::setw(2) << std::setfill('0') << static_cast<int>(mem_read8(pc + i)) << " ";
            }
            oss << "]";
            log(oss.str());
            return 1;
        }
    }
}

void DosMachine::set_cf(bool carry) {
    if (carry) emu_->x86.R_EFLG |= 1u;
    else emu_->x86.R_EFLG &= ~1u;
}

std::string DosMachine::read_dos_string(uint16_t seg, uint16_t off, char term, size_t max_len) {
    std::string out;
    uint32_t base = (static_cast<uint32_t>(seg) << 4) + off;
    for (size_t i = 0; i < max_len; ++i) {
        char c = static_cast<char>(mem_read8(base + static_cast<uint32_t>(i)));
        if (c == term) break;
        out.push_back(c);
    }
    return out;
}

uint16_t DosMachine::alloc_file_handle() const {
    for (uint16_t h = 5; h < 255; ++h) {
        if (files_.find(h) == files_.end()) return h;
    }
    return 0xFFFF;
}

std::string DosMachine::resolve_dos_path(const std::string& dos_path) {
    std::string p = dos_path;
    // Strip drive letter (e.g. C:\ or D:\)
    if (p.size() >= 2 && p[1] == ':') {
        p = p.substr(2);
    }
    while (!p.empty() && (p[0] == '\\' || p[0] == '/')) {
        p.erase(p.begin());
    }
    std::replace(p.begin(), p.end(), '\\', '/');
    fs::path candidate = fs::path(game_dir_) / p;
    return candidate.string();
}

uint16_t DosMachine::dos_alloc(uint16_t paras, uint16_t& max_avail) {
    max_avail = 0;
    for (size_t i = 0; i < mcbs_.size(); ++i) {
        if (mcbs_[i].free) {
            max_avail = std::max(max_avail, mcbs_[i].paras);
            if (mcbs_[i].paras >= paras) {
                uint16_t seg = mcbs_[i].seg;
                uint16_t rem = mcbs_[i].paras - paras;
                mcbs_[i].paras = paras;
                mcbs_[i].free = false;
                if (rem > 1) {
                    mcbs_.insert(mcbs_.begin() + static_cast<ptrdiff_t>(i + 1),
                                 {static_cast<uint16_t>(seg + paras + 1), static_cast<uint16_t>(rem - 1), true});
                }
                return seg;
            }
        }
    }
    return 0;
}

bool DosMachine::dos_free(uint16_t seg) {
    for (size_t i = 0; i < mcbs_.size(); ++i) {
        if (mcbs_[i].seg == seg) {
            mcbs_[i].free = true;
            // Coalesce adjacent free blocks
            if (i + 1 < mcbs_.size() && mcbs_[i + 1].free) {
                mcbs_[i].paras += mcbs_[i + 1].paras + 1;
                mcbs_.erase(mcbs_.begin() + static_cast<ptrdiff_t>(i + 1));
            }
            if (i > 0 && mcbs_[i - 1].free) {
                mcbs_[i - 1].paras += mcbs_[i].paras + 1;
                mcbs_.erase(mcbs_.begin() + static_cast<ptrdiff_t>(i));
            }
            return true;
        }
    }
    return false;
}

bool DosMachine::dos_resize(uint16_t seg, uint16_t new_paras, uint16_t& max_avail) {
    max_avail = 0;
    for (size_t i = 0; i < mcbs_.size(); ++i) {
        if (mcbs_[i].seg == seg) {
            // Merge with next if free
            if (i + 1 < mcbs_.size() && mcbs_[i + 1].free) {
                mcbs_[i].paras += mcbs_[i + 1].paras + 1;
                mcbs_.erase(mcbs_.begin() + static_cast<ptrdiff_t>(i + 1));
            }
            max_avail = mcbs_[i].paras;
            if (new_paras > mcbs_[i].paras) {
                return false;
            }
            uint16_t rem = mcbs_[i].paras - new_paras;
            mcbs_[i].paras = new_paras;
            if (rem > 1) {
                mcbs_.insert(mcbs_.begin() + static_cast<ptrdiff_t>(i + 1),
                             {static_cast<uint16_t>(seg + new_paras + 1), static_cast<uint16_t>(rem - 1), true});
            }
            return true;
        }
    }
    return false;
}

void DosMachine::int_10h() {
    uint8_t ah = emu_->x86.R_AH;
    switch (ah) {
        case 0x00: // Set video mode
            vga_.set_mode(emu_->x86.R_AL);
            mem_write8(0x0449, vga_.mode());
            if (vga_.mode() == 0x13) {
                std::memset(&mem_[0xA0000], 0, 64000);
            }
            log("INT 10h Set Video Mode 0x" + ([](uint8_t m){ char b[8]; std::snprintf(b, sizeof(b), "%02X", m); return std::string(b); })(vga_.mode()));
            break;
        case 0x0E: // Teletype output
            vga_.tty_putc(mem_.data(), static_cast<char>(emu_->x86.R_AL));
            break;
        case 0x0F: // Get current video mode
            emu_->x86.R_AL = vga_.mode();
            emu_->x86.R_AH = (vga_.mode() == 0x13) ? 40 : 80;
            emu_->x86.R_BH = 0;
            break;
        case 0x10: { // Palette registers
            uint8_t al = emu_->x86.R_AL;
            if (al == 0x10) {
                vga_.set_dac_color(static_cast<uint8_t>(emu_->x86.R_BX), emu_->x86.R_DH, emu_->x86.R_CH, emu_->x86.R_CL);
            } else if (al == 0x12) {
                uint16_t start = emu_->x86.R_BX;
                uint16_t count = emu_->x86.R_CX;
                uint32_t addr = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_DX;
                for (uint16_t i = 0; i < count; ++i) {
                    vga_.set_dac_color(static_cast<uint8_t>(start + i),
                                       mem_read8(addr + i * 3 + 0),
                                       mem_read8(addr + i * 3 + 1),
                                       mem_read8(addr + i * 3 + 2));
                }
                std::ostringstream oss;
                oss << "INT 10h 1012 start=" << start << " cnt=" << count << " ES:DX=" << std::hex << emu_->x86.R_ES << ":" << emu_->x86.R_DX;
                log(oss.str());
            } else if (al == 0x15) {
                uint8_t r, g, b;
                vga_.get_dac_color(static_cast<uint8_t>(emu_->x86.R_BX), r, g, b);
                emu_->x86.R_DH = r;
                emu_->x86.R_CH = g;
                emu_->x86.R_CL = b;
            } else if (al == 0x17) {
                uint16_t start = emu_->x86.R_BX;
                uint16_t count = emu_->x86.R_CX;
                uint32_t addr = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_DX;
                for (uint16_t i = 0; i < count; ++i) {
                    uint8_t r, g, b;
                    vga_.get_dac_color(static_cast<uint8_t>(start + i), r, g, b);
                    mem_write8(addr + i * 3 + 0, r);
                    mem_write8(addr + i * 3 + 1, g);
                    mem_write8(addr + i * 3 + 2, b);
                }
            } else {
                std::ostringstream oss;
                oss << "INT 10h AH=10h AL=0x" << std::hex << std::uppercase << static_cast<int>(al)
                    << " BX=" << emu_->x86.R_BX << " CX=" << emu_->x86.R_CX;
                log(oss.str());
            }
            break;
        }
        case 0x1A: // Get/Set Display Combination Code (VGA color)
            if (emu_->x86.R_AL == 0x00) {
                emu_->x86.R_AL = 0x1A;
                emu_->x86.R_BL = 0x08; // VGA with analog color display
                emu_->x86.R_BH = 0x00;
            }
            break;
        default:
            break;
    }
}

void DosMachine::int_11h() {
    emu_->x86.R_AX = mem_read16(0x0410);
}

void DosMachine::int_12h() {
    emu_->x86.R_AX = 640;
}

void DosMachine::int_15h() {
    uint8_t ah = emu_->x86.R_AH;
    if (ah == 0x88) {
        emu_->x86.R_AX = 0; // No XMS/extended mem; use EMS
        set_cf(false);
    } else if (ah == 0x84) { // BIOS Joystick
        if (emu_->x86.R_DX == 0) {
            emu_->x86.R_AL = static_cast<uint8_t>((~hotas_.gameport_buttons() & 0x0F) << 4);
            set_cf(false);
        } else if (emu_->x86.R_DX == 1) {
            emu_->x86.R_AX = hotas_.gameport_axis(0);
            emu_->x86.R_BX = hotas_.gameport_axis(1);
            emu_->x86.R_CX = hotas_.gameport_axis(2);
            emu_->x86.R_DX = hotas_.gameport_axis(3);
            set_cf(false);
        }
    } else {
        set_cf(true);
        emu_->x86.R_AH = 0x86;
    }
}

void DosMachine::int_16h() {
    uint8_t ah = emu_->x86.R_AH;
    if (ah == 0x00 || ah == 0x10) {
        if (!key_queue_.empty()) {
            auto k = key_queue_.front();
            key_queue_.pop_front();
            emu_->x86.R_AH = k.scancode;
            emu_->x86.R_AL = k.ascii;
        } else {
            emu_->x86.R_AX = 0;
        }
    } else if (ah == 0x01 || ah == 0x11) {
        if (!key_queue_.empty()) {
            auto k = key_queue_.front();
            emu_->x86.R_AH = k.scancode;
            emu_->x86.R_AL = k.ascii;
            emu_->x86.R_EFLG &= ~0x40u; // ZF = 0 (key available)
        } else {
            emu_->x86.R_EFLG |= 0x40u;  // ZF = 1 (empty)
        }
    } else if (ah == 0x02 || ah == 0x12) {
        emu_->x86.R_AL = kbd_shift_flags_;
    }
}

void DosMachine::int_1ah() {
    uint8_t ah = emu_->x86.R_AH;
    if (ah == 0x00) {
        emu_->x86.R_CX = static_cast<uint16_t>(bda_ticks_ >> 16);
        emu_->x86.R_DX = static_cast<uint16_t>(bda_ticks_ & 0xFFFF);
        emu_->x86.R_AL = 0;
    } else if (ah == 0x01) {
        bda_ticks_ = (static_cast<uint32_t>(emu_->x86.R_CX) << 16) | emu_->x86.R_DX;
    }
}

void DosMachine::int_33h() {
    uint16_t ax = emu_->x86.R_AX;
    switch (ax) {
        case 0x0000: // Reset mouse
            emu_->x86.R_AX = 0xFFFF;
            emu_->x86.R_BX = 2; // 2 buttons
            log("INT 33h Reset Mouse");
            break;
        case 0x0001:
        case 0x0002:
            break;
        case 0x0003: // Get position & buttons
            emu_->x86.R_BX = mouse_buttons_;
            emu_->x86.R_CX = static_cast<uint16_t>(std::clamp(mouse_x_ * 2, mouse_min_x_, mouse_max_x_));
            emu_->x86.R_DX = static_cast<uint16_t>(std::clamp(mouse_y_, mouse_min_y_, mouse_max_y_));
            break;
        case 0x0004: // Set position
            mouse_x_ = emu_->x86.R_CX / 2;
            mouse_y_ = emu_->x86.R_DX;
            break;
        case 0x0005: // Get button press info
            emu_->x86.R_AX = mouse_buttons_;
            emu_->x86.R_BX = (mouse_buttons_ & (1u << emu_->x86.R_BX)) ? 1 : 0;
            emu_->x86.R_CX = static_cast<uint16_t>(mouse_x_ * 2);
            emu_->x86.R_DX = static_cast<uint16_t>(mouse_y_);
            break;
        case 0x0006: // Get button release info
            emu_->x86.R_AX = mouse_buttons_;
            emu_->x86.R_BX = 0;
            emu_->x86.R_CX = static_cast<uint16_t>(mouse_x_ * 2);
            emu_->x86.R_DX = static_cast<uint16_t>(mouse_y_);
            break;
        case 0x0007: // Set horizontal range
            mouse_min_x_ = emu_->x86.R_CX;
            mouse_max_x_ = emu_->x86.R_DX;
            break;
        case 0x0008: // Set vertical range
            mouse_min_y_ = emu_->x86.R_CX;
            mouse_max_y_ = emu_->x86.R_DX;
            break;
        case 0x000B: // Read motion counters
            emu_->x86.R_CX = 0;
            emu_->x86.R_DX = 0;
            break;
        case 0x000C: // Define User Mouse Event Handler
            mouse_cb_mask_ = emu_->x86.R_CX;
            mouse_cb_seg_ = emu_->x86.R_ES;
            mouse_cb_off_ = emu_->x86.R_DX;
            break;
        case 0x000F: // Set mickey/pixel ratio
        case 0x0013: // Set double speed threshold
            break;
        case 0x0014: { // Swap Mouse Event Handler
            uint16_t old_mask = mouse_cb_mask_;
            uint16_t old_seg = mouse_cb_seg_;
            uint16_t old_off = mouse_cb_off_;
            mouse_cb_mask_ = emu_->x86.R_CX;
            mouse_cb_seg_ = emu_->x86.R_ES;
            mouse_cb_off_ = emu_->x86.R_DX;
            emu_->x86.R_CX = old_mask;
            x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, old_seg);
            emu_->x86.R_DX = old_off;
            break;
        }
        default: {
            std::ostringstream oss;
            oss << "Unhandled INT 33h AX=0x" << std::hex << std::uppercase << ax
                << " CX=" << emu_->x86.R_CX << " ES:DX=" << emu_->x86.R_ES << ":" << emu_->x86.R_DX;
            log(oss.str());
            break;
        }
    }
}

void DosMachine::populate_find_dta(const fs::directory_entry& entry) {
    uint32_t dta = (static_cast<uint32_t>(dta_seg_) << 4) + dta_off_;
    std::memset(&mem_[dta], 0, 43);
    uint8_t attr = entry.is_directory() ? 0x10 : 0x20;
    uint32_t fsize = entry.is_regular_file() ? static_cast<uint32_t>(entry.file_size()) : 0;
    mem_write8(dta + 21, attr);
    mem_write16(dta + 26, static_cast<uint16_t>(fsize & 0xFFFF));
    mem_write16(dta + 28, static_cast<uint16_t>(fsize >> 16));
    std::string fname = to_upper(entry.path().filename().string());
    if (fname.size() > 12) fname.resize(12);
    std::memcpy(&mem_[dta + 30], fname.c_str(), fname.size() + 1);
}

void DosMachine::int_21h() {
    uint8_t ah = emu_->x86.R_AH;
    set_cf(false);

    switch (ah) {
        case 0x02: // Display char
            vga_.tty_putc(mem_.data(), static_cast<char>(emu_->x86.R_DL));
            break;
        case 0x06: // Direct console I/O
            if (emu_->x86.R_DL == 0xFF) {
                if (!key_queue_.empty()) {
                    emu_->x86.R_AL = key_queue_.front().ascii;
                    key_queue_.pop_front();
                    emu_->x86.R_EFLG &= ~0x40u;
                } else {
                    emu_->x86.R_AL = 0;
                    emu_->x86.R_EFLG |= 0x40u;
                }
            } else {
                vga_.tty_putc(mem_.data(), static_cast<char>(emu_->x86.R_DL));
            }
            break;
        case 0x07:
        case 0x08:
        case 0x01: // Read char
            if (!key_queue_.empty()) {
                emu_->x86.R_AL = key_queue_.front().ascii;
                key_queue_.pop_front();
            } else {
                emu_->x86.R_AL = 0;
            }
            break;
        case 0x09: { // Print $-terminated string
            std::string s = read_dos_string(emu_->x86.R_DS, emu_->x86.R_DX, '$', 512);
            for (char c : s) vga_.tty_putc(mem_.data(), c);
            log("DOS PRINT: " + s);
            break;
        }
        case 0x0B: // Check stdin status
            emu_->x86.R_AL = key_queue_.empty() ? 0x00 : 0xFF;
            break;
        case 0x0E: // Select default drive
            emu_->x86.R_AL = 4; // A..D
            break;
        case 0x19: // Get current drive (2 = C:)
            emu_->x86.R_AL = 2;
            break;
        case 0x1A: // Set DTA
            dta_seg_ = emu_->x86.R_DS;
            dta_off_ = emu_->x86.R_DX;
            break;
        case 0x25: { // Set interrupt vector
            uint8_t vec = emu_->x86.R_AL;
            mem_write16(vec * 4 + 0, emu_->x86.R_DX);
            mem_write16(vec * 4 + 2, emu_->x86.R_DS);
            if (vec != 0xAB) {
                std::ostringstream oss;
                oss << "SET IVT 0x" << std::hex << std::uppercase << static_cast<int>(vec)
                    << " -> " << emu_->x86.R_DS << ":" << emu_->x86.R_DX;
                log(oss.str());
            }
            break;
        }
        case 0x2A: { // Get date
            std::time_t t = std::time(nullptr);
            std::tm tm_now{};
            localtime_s(&tm_now, &t);
            emu_->x86.R_CX = static_cast<uint16_t>(tm_now.tm_year + 1900);
            emu_->x86.R_DH = static_cast<uint8_t>(tm_now.tm_mon + 1);
            emu_->x86.R_DL = static_cast<uint8_t>(tm_now.tm_mday);
            emu_->x86.R_AL = static_cast<uint8_t>(tm_now.tm_wday);
            break;
        }
        case 0x2C: { // Get time
            std::time_t t = std::time(nullptr);
            std::tm tm_now{};
            localtime_s(&tm_now, &t);
            emu_->x86.R_CH = static_cast<uint8_t>(tm_now.tm_hour);
            emu_->x86.R_CL = static_cast<uint8_t>(tm_now.tm_min);
            emu_->x86.R_DH = static_cast<uint8_t>(tm_now.tm_sec);
            emu_->x86.R_DL = 0;
            break;
        }
        case 0x2F: // Get DTA
            x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, dta_seg_);
            emu_->x86.R_BX = dta_off_;
            break;
        case 0x30: // Get DOS version (5.00)
            emu_->x86.R_AL = 5;
            emu_->x86.R_AH = 0;
            emu_->x86.R_BX = 0;
            emu_->x86.R_CX = 0;
            break;
        case 0x35: { // Get interrupt vector
            uint8_t vec = emu_->x86.R_AL;
            emu_->x86.R_BX = mem_read16(vec * 4 + 0);
            x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, mem_read16(vec * 4 + 2));
            break;
        }
        case 0x3C:   // Create file
        case 0x3D: { // Open file
            std::string dos_name = read_dos_string(emu_->x86.R_DS, emu_->x86.R_DX, '\0');
            std::string upper = to_upper(dos_name);
            if (upper == "CON" || upper == "EMMXXXX0") {
                auto of = std::make_unique<OpenFile>();
                of->is_console = true;
                of->host_path = upper;
                uint16_t h = alloc_file_handle();
                files_[h] = std::move(of);
                emu_->x86.R_AX = h;
                break;
            }
            std::string host_path = resolve_dos_path(dos_name);
            auto of = std::make_unique<OpenFile>();
            of->host_path = host_path;
            const char* fmode = "rb";
            if (ah == 0x3C) {
                fmode = "w+b";
            } else {
                uint8_t acc = emu_->x86.R_AL & 0x03;
                if (acc == 0) fmode = "rb";
                else if (acc == 1) fmode = "r+b";
                else fmode = "r+b";
            }
            of->fp = std::fopen(host_path.c_str(), fmode);
            if (!of->fp && ah == 0x3D && (emu_->x86.R_AL & 0x03) == 1) {
                of->fp = std::fopen(host_path.c_str(), "wb");
            }
            if (!of->fp) {
                set_cf(true);
                emu_->x86.R_AX = 2; // File not found
                log("DOS OPEN FAIL: " + dos_name);
            } else {
                uint16_t h = alloc_file_handle();
                files_[h] = std::move(of);
                emu_->x86.R_AX = h;
                log("DOS OPEN OK: " + dos_name + " -> h=" + std::to_string(h));

                // Auto-detect active theater & 3D flight session state from DOS file opens!
                {
                    std::string uname = to_upper(dos_name);
                    // Only clear 3D session on true menu/debrief screens (NOT SQUAD*.SQU, which Campaign reads when entering flight!)
                    if (uname.find("WARROOM") != std::string::npos ||
                        uname.find("DEBRIEF") != std::string::npos ||
                        uname.find("BARROOM") != std::string::npos ||
                        uname.find("TITLE") != std::string::npos ||
                        uname.find("MONITOR.IDX") != std::string::npos) {
                        in_3d_flight_session_ = false;
                    } else if (uname.find("COCKPIT") != std::string::npos) {
                        in_3d_flight_session_ = true;
                    }
                    size_t dot = uname.rfind('.');
                    if (dot != std::string::npos && dot + 4 == uname.size()) {
                        std::string ext = uname.substr(dot + 1, 3);
                        bool is_known_ext = (ext == "IRQ" || ext == "BOS" || ext == "ISR" || ext == "KOR" ||
                                             ext == "PAN" || ext == "NEL" || ext == "HOK" || ext == "PAK");
                        if (is_known_ext &&
                            (uname.find("ALT.") != std::string::npos ||
                             uname.find("ISLDAT.") != std::string::npos ||
                             uname.find("MAPOBJ.") != std::string::npos ||
                             uname.find("ILS.") != std::string::npos ||
                             uname.find("ILM.") != std::string::npos)) {
                            in_3d_flight_session_ = true;
                            if (detected_theater_ext_ != ext) {
                                detected_theater_ext_ = ext;
                                log("AUTO-DETECTED THEATER: " + ext);
                            }
                        }
                    }
                    // Trigger a brief 25-MIPS Disk/Mission Load Turbo burst when loading missions (.RFM/.CMP),
                    // theater maps, overlays, or UI screens (skip audio/cutscene streams so music/intros stay normal speed!)
                    bool is_media_stream = (uname.find(".VOX") != std::string::npos ||
                                            uname.find(".SNG") != std::string::npos ||
                                            uname.find(".MT")  != std::string::npos ||
                                            uname.find(".AD")  != std::string::npos ||
                                            uname.find("INTRO") != std::string::npos ||
                                            uname.find("CREDITS") != std::string::npos);
                    if (!is_media_stream) {
                        disk_turbo_sec_ = std::max(disk_turbo_sec_, 0.55);
                    }
                }

                // Fast-Boot Skip Intro state machine (works across Falcon / Hornet / MiG)
                if (fast_boot_active_) {
                    std::string uname = to_upper(dos_name);
                    if (fast_boot_stage_ < 1 &&
                        (uname.find("INTRO1") != std::string::npos || uname.find("INTRO2") != std::string::npos)) {
                        fast_boot_stage_ = 1;
                        fast_boot_key_at_ = total_insns_ + 1500000ULL;
                    } else if (fast_boot_stage_ < 2 &&
                               (uname.find("CREDITS") != std::string::npos || uname.find("MCREDITS") != std::string::npos ||
                                uname.find("TITLEPIC") != std::string::npos)) {
                        fast_boot_stage_ = 2;
                        fast_boot_key_at_ = total_insns_ + 1500000ULL;
                    } else if (fast_boot_stage_ < 3 && uname.find("THEATER.BIN") != std::string::npos) {
                        // Stop injecting keys immediately so no stray Esc reaches the War Room
                        fast_boot_stage_ = 3;
                        fast_boot_key_at_ = total_insns_ + 25000000ULL; // Fallback finish if no WARROOM file
                    } else if (uname.find("WARROOM") != std::string::npos || uname.find("READYRM") != std::string::npos) {
                        fast_boot_stage_ = 4;
                        fast_boot_key_at_ = total_insns_ + 5000000ULL; // Let War Room finish rendering cleanly
                    }
                }
            }
            break;
        }
        case 0x3E: { // Close file
            uint16_t h = emu_->x86.R_BX;
            if (h >= 5) {
                auto it = files_.find(h);
                if (it != files_.end()) {
                    files_.erase(it);
                } else {
                    set_cf(true);
                    emu_->x86.R_AX = 6;
                }
            }
            break;
        }
        case 0x3F: { // Read file
            uint16_t h = emu_->x86.R_BX;
            uint16_t count = emu_->x86.R_CX;
            uint32_t dst = (static_cast<uint32_t>(emu_->x86.R_DS) << 4) + emu_->x86.R_DX;
            auto it = files_.find(h);
            if (it == files_.end()) {
                log("DOS READ FAIL bad handle h=" + std::to_string(h) + " cnt=" + std::to_string(count));
                set_cf(true);
                emu_->x86.R_AX = 6;
                break;
            }
            if (it->second->is_console || !it->second->fp) {
                emu_->x86.R_AX = 0;
                break;
            }
            std::vector<uint8_t> buf(count);
            size_t got = std::fread(buf.data(), 1, count, it->second->fp);
            for (size_t i = 0; i < got; ++i) {
                mem_write8(dst + static_cast<uint32_t>(i), buf[i]);
            }
            // If .RTLink/Plus just paged in the flight/missile overlay from FALCONCD/HORNETCD/MIGCD.EXE,
            // seamlessly apply the active SAM Realism Mod non-destructively in RAM!
            if (got >= 1024 && dst + got < 0xA0000) {
                static constexpr char kSig[] = "AIM-9P  AIM-9M  ";
                if (to_upper(it->second->host_path).find(".EXE") != std::string::npos) {
                    const uint8_t* found = std::search(buf.data(), buf.data() + got,
                                                       reinterpret_cast<const uint8_t*>(kSig),
                                                       reinterpret_cast<const uint8_t*>(kSig) + 16);
                    if (found != buf.data() + got) {
                        uint32_t sig_lin = dst + static_cast<uint32_t>(found - buf.data());
                        if (sig_lin >= 0x2A6 + 12 * 18) {
                            sam_sig_addr_ = sig_lin;
                            sam_vanilla_saved_ = false;
                            apply_sam_mod_in_ram();
                        }
                    }
                }
            }
            // Sustain 25-MIPS Disk/Mission Load Turbo during sustained non-audio file reads (mission/map/overlay loading!)
            if (got >= 512) {
                std::string up = to_upper(it->second->host_path);
                if (up.find(".VOX") == std::string::npos && up.find(".SNG") == std::string::npos &&
                    up.find(".MT")  == std::string::npos && up.find("INTRO") == std::string::npos) {
                    disk_turbo_sec_ = std::max(disk_turbo_sec_, 0.45);
                }
            }
            emu_->x86.R_AX = static_cast<uint16_t>(got);
            break;
        }
        case 0x40: { // Write file
            uint16_t h = emu_->x86.R_BX;
            uint16_t count = emu_->x86.R_CX;
            uint32_t src = (static_cast<uint32_t>(emu_->x86.R_DS) << 4) + emu_->x86.R_DX;
            if (h == 1 || h == 2) {
                std::string s;
                for (uint16_t i = 0; i < count; ++i) {
                    char c = static_cast<char>(mem_read8(src + i));
                    vga_.tty_putc(mem_.data(), c);
                    s.push_back(c);
                }
                log("STDOUT: " + s);
                emu_->x86.R_AX = count;
                break;
            }
            auto it = files_.find(h);
            if (it == files_.end() || !it->second->fp) {
                set_cf(true);
                emu_->x86.R_AX = 6;
                break;
            }
            std::vector<uint8_t> buf(count);
            for (uint16_t i = 0; i < count; ++i) {
                buf[i] = mem_read8(src + i);
            }
            size_t written = std::fwrite(buf.data(), 1, count, it->second->fp);
            std::fflush(it->second->fp);
            emu_->x86.R_AX = static_cast<uint16_t>(written);
            break;
        }
        case 0x41: { // Delete file
            std::string dos_name = read_dos_string(emu_->x86.R_DS, emu_->x86.R_DX, '\0');
            std::string host_path = resolve_dos_path(dos_name);
            std::error_code ec;
            if (fs::remove(host_path, ec)) {
                log("DOS DELETE OK: " + dos_name);
            } else {
                set_cf(true);
                emu_->x86.R_AX = 2;
            }
            break;
        }
        case 0x42: { // LSEEK
            uint16_t h = emu_->x86.R_BX;
            auto it = files_.find(h);
            if (it == files_.end() || !it->second->fp) {
                set_cf(true);
                emu_->x86.R_AX = 6;
                break;
            }
            int32_t offset = static_cast<int32_t>((static_cast<uint32_t>(emu_->x86.R_CX) << 16) | emu_->x86.R_DX);
            int whence = SEEK_SET;
            if (emu_->x86.R_AL == 1) whence = SEEK_CUR;
            else if (emu_->x86.R_AL == 2) whence = SEEK_END;
            if (std::fseek(it->second->fp, offset, whence) != 0) {
                set_cf(true);
                emu_->x86.R_AX = 1;
                break;
            }
            long pos = std::ftell(it->second->fp);
            if (pos < 0) pos = 0;
            uint32_t u = static_cast<uint32_t>(pos);
            emu_->x86.R_DX = static_cast<uint16_t>(u >> 16);
            emu_->x86.R_AX = static_cast<uint16_t>(u & 0xFFFF);
            break;
        }
        case 0x43: { // Get/Set file attributes
            std::string host = resolve_dos_path(read_dos_string(emu_->x86.R_DS, emu_->x86.R_DX));
            if (fs::exists(host)) {
                emu_->x86.R_CX = fs::is_directory(host) ? 0x10 : 0x20;
            } else {
                set_cf(true);
                emu_->x86.R_AX = 2;
            }
            break;
        }
        case 0x44: { // IOCTL
            uint8_t al = emu_->x86.R_AL;
            uint16_t h = emu_->x86.R_BX;
            if (al == 0x00) {
                if (h <= 2) emu_->x86.R_DX = 0x0083; // Char device (CON)
                else {
                    auto it = files_.find(h);
                    emu_->x86.R_DX = (it != files_.end() && it->second->is_console) ? 0x0080 : 0x0002;
                }
            } else if (al == 0x07) {
                emu_->x86.R_AL = 0xFF; // Ready
            }
            break;
        }
        case 0x45: { // Duplicate file handle
            uint16_t h = emu_->x86.R_BX;
            if (h <= 4) {
                auto dup = std::make_unique<OpenFile>();
                dup->is_console = true;
                uint16_t nh = alloc_file_handle();
                files_[nh] = std::move(dup);
                emu_->x86.R_AX = nh;
                break;
            }
            auto it = files_.find(h);
            if (it == files_.end()) {
                set_cf(true);
                emu_->x86.R_AX = 6;
            } else {
                auto dup = std::make_unique<OpenFile>();
                dup->host_path = it->second->host_path;
                dup->is_console = it->second->is_console;
                if (!dup->is_console && it->second->fp) {
                    long cur = std::ftell(it->second->fp);
                    dup->fp = std::fopen(dup->host_path.c_str(), "r+b");
                    if (!dup->fp) dup->fp = std::fopen(dup->host_path.c_str(), "rb");
                    if (dup->fp && cur >= 0) std::fseek(dup->fp, cur, SEEK_SET);
                }
                uint16_t nh = alloc_file_handle();
                files_[nh] = std::move(dup);
                emu_->x86.R_AX = nh;
            }
            break;
        }
        case 0x46: { // Force duplicate file handle
            uint16_t h1 = emu_->x86.R_BX;
            uint16_t h2 = emu_->x86.R_CX;
            if (h1 <= 4 || h2 <= 4) {
                // Stdio redirection (e.g. BX=2, CX=1): succeed cleanly
                break;
            }
            auto it = files_.find(h1);
            if (it == files_.end()) {
                set_cf(true);
                emu_->x86.R_AX = 6;
            } else {
                auto dup = std::make_unique<OpenFile>();
                dup->host_path = it->second->host_path;
                dup->is_console = it->second->is_console;
                if (!dup->is_console && it->second->fp) {
                    long cur = std::ftell(it->second->fp);
                    dup->fp = std::fopen(dup->host_path.c_str(), "r+b");
                    if (!dup->fp) dup->fp = std::fopen(dup->host_path.c_str(), "rb");
                    if (dup->fp && cur >= 0) std::fseek(dup->fp, cur, SEEK_SET);
                }
                files_[h2] = std::move(dup);
            }
            break;
        }
        case 0x47: { // Get current directory (root = empty string)
            uint32_t dst = (static_cast<uint32_t>(emu_->x86.R_DS) << 4) + emu_->x86.R_SI;
            mem_write8(dst, 0);
            break;
        }
        case 0x48: { // Allocate memory
            uint16_t max_av = 0;
            uint16_t seg = dos_alloc(emu_->x86.R_BX, max_av);
            if (seg != 0) {
                emu_->x86.R_AX = seg;
            } else {
                set_cf(true);
                emu_->x86.R_AX = 8;
                emu_->x86.R_BX = max_av;
            }
            break;
        }
        case 0x49: { // Free memory
            if (!dos_free(emu_->x86.R_ES)) {
                set_cf(true);
                emu_->x86.R_AX = 9;
            }
            break;
        }
        case 0x4A: { // Resize memory block
            uint16_t max_av = 0;
            if (!dos_resize(emu_->x86.R_ES, emu_->x86.R_BX, max_av)) {
                set_cf(true);
                emu_->x86.R_AX = 8;
                emu_->x86.R_BX = max_av;
            }
            break;
        }
        case 0x4B: { // EXEC / Load Overlay (AL=03h)
            if (emu_->x86.R_AL == 0x03) {
                std::string dos_name = read_dos_string(emu_->x86.R_DS, emu_->x86.R_DX, '\0');
                std::string host_path = resolve_dos_path(dos_name);
                uint32_t pb = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_BX;
                uint16_t ovl_load_seg = mem_read16(pb + 0);
                uint16_t ovl_reloc_fac = mem_read16(pb + 2);

                std::ifstream f(host_path, std::ios::binary);
                if (!f) {
                    set_cf(true);
                    emu_->x86.R_AX = 2;
                    log("DOS 4B03 FAIL open: " + dos_name);
                    break;
                }
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                uint32_t dst_base = static_cast<uint32_t>(ovl_load_seg) << 4;
                if (data.size() >= 28 && data[0] == 'M' && data[1] == 'Z') {
                    auto rd16 = [&](size_t off) -> uint16_t {
                        return static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
                    };
                    uint16_t last_p = rd16(2);
                    uint16_t pages = rd16(4);
                    uint16_t reloc_cnt = rd16(6);
                    uint16_t hdr_paras = rd16(8);
                    uint16_t reloc_off = rd16(24);
                    uint32_t hdr_size = static_cast<uint32_t>(hdr_paras) * 16u;
                    uint32_t mz_size = static_cast<uint32_t>(pages - 1) * 512u + (last_p ? last_p : 512u);
                    if (mz_size > data.size()) mz_size = static_cast<uint32_t>(data.size());
                    uint32_t img_size = mz_size - hdr_size;
                    for (uint32_t i = 0; i < img_size; ++i) {
                        mem_write8(dst_base + i, data[hdr_size + i]);
                    }
                    for (uint16_t i = 0; i < reloc_cnt; ++i) {
                        uint16_t r_off = rd16(reloc_off + i * 4);
                        uint16_t r_seg = rd16(reloc_off + i * 4 + 2);
                        uint32_t target = (static_cast<uint32_t>(ovl_load_seg + r_seg) << 4) + r_off;
                        mem_write16(target, static_cast<uint16_t>(mem_read16(target) + ovl_reloc_fac));
                    }
                    std::ostringstream oss;
                    oss << "DOS 4B03 LOADED MZ OVERLAY: " << dos_name << " (" << img_size
                        << " B, " << reloc_cnt << " relocs) @ seg " << std::hex << std::uppercase
                        << ovl_load_seg << " reloc=" << ovl_reloc_fac;
                    log(oss.str());
                } else {
                    for (size_t i = 0; i < data.size(); ++i) {
                        mem_write8(dst_base + static_cast<uint32_t>(i), data[i]);
                    }
                    log("DOS 4B03 LOADED RAW OVERLAY: " + dos_name);
                }
            } else {
                set_cf(true);
                emu_->x86.R_AX = 1;
                log("Unhandled INT 21h AH=4B AL=" + std::to_string(emu_->x86.R_AL));
            }
            break;
        }
        case 0x4C: // Exit
            exited_ = true;
            exit_code_ = emu_->x86.R_AL;
            log("DOS EXIT code=" + std::to_string(exit_code_));
            x86emu_stop(emu_);
            break;
        case 0x4E: { // FindFirst
            std::string pat = read_dos_string(emu_->x86.R_DS, emu_->x86.R_DX);
            std::string resolved = resolve_dos_path(pat);
            fs::path p(resolved);
            fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(game_dir_);
            std::string mask = to_upper(p.filename().string());
            find_results_.clear();
            find_index_ = 0;
            if (fs::exists(dir) && fs::is_directory(dir)) {
                for (const auto& entry : fs::directory_iterator(dir)) {
                    if (wildcard_match(mask, to_upper(entry.path().filename().string()))) {
                        find_results_.push_back(entry);
                    }
                }
            }
            if (!find_results_.empty()) {
                populate_find_dta(find_results_[find_index_++]);
            } else {
                set_cf(true);
                emu_->x86.R_AX = 18;
            }
            break;
        }
        case 0x4F: // FindNext
            if (find_index_ < find_results_.size()) {
                populate_find_dta(find_results_[find_index_++]);
            } else {
                set_cf(true);
                emu_->x86.R_AX = 18;
            }
            break;
        case 0x51:
        case 0x62: // Get PSP
            emu_->x86.R_BX = psp_seg_;
            break;
        case 0x58: // Get/Set Memory Allocation Strategy
            if (emu_->x86.R_AL == 0x00) emu_->x86.R_AX = 0;
            else if (emu_->x86.R_AL == 0x02) emu_->x86.R_AL = 0;
            break;
        default: {
            std::ostringstream oss;
            oss << "Unhandled INT 21h AH=0x" << std::hex << std::uppercase << static_cast<int>(ah)
                << " AX=" << emu_->x86.R_AX;
            log(oss.str());
            break;
        }
    }
}

void DosMachine::int_67h() {
    uint8_t ah = emu_->x86.R_AH;
    switch (ah) {
        case 0x40: // Get Status
            emu_->x86.R_AH = 0x00;
            break;
        case 0x41: // Get Page Frame Segment
            emu_->x86.R_BX = EMS_FRAME_SEG;
            emu_->x86.R_AH = 0x00;
            break;
        case 0x42: // Get Unallocated Page Count
            emu_->x86.R_BX = static_cast<uint16_t>(EMS_TOTAL_PAGES - ems_pages_used_);
            emu_->x86.R_DX = static_cast<uint16_t>(EMS_TOTAL_PAGES);
            emu_->x86.R_AH = 0x00;
            break;
        case 0x43: { // Allocate Pages
            uint16_t count = emu_->x86.R_BX;
            if (count == 0 || ems_pages_used_ + count > EMS_TOTAL_PAGES) {
                emu_->x86.R_AH = (count == 0) ? 0x89 : 0x87;
                break;
            }
            int free_h = -1;
            for (int i = 1; i < static_cast<int>(ems_handles_.size()); ++i) {
                if (!ems_handles_[i].in_use) { free_h = i; break; }
            }
            if (free_h < 0) {
                emu_->x86.R_AH = 0x85;
                break;
            }
            ems_handles_[free_h].in_use = true;
            ems_handles_[free_h].data.assign(static_cast<size_t>(count) * EMS_PAGE_SIZE, 0);
            ems_pages_used_ += count;
            emu_->x86.R_DX = static_cast<uint16_t>(free_h);
            emu_->x86.R_AH = 0x00;
            break;
        }
        case 0x44: { // Map/Unmap Handle Page
            uint8_t phys = emu_->x86.R_AL;
            uint16_t logical = emu_->x86.R_BX;
            uint16_t handle = emu_->x86.R_DX;
            if (phys >= 4) { emu_->x86.R_AH = 0x8B; break; }
            if (logical == 0xFFFF) {
                ems_slots_[phys] = {-1, -1};
                emu_->x86.R_AH = 0x00;
                break;
            }
            if (handle >= ems_handles_.size() || !ems_handles_[handle].in_use) {
                emu_->x86.R_AH = 0x83;
                break;
            }
            if (static_cast<size_t>(logical) * EMS_PAGE_SIZE >= ems_handles_[handle].data.size()) {
                emu_->x86.R_AH = 0x8A;
                break;
            }
            ems_slots_[phys] = {static_cast<int>(handle), static_cast<int>(logical)};
            emu_->x86.R_AH = 0x00;
            break;
        }
        case 0x45: { // Deallocate Pages
            uint16_t handle = emu_->x86.R_DX;
            if (handle == 0 || handle >= ems_handles_.size() || !ems_handles_[handle].in_use) {
                emu_->x86.R_AH = 0x83;
                break;
            }
            ems_pages_used_ -= static_cast<int>(ems_handles_[handle].data.size() / EMS_PAGE_SIZE);
            ems_handles_[handle] = {};
            for (auto& s : ems_slots_) {
                if (s.handle == handle) s = {-1, -1};
            }
            emu_->x86.R_AH = 0x00;
            break;
        }
        case 0x46: // Get EMM Version (4.0)
            emu_->x86.R_AL = 0x40;
            emu_->x86.R_AH = 0x00;
            break;
        case 0x47: // Save Page Map
            ems_saved_maps_[emu_->x86.R_DX] = ems_slots_;
            emu_->x86.R_AH = 0x00;
            break;
        case 0x48: { // Restore Page Map
            auto it = ems_saved_maps_.find(emu_->x86.R_DX);
            if (it != ems_saved_maps_.end()) {
                ems_slots_ = it->second;
                ems_saved_maps_.erase(it);
            }
            emu_->x86.R_AH = 0x00;
            break;
        }
        case 0x4B: { // Get Handle Count
            uint16_t cnt = 0;
            for (const auto& h : ems_handles_) if (h.in_use) cnt++;
            emu_->x86.R_BX = cnt;
            emu_->x86.R_AH = 0x00;
            break;
        }
        case 0x4C: { // Get Handle Pages
            uint16_t handle = emu_->x86.R_DX;
            if (handle < ems_handles_.size() && ems_handles_[handle].in_use) {
                emu_->x86.R_BX = static_cast<uint16_t>(ems_handles_[handle].data.size() / EMS_PAGE_SIZE);
                emu_->x86.R_AH = 0x00;
            } else {
                emu_->x86.R_AH = 0x83;
            }
            break;
        }
        case 0x50: { // Map/Unmap Multiple Handle Pages
            uint8_t sub = emu_->x86.R_AL;
            uint16_t count = emu_->x86.R_CX;
            uint16_t handle = emu_->x86.R_DX;
            uint32_t ptr = (static_cast<uint32_t>(emu_->x86.R_DS) << 4) + emu_->x86.R_SI;
            for (uint16_t i = 0; i < count; ++i) {
                uint16_t logical = mem_read16(ptr + i * 4);
                uint16_t phys_or_seg = mem_read16(ptr + i * 4 + 2);
                int phys = (sub == 0) ? phys_or_seg : ((phys_or_seg - EMS_FRAME_SEG) / 0x0400);
                if (phys >= 0 && phys < 4) {
                    if (logical == 0xFFFF) ems_slots_[phys] = {-1, -1};
                    else ems_slots_[phys] = {static_cast<int>(handle), static_cast<int>(logical)};
                }
            }
            emu_->x86.R_AH = 0x00;
            break;
        }
        case 0x58: { // Get Mappable Physical Address Array
            if (emu_->x86.R_AL == 0x00) {
                uint32_t dst = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_DI;
                for (uint16_t i = 0; i < 4; ++i) {
                    mem_write16(dst + i * 4 + 0, static_cast<uint16_t>(EMS_FRAME_SEG + i * 0x0400));
                    mem_write16(dst + i * 4 + 2, i);
                }
                emu_->x86.R_CX = 4;
            } else {
                emu_->x86.R_CX = 4;
            }
            emu_->x86.R_AH = 0x00;
            break;
        }
        default: {
            std::ostringstream oss;
            oss << "Unhandled EMS INT 67h AH=0x" << std::hex << std::uppercase << static_cast<int>(ah);
            log(oss.str());
            emu_->x86.R_AH = 0x84;
            break;
        }
    }
}

void DosMachine::int_7ah() {
    uint16_t fn = emu_->x86.R_BX;
    switch (fn) {
        case 0x0000: // IPX Open Socket
            emu_->x86.R_AL = 0x00;
            if (emu_->x86.R_DX == 0) emu_->x86.R_DX = 0x4567;
            break;
        case 0x0001: // IPX Close Socket
            emu_->x86.R_AL = 0x00;
            break;
        case 0x0002: // Get Local Target
            emu_->x86.R_AL = 0x00;
            emu_->x86.R_CX = 1; // 1 tick transport time
            break;
        case 0x0003: { // IPX Send Packet -> forward over modern UDP!
            uint32_t ecb = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_SI;
            uint16_t frag_cnt = mem_read16(ecb + 34);
            std::vector<uint8_t> payload;
            for (uint16_t i = 0; i < frag_cnt && i < 8; ++i) {
                uint16_t f_off = mem_read16(ecb + 36 + i * 6 + 0);
                uint16_t f_seg = mem_read16(ecb + 36 + i * 6 + 2);
                uint16_t f_len = mem_read16(ecb + 36 + i * 6 + 4);
                uint32_t f_addr = (static_cast<uint32_t>(f_seg) << 4) + f_off;
                for (uint16_t b = 0; b < f_len; ++b) {
                    payload.push_back(mem_read8(f_addr + b));
                }
            }
            if (!payload.empty()) {
                net_.send_payload(payload.data(), payload.size());
            }
            mem_write8(ecb + 8, 0x00); // InUse = 0 (completed)
            mem_write8(ecb + 9, 0x00); // CompletionCode = 0 (success)
            emu_->x86.R_AL = 0x00;
            break;
        }
        case 0x0004: { // IPX Listen For Packet -> register ECB
            uint32_t ecb = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_SI;
            uint16_t sock = mem_read16(ecb + 10);
            mem_write8(ecb + 8, 0xFE); // InUse = listening
            ipx_listen_ecbs_.push_back({emu_->x86.R_ES, emu_->x86.R_SI, sock});
            emu_->x86.R_AL = 0x00;
            break;
        }
        case 0x0009: { // Get Internetwork Address (4-byte net + 6-byte node)
            uint32_t dst = (static_cast<uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_SI;
            mem_write8(dst + 0, 0x00); mem_write8(dst + 1, 0x00);
            mem_write8(dst + 2, 0x00); mem_write8(dst + 3, 0x01);
            const auto& node = net_.local_node_id();
            for (int i = 0; i < 6; ++i) mem_write8(dst + 4 + i, node[i]);
            emu_->x86.R_AL = 0x00;
            break;
        }
        case 0x000A: // IPX Relinquish Control
            poll_ipx_udp();
            emu_->x86.R_AL = 0x00;
            break;
        default:
            emu_->x86.R_AL = 0x00;
            break;
    }
}

void DosMachine::poll_ipx_udp() {
    net_.poll();
    NetPacket pkt;
    while (!ipx_listen_ecbs_.empty() && net_.pop_packet(pkt)) {
        auto lecb = ipx_listen_ecbs_.front();
        ipx_listen_ecbs_.erase(ipx_listen_ecbs_.begin());
        uint32_t ecb = (static_cast<uint32_t>(lecb.es) << 4) + lecb.si;
        uint16_t frag_cnt = mem_read16(ecb + 34);
        size_t pos = 0;
        for (uint16_t i = 0; i < frag_cnt && i < 8 && pos < pkt.data.size(); ++i) {
            uint16_t f_off = mem_read16(ecb + 36 + i * 6 + 0);
            uint16_t f_seg = mem_read16(ecb + 36 + i * 6 + 2);
            uint16_t f_len = mem_read16(ecb + 36 + i * 6 + 4);
            uint32_t f_addr = (static_cast<uint32_t>(f_seg) << 4) + f_off;
            for (uint16_t b = 0; b < f_len && pos < pkt.data.size(); ++b) {
                mem_write8(f_addr + b, pkt.data[pos++]);
            }
        }
        mem_write8(ecb + 8, 0x00); // InUse = 0 (received!)
        mem_write8(ecb + 9, 0x00); // CompletionCode = 0
    }
}

FlightTelemetry DosMachine::read_flight_telemetry() const {
    FlightTelemetry t{};
    t.player_model_id = profile_.player_model_id;
    t.fwd_cockpit_cy_vga = profile_.fwd_cockpit_cy_vga;

    // Verified live 6-DOF flight state struct via TitleProfile (Falcon / Hornet / MiG-29)
    const uint32_t p6  = profile_.player_6dof_addr;
    const uint32_t cam = profile_.camera_addr;
    const uint32_t ent = profile_.entity_array_addr;
    const uint32_t gnd = profile_.ground_unit_addr;
    const uint32_t msl = profile_.missile_array_addr;
    const uint32_t vma = profile_.view_mode_addr;

    auto rd_u16 = [&](uint32_t a) -> uint16_t {
        return static_cast<uint16_t>(mem_[a] | (mem_[a + 1] << 8));
    };
    auto rd_i16 = [&](uint32_t a) -> int16_t {
        return static_cast<int16_t>(rd_u16(a));
    };
    auto rd_i32 = [&](uint32_t a) -> int32_t {
        return static_cast<int32_t>(rd_u16(a) | (static_cast<uint32_t>(rd_u16(a + 2)) << 16));
    };

    uint16_t mach100 = rd_u16(p6 + 0x12u);
    uint16_t kias = rd_u16(p6 + 0x14u);
    uint16_t rpm = rd_u16(p6 + 0x18u);
    uint16_t alt_msl4 = rd_u16(p6 + profile_.msl4_off);
    uint16_t alt_agl4 = rd_u16(p6 + profile_.agl4_off);
    uint16_t z_half_ft = rd_u16(p6 + 0x08u);

    // Detect active 3D flight BOTH airborne (kias > 10) AND sitting at 0..10 kts on the runway / zoom-in
    // in Campaign / Red Flag / Training missions (so Hybrid Mode & 20-MIPS Auto-Turbo engage immediately!)
    int32_t cam_x0 = rd_i32(cam + 0x00u);
    int32_t cam_y0 = rd_i32(cam + 0x04u);
    int32_t pos_x0 = rd_i32(p6 + 0x00u);
    int32_t pos_y0 = rd_i32(p6 + 0x04u);
    bool has_valid_cam0 = (cam_x0 != 0 || cam_y0 != 0) &&
                          std::abs(cam_x0) < 10000000 && std::abs(cam_y0) < 10000000;
    bool has_valid_pos0 = (pos_x0 != 0 || pos_y0 != 0) &&
                          std::abs(pos_x0) < 10000000 && std::abs(pos_y0) < 10000000;
    bool airborne = (kias > 10 && kias < 1800 && alt_msl4 < 25000);
    bool on_runway_3d = (in_3d_flight_session_ && has_valid_cam0 && has_valid_pos0 &&
                         kias < 1800 && alt_msl4 < 25000);
    (void)rpm;

    if (airborne || on_runway_3d) {
        t.in_flight = true;
        t.world_x = static_cast<double>(rd_i32(p6 + 0x00u));
        t.world_y = static_cast<double>(rd_i32(p6 + 0x04u));
        t.altitude_ft = (z_half_ft > 0) ? (static_cast<float>(z_half_ft) * 0.5f)
                                        : (static_cast<float>(alt_msl4) * 4.0f);
        t.agl_ft = static_cast<float>(alt_agl4) * 4.0f;

        // Falcon 3.0 uses 14-bit binary angles (0..16383 = 0..360 deg) for Yaw, Pitch, and Roll!
        // Sign-extend 14-bit angles into [-8192 .. +8191] (-180 .. +180 deg) so negative pitch/roll work properly.
        auto bam14_signed_deg = [](uint16_t raw) -> float {
            int v = static_cast<int>(raw & 0x3FFF);
            if (v >= 8192) v -= 16384;
            return static_cast<float>(v) * (360.0f / 16384.0f);
        };

        uint16_t raw_yaw = rd_u16(p6 + 0x0Cu) & 0x3FFF;
        uint16_t raw_pitch = rd_u16(p6 + 0x0Eu);
        uint16_t raw_roll = rd_u16(p6 + 0x10u);

        t.yaw_deg = static_cast<float>(raw_yaw) * (360.0f / 16384.0f);
        t.pitch_deg = std::clamp(bam14_signed_deg(raw_pitch), -89.5f, 89.5f);
        t.roll_deg = bam14_signed_deg(raw_roll);
        t.airspeed_kts = static_cast<float>(kias);
        t.mach = static_cast<float>(mach100) * 0.01f;
        t.g_load = std::clamp(static_cast<float>(rd_i16(p6 + profile_.g_load_off)) * 0.1f, -4.0f, 12.0f);

        // Read active View Mode BEFORE resolving camera so Forward Cockpit (0) never flickers!
        // (0=Fwd, 2=Left, 4=Right, 6=Rear, 18=External/Chase/Flyby, 26=Padlock)
        t.view_mode = rd_u16(vma);
        t.padlock_active = (t.view_mode == 26);

        // Read active 3D camera state (tracks Side/Rear, Padlock, Chase, Orbit, Flyby, Enemy!)
        int32_t cx = rd_i32(cam + 0x00u);
        int32_t cy = rd_i32(cam + 0x04u);
        int32_t cz = rd_i32(cam + 0x08u);
        bool valid_cam = (cx != 0 || cy != 0) && std::abs(cx) < 10000000 && std::abs(cy) < 10000000 && cz >= 0 && cz < 200000;
        double dcz = valid_cam ? std::abs(static_cast<double>(cz) - static_cast<double>(z_half_ft)) : 0.0;
        bool is_cockpit_view = (t.view_mode == 0 || t.view_mode == 2 || t.view_mode == 4 || t.view_mode == 6 || t.view_mode == 26);

        if (t.view_mode == 0 && (!valid_cam || dcz < 250.0)) {
            // In Forward Cockpit (view_mode == 0, excluding high-altitude Campaign zoom-in where dcz >= 250),
            // lock directly to the atomic player aircraft state!
            t.has_camera = true;
            t.cam_x = t.world_x;
            t.cam_y = t.world_y;
            t.cam_alt_ft = t.altitude_ft;
            t.cam_yaw_deg = t.yaw_deg;
            t.cam_pitch_deg = t.pitch_deg;
            t.cam_roll_deg = t.roll_deg;
            t.is_external_cam = false;
        } else if (valid_cam) {
            t.has_camera = true;
            t.cam_x = static_cast<double>(cx);
            t.cam_y = static_cast<double>(cy);
            t.cam_alt_ft = static_cast<float>(cz) * 0.5f;
            t.cam_yaw_deg = static_cast<float>(rd_u16(cam + 0x0Cu) & 0x3FFFu) * (360.0f / 16384.0f);
            t.cam_pitch_deg = std::clamp(bam14_signed_deg(rd_u16(cam + 0x0Eu)), -89.5f, 89.5f);
            t.cam_roll_deg = bam14_signed_deg(rd_u16(cam + 0x10u));

            double dcx = t.cam_x - t.world_x;
            double dcy = t.cam_y - t.world_y;
            double cam_dist = std::sqrt(dcx * dcx + dcy * dcy + dcz * dcz);
            // Inside-cockpit views (2/4/6/26) are never external; external views (18) or Campaign zoom-in (dcz >= 250) are external
            t.is_external_cam = is_cockpit_view ? (dcz >= 250.0) : (cam_dist > 120.0 || t.view_mode == 18);
        } else {
            t.cam_x = t.world_x;
            t.cam_y = t.world_y;
            t.cam_alt_ft = t.altitude_ft;
            t.cam_yaw_deg = t.yaw_deg;
            t.cam_pitch_deg = t.pitch_deg;
            t.cam_roll_deg = t.roll_deg;
            t.is_external_cam = false;
        }

        // 1. Read all active 3D aircraft (16 slots, stride 0x2C = 44 B)
        // Slot 0 = player aircraft (rendered in external views!), slots 1..15 = active AI aircraft
        t.entities.reserve(16);
        for (int i = 0; i < 16; ++i) {
            uint32_t base = ent + static_cast<uint32_t>(i) * 0x2Cu;
            uint16_t type_w = rd_u16(base + 0x26);
            if ((type_w & 0x8000u) == 0) continue; // Inactive slot
            int32_t ex = rd_i32(base + 0x00);
            int32_t ey = rd_i32(base + 0x04);
            int32_t ez = rd_i32(base + 0x08);
            if (ex == 0 && ey == 0 && ez == 0) continue;
            if (std::abs(ex) > 10000000 || std::abs(ey) > 10000000 || ez < -5000 || ez > 200000) continue;

            WorldEntity ent_obj;
            ent_obj.world_x = static_cast<double>(ex);
            ent_obj.world_y = static_cast<double>(ey);
            ent_obj.world_z = static_cast<double>(ez);
            ent_obj.yaw_deg = static_cast<float>(rd_u16(base + 0x0C) & 0x3FFFu) * (360.0f / 16384.0f);
            ent_obj.pitch_deg = std::clamp(bam14_signed_deg(rd_u16(base + 0x0E)), -89.5f, 89.5f);
            ent_obj.roll_deg = bam14_signed_deg(rd_u16(base + 0x10));
            ent_obj.flight_id = rd_u16(base + 0x22);
            ent_obj.flags = rd_u16(base + 0x24);
            ent_obj.type_id = type_w & 0x7FFFu;
            ent_obj.is_player = (i == 0);
            ent_obj.is_ground = false;
            t.entities.push_back(ent_obj);
        }

        // 2. Read all active ground vehicles, armored columns, AAA & SAM batteries (100 slots, stride 0x36 = 54 B)
        t.ground_units.reserve(72);
        for (int i = 0; i < 100; ++i) {
            uint32_t base = gnd + static_cast<uint32_t>(i) * 0x36u;
            uint16_t flags = rd_u16(base + 0x24);
            uint16_t type_w = rd_u16(base + 0x26);
            if ((flags & 0x4014u) != 0x4014u || (type_w & 0x8000u) == 0) continue;
            uint16_t hp = rd_u16(base + 0x1E);
            if (hp == 0) continue; // Destroyed ground unit
            int32_t gx = rd_i32(base + 0x00);
            int32_t gy = rd_i32(base + 0x04);
            int32_t gz = rd_i32(base + 0x08);
            if (gx == 0 && gy == 0) continue;
            if (std::abs(gx) > 10000000 || std::abs(gy) > 10000000 || gz < -5000 || gz > 100000) continue;

            WorldEntity g;
            g.world_x = static_cast<double>(gx);
            g.world_y = static_cast<double>(gy);
            g.world_z = static_cast<double>(std::max<int32_t>(0, gz));
            g.yaw_deg = static_cast<float>(rd_u16(base + 0x0C) & 0x3FFFu) * (360.0f / 16384.0f);
            g.pitch_deg = 0.0f;
            g.roll_deg = 0.0f;
            g.flight_id = rd_u16(base + 0x22);
            g.flags = flags;
            g.type_id = type_w & 0x7FFFu;
            g.is_player = false;
            g.is_ground = true;
            t.ground_units.push_back(g);
        }

        // 3. Read all active in-flight SAM & AAM missiles (104 slots, stride 0x2A = 42 B, flags == 0x4015)
        t.missiles.reserve(16);
        for (int i = 0; i < 104; ++i) {
            uint32_t base = msl + static_cast<uint32_t>(i) * 0x2Au;
            uint16_t flags = rd_u16(base + 0x24);
            if ((flags & 0x4015u) != 0x4015u) continue;
            int32_t mx = rd_i32(base + 0x00);
            int32_t my = rd_i32(base + 0x04);
            int32_t mz = rd_i32(base + 0x08);
            if (mx == 0 && my == 0 && mz == 0) continue;
            if (std::abs(mx) > 10000000 || std::abs(my) > 10000000 || mz < -1000 || mz > 200000) continue;

            WorldMissile m;
            m.slot_id = static_cast<uint16_t>(i);
            m.world_x = static_cast<double>(mx);
            m.world_y = static_cast<double>(my);
            m.world_z = static_cast<double>(std::max<int32_t>(0, mz));
            m.yaw_deg = static_cast<float>(rd_u16(base + 0x0C) & 0x3FFFu) * (360.0f / 16384.0f);
            m.pitch_deg = std::clamp(bam14_signed_deg(rd_u16(base + 0x0E)), -89.5f, 89.5f);
            m.roll_deg = bam14_signed_deg(rd_u16(base + 0x10));
            m.age_ticks = rd_u16(base + 0x16);
            m.kind_id = rd_u16(base + 0x22);
            // Identify likely SAMs (climbing steeply or launched from low altitude / ground battery pool)
            m.is_sam = (m.world_z < 1200.0 && m.pitch_deg > 8.0f) || (m.pitch_deg > 22.0f) || (i >= 32);
            t.missiles.push_back(m);
        }

        // 4. Resolve the EXACT unclamped 3D Padlock target (aircraft OR incoming SAM/AAM) when in Padlock view (26)!
        if (t.padlock_active && profile_.padlock_vec_addr != 0 && profile_.padlock_name_addr != 0) {
            // Read ASCII target name ("SU25", "MIG29", "SAM", "AIM-9", "MISSILE", etc.)
            int nlen = 0;
            for (int k = 0; k < 12; ++k) {
                char c = static_cast<char>(mem_[profile_.padlock_name_addr + static_cast<uint32_t>(k)]);
                if (c == '\0') break;
                if (c >= 32 && c < 127) t.padlock_target_name[nlen++] = c;
            }
            while (nlen > 0 && t.padlock_target_name[nlen - 1] == ' ') nlen--;
            t.padlock_target_name[nlen] = '\0';

            bool name_same = (std::strcmp(t.padlock_target_name, padlock_sticky_name_) == 0);
            bool name_looks_missile = (std::strstr(t.padlock_target_name, "SAM") != nullptr ||
                                       std::strstr(t.padlock_target_name, "MIS") != nullptr ||
                                       std::strstr(t.padlock_target_name, "AIM") != nullptr ||
                                       std::strstr(t.padlock_target_name, "SA-") != nullptr ||
                                       std::strstr(t.padlock_target_name, "AA-") != nullptr);

            // Live unclamped Padlock target vector:
            //   +0 (uint8) = fixed-point shift factor
            //   +2/+4/+6 (int16) = Aircraft body-frame (Right, Down, Forward)
            //   +8/+A/+C (int16) = World-space (dE, -dZ, dN)
            const uint32_t pv = profile_.padlock_vec_addr;
            uint8_t shift = mem_[pv];
            if (shift == 0 || shift > 10) shift = 1;
            double dE_raw = static_cast<double>(rd_i16(pv + 8u));
            double dZ_raw = -static_cast<double>(rd_i16(pv + 10u));
            double dN_raw = static_cast<double>(rd_i16(pv + 12u));
            double raw_len = std::sqrt(dN_raw * dN_raw + dE_raw * dE_raw + dZ_raw * dZ_raw);

            if (raw_len > 2.0) {
                // Unit direction vector in world space is 100% scale-invariant across ALL ranges (0.05..80+ NM)!
                double dir_n = dN_raw / raw_len;
                double dir_e = dE_raw / raw_len;
                double dir_z = dZ_raw / raw_len;

                double scale_hf = static_cast<double>(1u << shift);
                double raw_tx = t.world_x + dN_raw * scale_hf;
                double raw_ty = t.world_y + dE_raw * scale_hf;
                double raw_tz = std::max(0.0, static_cast<double>(t.altitude_ft) * 2.0 + dZ_raw * scale_hf);

                // Match against live WorldMissile / WorldEntity / GroundUnit along this exact 3D ray!
                // Using scale-invariant ray alignment (cos > 0.992) + power-of-2 distance check handles
                // BOTH close (<2.7 NM) and far (4..40+ NM) SAMs & aircraft with 0.01 deg precision!
                double best_score = 1e18;
                int best_kind = 0;
                int best_slot = -1;
                double best_x = raw_tx, best_y = raw_ty, best_z = raw_tz;

                auto try_match = [&](int kind, int slot, double cx, double cy, double cz) {
                    double dn = cx - t.world_x;
                    double de = cy - t.world_y;
                    double dz = cz - static_cast<double>(t.altitude_ft) * 2.0;
                    double dist = std::sqrt(dn * dn + de * de + dz * dz);
                    if (dist < 20.0 || dist > 600000.0) return;

                    double cos_a = (dn * dir_n + de * dir_e + dz * dir_z) / dist;
                    if (cos_a < 0.985) return; // Must lie within ~10 deg of Falcon's exact 3D Padlock vector!

                    // Check best power-of-2 shift match (Falcon scales raw_len by 2^k so int16 never overflows)
                    double min_rel_dist_err = 1e9;
                    for (int k = 0; k <= 6; ++k) {
                        double est_dist = raw_len * static_cast<double>(1u << k);
                        double rel = std::abs(dist - est_dist) / std::max(200.0, est_dist);
                        if (rel < min_rel_dist_err) min_rel_dist_err = rel;
                    }
                    double ang_err = 1.0 - cos_a;
                    double score = ang_err * 2000.0 + min_rel_dist_err * 40.0;
                    if (name_looks_missile && kind == 2) score -= 4.0;
                    if (!name_looks_missile && kind == 1) score -= 2.0;
                    if (name_same && kind == padlock_sticky_kind_ && slot == padlock_sticky_slot_) {
                        score -= 6.0;
                    }
                    if (score < best_score) {
                        best_score = score;
                        best_kind = kind;
                        best_slot = slot;
                        best_x = cx; best_y = cy; best_z = cz;
                    }
                };

                for (size_t i = 0; i < t.missiles.size(); ++i) {
                    try_match(2, static_cast<int>(t.missiles[i].slot_id),
                              t.missiles[i].world_x, t.missiles[i].world_y, t.missiles[i].world_z);
                }
                for (size_t i = 0; i < t.entities.size(); ++i) {
                    if (t.entities[i].is_player) continue;
                    try_match(1, static_cast<int>(i),
                              t.entities[i].world_x, t.entities[i].world_y, t.entities[i].world_z);
                }
                for (size_t i = 0; i < t.ground_units.size(); ++i) {
                    try_match(3, static_cast<int>(i),
                              t.ground_units[i].world_x, t.ground_units[i].world_y, t.ground_units[i].world_z);
                }

                t.padlock_has_target = true;
                t.padlock_target_is_missile = (best_kind == 2) || name_looks_missile;
                if (best_kind != 0 && best_score < 25.0) {
                    t.padlock_target_x = best_x;
                    t.padlock_target_y = best_y;
                    t.padlock_target_z = best_z;
                    padlock_sticky_kind_ = best_kind;
                    padlock_sticky_slot_ = best_slot;
                    std::strncpy(padlock_sticky_name_, t.padlock_target_name, sizeof(padlock_sticky_name_) - 1);
                } else {
                    // Exact direction directly from Falcon's 0x4619C/98/9A vector (always points dead-on!)
                    t.padlock_target_x = raw_tx;
                    t.padlock_target_y = raw_ty;
                    t.padlock_target_z = raw_tz;
                }
            }

            if (t.padlock_has_target) {
                double dx = (t.padlock_target_x - t.world_x) * 0.5;
                double dy = (t.padlock_target_y - t.world_y) * 0.5;
                double dz = (t.padlock_target_z * 0.5) - static_cast<double>(t.altitude_ft);
                t.padlock_range_nm = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / 6076.12);
            }
        } else {
            padlock_sticky_kind_ = 0;
            padlock_sticky_slot_ = -1;
            padlock_sticky_name_[0] = '\0';
        }
    }
    return t;
}

bool DosMachine::harvest_cockpit_views(Cockpit3D& out) {
    if (!read_flight_telemetry().in_flight) return false;
    auto rd_u16 = [&](uint32_t addr) -> uint16_t {
        return static_cast<uint16_t>(mem_[addr] | (static_cast<uint16_t>(mem_[addr + 1]) << 8));
    };

    // Only harvest when cleanly sitting in Forward Cockpit (view_mode == 0) AND the frame is fully rendered (not black fade-in!)
    if (rd_u16(profile_.view_mode_addr) != 0) return false;

    std::vector<uint8_t> idx_buf(Cockpit3D::kSpriteW * Cockpit3D::kSpriteH);
    std::vector<uint8_t> prev_buf(Cockpit3D::kSpriteW * Cockpit3D::kSpriteH);

    auto is_valid_cockpit_frame = [](const std::vector<uint8_t>& buf) -> bool {
        int world_px = 0;
        int nonblack_pit = 0;
        for (uint8_t v : buf) {
            if (v >= 128) world_px++;
            else if (v > 0) nonblack_pit++;
        }
        // Every real Falcon 3.0 cockpit view (Fwd/Left/Right/Rear) has >10,000 sky/world pixels (>=128)
        // AND >12,000 non-black cockpit art pixels (1..127). Reject black fade-in / blank pages!
        return (world_px >= 8000 && nonblack_pit >= 10000);
    };

    vga_.render_indices_200(idx_buf.data());
    if (!is_valid_cockpit_frame(idx_buf)) {
        return false; // Screen is still fading in from black — try again next frame!
    }

    // Save timers so harvesting takes ~0 ms of simulated flight time
    double saved_pit = pit_time_accum_;
    double saved_bda = bda_time_accum_;
    double saved_vga = vga_time_accum_;

    // 1. Capture validated Forward Cockpit (already active!)
    out.set_view_indices(Cockpit3D::ViewForward, idx_buf.data());
    prev_buf = idx_buf;

    auto switch_and_capture = [&](uint8_t scancode, uint8_t ascii, uint16_t expected_mode,
                                  Cockpit3D::ViewSlot slot, bool do_capture) {
        push_key(scancode, ascii, true);
        step(60000);
        push_key(scancode, ascii, false);
        bool switched = false;
        for (int iter = 0; iter < 60; ++iter) {
            step(50000);
            on_frame_tick();
            if (rd_u16(profile_.view_mode_addr) == expected_mode) {
                switched = true;
                // Wait until Falcon actually blits the new cockpit sprite to the active VRAM page
                // (verify both valid cockpit+world pixels AND pixel change from previous view!)
                for (int k = 0; k < 24; ++k) {
                    step(50000);
                    on_frame_tick();
                    vga_.render_indices_200(idx_buf.data());
                    if (is_valid_cockpit_frame(idx_buf)) {
                        int diff_px = 0;
                        for (size_t p = 120 * 320; p < 190 * 320; p += 17) {
                            if (idx_buf[p] != prev_buf[p]) diff_px++;
                        }
                        if (!do_capture || diff_px > 200) {
                            // Step 2 tiny chunks more to ensure full scanline blit finished
                            step(40000); on_frame_tick();
                            vga_.render_indices_200(idx_buf.data());
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (do_capture && switched && is_valid_cockpit_frame(idx_buf)) {
            out.set_view_indices(slot, idx_buf.data());
            prev_buf = idx_buf;
        }
    };

    // 2. Capture Left Console (key '4' -> view_mode 2)
    switch_and_capture(0x05, '4', 2, Cockpit3D::ViewLeft, true);
    // 3. Capture Right Console (key '6' -> view_mode 4)
    switch_and_capture(0x07, '6', 4, Cockpit3D::ViewRight, true);
    // 4. Capture Rear 6-o'clock (key '5' -> view_mode 6)
    switch_and_capture(0x06, '5', 6, Cockpit3D::ViewRear, true);
    // 5. Return cleanly to Forward Cockpit (Falcon 3.0 key '3' -> view_mode 0!)
    switch_and_capture(0x04, '3', 0, Cockpit3D::ViewForward, false);

    pit_time_accum_ = saved_pit;
    bda_time_accum_ = saved_bda;
    vga_time_accum_ = saved_vga;

    if (out.has_all_views()) {
        log("3D VIRTUAL COCKPIT: Harvested all 4 native F-16C cockpit views (Fwd/Left/Right/Rear)!");
        return true;
    }
    return false;
}

void DosMachine::read_player_fm_extras(float& weight_lbs, float& throttle_01) const {
    const uint32_t p6 = profile_.player_6dof_addr;
    auto rd_u16 = [&](uint32_t addr) -> uint16_t {
        return static_cast<uint16_t>(mem_[addr] | (static_cast<uint16_t>(mem_[addr + 1]) << 8));
    };
    uint16_t w = rd_u16(p6 + profile_.weight_off);
    uint16_t thr = rd_u16(p6 + 0x18u);
    weight_lbs = (w >= 14000 && w <= 45000) ? static_cast<float>(w) : 21500.0f;
    // Falcon stores throttle/RPM 0..100 (or 0..115 in AB) at p6 + 0x18
    throttle_01 = std::clamp(static_cast<float>(thr) / 100.0f, 0.0f, 1.0f);
}

void DosMachine::read_flight_audio_extras(float& rpm_pct, float& throttle_01, int& ab_stage, float& g_load) const {
    const uint32_t p6 = profile_.player_6dof_addr;
    auto rd_u16 = [&](uint32_t addr) -> uint16_t {
        return static_cast<uint16_t>(mem_[addr] | (static_cast<uint16_t>(mem_[addr + 1]) << 8));
    };
    uint16_t rpm = rd_u16(p6 + 0x18u);
    uint16_t raw_thr = rd_u16(p6 + 0x2Eu);
    uint16_t ab = rd_u16(p6 + 0x1Eu);
    int16_t g10 = static_cast<int16_t>(rd_u16(p6 + profile_.g_load_off));
    rpm_pct = std::clamp(static_cast<float>(rpm), 0.0f, 115.0f);
    throttle_01 = std::clamp(static_cast<float>(raw_thr) / 305.0f, 0.0f, 1.0f);
    ab_stage = (mem_[p6 + profile_.ab_flag_off] != 0 || ab > 0) ? std::clamp<int>(static_cast<int>(ab), 1, 5) : 0;
    g_load = std::clamp(static_cast<float>(g10) * 0.1f, -4.0f, 12.0f);
}

void DosMachine::write_player_flight_state(const F16SimState& st, bool sync_forward_cam, bool write_throttle) {
    const uint32_t p6  = profile_.player_6dof_addr;
    const uint32_t ent = profile_.entity_array_addr;
    const uint32_t cam = profile_.camera_addr;
    auto wr_u16 = [&](uint32_t addr, uint16_t v) {
        mem_[addr + 0] = static_cast<uint8_t>(v & 0xFF);
        mem_[addr + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto wr_i16 = [&](uint32_t addr, int16_t v) {
        wr_u16(addr, static_cast<uint16_t>(v));
    };
    auto wr_i32 = [&](uint32_t addr, int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        mem_[addr + 0] = static_cast<uint8_t>(u & 0xFF);
        mem_[addr + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
        mem_[addr + 2] = static_cast<uint8_t>((u >> 16) & 0xFF);
        mem_[addr + 3] = static_cast<uint8_t>((u >> 24) & 0xFF);
    };
    auto deg_to_bam14 = [](double deg) -> uint16_t {
        double d = std::fmod(deg, 360.0);
        if (d < 0.0) d += 360.0;
        return static_cast<uint16_t>(std::lround(d * (16384.0 / 360.0))) & 0x3FFFu;
    };

    int32_t wx = static_cast<int32_t>(std::llround(st.world_x));
    int32_t wy = static_cast<int32_t>(std::llround(st.world_y));
    int32_t wz = static_cast<int32_t>(std::llround(std::clamp(st.alt_ft, 0.0, 65000.0) * 2.0));
    uint16_t yaw_b = deg_to_bam14(st.yaw_deg);
    uint16_t pitch_b = deg_to_bam14(st.pitch_deg);
    uint16_t roll_b = deg_to_bam14(st.roll_deg);
    uint16_t fp_yaw_b = deg_to_bam14(st.fp_yaw_deg);
    uint16_t fp_pitch_b = deg_to_bam14(st.fp_pitch_deg);

    // 1. Player 6-DOF flight struct
    wr_i32(p6 + 0x00u, wx);
    wr_i32(p6 + 0x04u, wy);
    wr_u16(p6 + 0x08u, static_cast<uint16_t>(std::clamp(wz, 0, 65535)));
    wr_u16(p6 + 0x0Cu, yaw_b);
    wr_u16(p6 + 0x0Eu, pitch_b);
    wr_u16(p6 + 0x10u, roll_b);
    wr_u16(p6 + 0x12u, static_cast<uint16_t>(std::clamp(std::lround(st.mach * 100.0), 0L, 300L)));
    wr_u16(p6 + 0x14u, static_cast<uint16_t>(std::clamp(std::lround(st.kias), 0L, 1600L)));
    if (write_throttle) {
        double t = std::clamp(st.throttle_cmd, 0.0, 1.0);
        // Sync real throttle command (+0x2E: 0..183=IDLE..MIL, 183..305=AB1..AB5),
        // AB stage (+0x1E: 0..5), AB lamp flag (+ab_flag_off), and RPM display (+0x18: 0..100)!
        uint16_t raw_thr = 0;
        uint16_t ab_stage = 0;
        if (t <= 0.78) {
            raw_thr = static_cast<uint16_t>(std::lround((t / 0.78) * 183.0));
            ab_stage = 0;
        } else {
            double ab_f = (t - 0.78) / 0.22;
            raw_thr = static_cast<uint16_t>(std::lround(183.0 + ab_f * 122.0));
            ab_stage = static_cast<uint16_t>(std::clamp<long>(std::lround(1.0 + ab_f * 4.0), 1L, 5L));
        }
        wr_u16(p6 + 0x2Eu, raw_thr);
        wr_u16(p6 + 0x1Eu, ab_stage);
        mem_[p6 + profile_.ab_flag_off] = (ab_stage > 0) ? 0xFF : 0x00;
        wr_u16(p6 + 0x18u, static_cast<uint16_t>(std::clamp(std::lround(st.rpm_pct), 0L, 100L)));
    }

    // Roll rate, flight path velocity vector (for HUD velocity vector & EEGS gun funnel!), MSL/AGL, TAS, G-load
    wr_i16(p6 + 0x38u, static_cast<int16_t>(std::clamp(std::lround(st.roll_rate_dps * 2.0), -1000L, 1000L)));
    wr_u16(p6 + 0x3Au, fp_yaw_b);
    wr_u16(p6 + 0x3Cu, fp_pitch_b);
    wr_u16(p6 + profile_.msl4_off, static_cast<uint16_t>(std::clamp(std::lround(st.alt_ft * 0.25), 0L, 20000L)));
    wr_u16(p6 + profile_.agl4_off, static_cast<uint16_t>(std::clamp(std::lround(std::max(0.0, st.alt_ft - st.ground_alt_ft) * 0.25), 0L, 20000L)));
    wr_u16(p6 + profile_.tas_off, static_cast<uint16_t>(std::clamp(std::lround(st.tas_fps), 0L, 3000L)));
    wr_i16(p6 + profile_.vel_off + 0u, static_cast<int16_t>(std::clamp(std::lround(st.vn_fps * 0.25), -2000L, 2000L)));
    wr_i16(p6 + profile_.vel_off + 2u, static_cast<int16_t>(std::clamp(std::lround(st.ve_fps * 0.25), -2000L, 2000L)));
    wr_i16(p6 + profile_.vel_off + 4u, static_cast<int16_t>(std::clamp(std::lround(st.vu_fps * 0.25), -2000L, 2000L)));
    wr_u16(p6 + profile_.vel_off + 6u, static_cast<uint16_t>(std::clamp(wz, 0, 65535)));
    wr_i16(p6 + profile_.g_load_off, static_cast<int16_t>(std::clamp(std::lround(st.g_load * 10.0), -40L, 120L)));

    // 2. Player 3D entity slot 0 (used by AI, radar, weapons, & external chase/orbit views!)
    wr_i32(ent + 0x00u, wx);
    wr_i32(ent + 0x04u, wy);
    wr_i32(ent + 0x08u, wz);
    wr_u16(ent + 0x0Cu, yaw_b);
    wr_u16(ent + 0x0Eu, pitch_b);
    wr_u16(ent + 0x10u, roll_b);

    // 3. Forward cockpit camera (only override when sitting in forward cockpit so Chase/Padlock/Side views still work!)
    if (sync_forward_cam) {
        wr_i32(cam + 0x00u, wx);
        wr_i32(cam + 0x04u, wy);
        wr_i32(cam + 0x08u, wz);
        wr_u16(cam + 0x0Cu, yaw_b);
        wr_u16(cam + 0x0Eu, pitch_b);
        wr_u16(cam + 0x10u, roll_b);
    }
}

void DosMachine::set_sam_mod_preset(SamModPreset preset) {
    sam_mod_preset_ = preset;
    apply_sam_mod_in_ram();
}

bool DosMachine::apply_sam_mod_in_ram() {
    static constexpr char kSig[] = "AIM-9P  AIM-9M  ";
    auto matches_sig = [&](uint32_t addr) -> bool {
        if (addr < 0x1000 || addr + 16 > 0xA0000) return false;
        return std::memcmp(&mem_[addr], kSig, 16) == 0;
    };

    if (!matches_sig(sam_sig_addr_)) {
        sam_sig_addr_ = 0;
        const uint8_t* base = mem_.data();
        const uint8_t* found = std::search(base + 0x1000, base + 0xA0000,
                                           reinterpret_cast<const uint8_t*>(kSig),
                                           reinterpret_cast<const uint8_t*>(kSig) + 16);
        if (found == base + 0xA0000) return false;
        sam_sig_addr_ = static_cast<uint32_t>(found - base);
        sam_vanilla_saved_ = false;
    }

    // Verified across FALCONCD.EXE, HORNETCD.EXE, and MIGCD.EXE:
    //   missile_table[16] (stride 18 B) starts at sig_addr - 0x2A6
    //     idx 12 = SAM (R)  @ sig_addr - 0x2A6 + 12*18 = sig_addr - 0x1CE
    //     idx 13 = SAM (IR) @ sig_addr - 0x2A6 + 13*18 = sig_addr - 0x1BC
    //   cm_table[16] (stride 2 B) starts at sig_addr - 0xB6
    //     idx 12 = SAM (R)  @ sig_addr - 0xB6 + 24 = sig_addr - 0x9E
    //     idx 13 = SAM (IR) @ sig_addr - 0xB6 + 26 = sig_addr - 0x9C
    uint32_t sam_r_addr  = sam_sig_addr_ - 0x1CEu;
    uint32_t sam_ir_addr = sam_sig_addr_ - 0x1BCu;
    uint32_t cm_r_addr   = sam_sig_addr_ - 0x9Eu;
    uint32_t cm_ir_addr  = sam_sig_addr_ - 0x9Cu;

    if (!sam_vanilla_saved_) {
        std::memcpy(sam_vanilla_structs_.data(), &mem_[sam_r_addr], 36);
        std::memcpy(sam_vanilla_cm_.data(), &mem_[cm_r_addr], 4);
        sam_vanilla_saved_ = true;
    }

    // Always start cleanly from vanilla bytes before applying preset
    std::memcpy(&mem_[sam_r_addr], sam_vanilla_structs_.data(), 36);
    std::memcpy(&mem_[cm_r_addr], sam_vanilla_cm_.data(), 4);

    auto wr_u16 = [&](uint32_t addr, uint16_t v) {
        mem_[addr + 0] = static_cast<uint8_t>(v & 0xFF);
        mem_[addr + 1] = static_cast<uint8_t>(v >> 8);
    };

    if (sam_mod_preset_ == SamModPreset::HistoricalF3Msl) {
        // Bill White F3MSL SA-7B (Strela-2M) + Realistic Gulf War Radar SAMs (SA-2/SA-3/SA-6/SA-8)
        // SAM (R): realistic burn=165, turn=1850 (down from 4096!), blast=128, CM=240 (down from 750!)
        wr_u16(sam_r_addr + 0x06, 165);
        wr_u16(sam_r_addr + 0x08, 1850);
        wr_u16(sam_r_addr + 0x10, 128);
        wr_u16(cm_r_addr, 240);

        // SAM (IR) SA-7B Strela-2M: fixes the infamous 0x6400 (80x!) agility bug & 0x01 flare immunity!
        mem_[sam_ir_addr + 0x05] = 0x48;    // Realistic Mach 1.5 peak speed
        wr_u16(sam_ir_addr + 0x06, 80);     // Burn time = 80 (down from 202!)
        wr_u16(sam_ir_addr + 0x08, 1550);   // Realistic ~10-12G pull (down from 6144!)
        wr_u16(sam_ir_addr + 0x0A, 0x0140); // Fixes 0x6400 (25,600 -> 320, matching AIM-9M!)
        wr_u16(sam_ir_addr + 0x0E, 0x0686); // F3MSL SA-7B max range
        wr_u16(cm_ir_addr, 0x0025);         // F3MSL SA-7B flare susceptibility (up from 1!)
    } else if (sam_mod_preset_ == SamModPreset::EarlyStrela) {
        // SA-7A (Strela-2A) + Relaxed Radar SAMs
        wr_u16(sam_r_addr + 0x06, 130);
        wr_u16(sam_r_addr + 0x08, 1350);
        wr_u16(sam_r_addr + 0x10, 96);
        wr_u16(cm_r_addr, 160);

        mem_[sam_ir_addr + 0x05] = 0x40;
        wr_u16(sam_ir_addr + 0x06, 40);
        wr_u16(sam_ir_addr + 0x08, 1100);
        wr_u16(sam_ir_addr + 0x0A, 0x0100);
        wr_u16(sam_ir_addr + 0x0E, 0x0580);
        wr_u16(cm_ir_addr, 0x0050);
    } else if (sam_mod_preset_ == SamModPreset::LowThreat) {
        // Training / Low SAM Threat
        wr_u16(sam_r_addr + 0x06, 95);
        wr_u16(sam_r_addr + 0x08, 950);
        wr_u16(sam_r_addr + 0x10, 64);
        wr_u16(cm_r_addr, 100);

        mem_[sam_ir_addr + 0x05] = 0x38;
        wr_u16(sam_ir_addr + 0x06, 30);
        wr_u16(sam_ir_addr + 0x08, 850);
        wr_u16(sam_ir_addr + 0x0A, 0x00E0);
        wr_u16(sam_ir_addr + 0x0E, 0x0400);
        wr_u16(cm_ir_addr, 0x0090);
    }
    return true;
}

SamModStatus DosMachine::read_sam_mod_status() const {
    SamModStatus st;
    st.preset = sam_mod_preset_;
    static constexpr char kSig[] = "AIM-9P  AIM-9M  ";
    auto matches_sig = [&](uint32_t addr) -> bool {
        if (addr < 0x1000 || addr + 16 > 0xA0000) return false;
        return std::memcmp(&mem_[addr], kSig, 16) == 0;
    };
    if (!matches_sig(sam_sig_addr_)) {
        const uint8_t* base = mem_.data();
        const uint8_t* found = std::search(base + 0x1000, base + 0xA0000,
                                           reinterpret_cast<const uint8_t*>(kSig),
                                           reinterpret_cast<const uint8_t*>(kSig) + 16);
        if (found == base + 0xA0000) return st;
        sam_sig_addr_ = static_cast<uint32_t>(found - base);
    }
    st.table_found = true;
    st.sig_addr = sam_sig_addr_;
    auto rd_u16 = [&](uint32_t addr) -> uint16_t {
        return static_cast<uint16_t>(mem_[addr] | (static_cast<uint16_t>(mem_[addr + 1]) << 8));
    };
    uint32_t sam_r_addr  = sam_sig_addr_ - 0x1CEu;
    uint32_t sam_ir_addr = sam_sig_addr_ - 0x1BCu;
    st.sam_r_burn   = rd_u16(sam_r_addr + 0x06);
    st.sam_r_turn   = rd_u16(sam_r_addr + 0x08);
    st.sam_r_blast  = rd_u16(sam_r_addr + 0x10);
    st.sam_r_cm     = rd_u16(sam_sig_addr_ - 0x9Eu);
    st.sam_ir_burn  = rd_u16(sam_ir_addr + 0x06);
    st.sam_ir_turn  = rd_u16(sam_ir_addr + 0x08);
    st.sam_ir_agi   = rd_u16(sam_ir_addr + 0x0A);
    st.sam_ir_cm    = rd_u16(sam_sig_addr_ - 0x9Cu);
    return st;
}

void DosMachine::deliver_hw_irq(uint8_t vec) {
    if (!emu_ || paused_ || exited_ || in_mouse_cb_) return;
    if ((emu_->x86.R_EFLG & 0x200u) == 0) return; // IF=0: interrupts masked
    uint16_t off = mem_read16(vec * 4 + 0);
    uint16_t seg = mem_read16(vec * 4 + 2);
    if (seg == 0 || seg == kHleStubSeg || seg == kEmmStubSeg) return;

    // Real-mode hardware IRQ entry: push FLAGS, CS, IP; clear IF & TF; jump to IVT[vec]
    // (Avoids libx86emu's x86emu_intr_raise collision that can swallow software INT 21h!)
    uint16_t sp = emu_->x86.R_SP;
    uint32_t ss_base = static_cast<uint32_t>(emu_->x86.R_SS) << 4;
    sp -= 2; mem_write16(ss_base + sp, static_cast<uint16_t>(emu_->x86.R_EFLG));
    sp -= 2; mem_write16(ss_base + sp, emu_->x86.R_CS);
    sp -= 2; mem_write16(ss_base + sp, emu_->x86.R_IP);
    emu_->x86.R_SP = sp;
    emu_->x86.R_EFLG &= ~0x0300u; // Clear IF and TF
    x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, seg);
    emu_->x86.R_IP = off;
}

uint64_t DosMachine::step(uint64_t max_instructions) {
    if (!emu_ || paused_ || exited_) return 0;

    // Compute instructions per PIT IRQ0 based on programmed PIT divisor (1.193182 MHz / reload)
    uint32_t reload = (pit_reload0_ == 0) ? 65536u : static_cast<uint32_t>(pit_reload0_);
    uint64_t insns_per_irq = std::max<uint64_t>(2000, (static_cast<uint64_t>(reload) * cpu_speed_ips_) / 1193182ULL);

    uint64_t done = 0;
    while (done < max_instructions && !paused_ && !exited_) {
        // Safely deliver queued keyboard/mouse hardware events ONLY when IF=1 and non-reentrant
        if ((emu_->x86.R_EFLG & 0x200u) && !in_mouse_cb_) {
            if (!pending_hw_scancodes_.empty()) {
                last_scancode_ = pending_hw_scancodes_.front();
                pending_hw_scancodes_.pop_front();
                deliver_hw_irq(0x09);
            }
            if (pending_mouse_mask_ != 0 && (emu_->x86.R_EFLG & 0x200u)) {
                uint16_t mask = pending_mouse_mask_;
                pending_mouse_mask_ = 0;
                invoke_mouse_callback(mask);
            }
        }

        uint64_t to_irq = (pit_insn_accum_ >= insns_per_irq) ? 1 : (insns_per_irq - pit_insn_accum_);
        uint64_t chunk = std::min(max_instructions - done, std::min<uint64_t>(to_irq, 8000ULL));

        emu_->max_instr = emu_->x86.R_TSC + chunk;
        uint64_t before = emu_->x86.R_TSC;
        x86emu_run(emu_, X86EMU_RUN_MAX_INSTR | X86EMU_RUN_NO_CODE);
        uint64_t ran = emu_->x86.R_TSC - before;
        if (ran == 0) break;

        done += ran;
        total_insns_ += ran;
        pit_insn_accum_ += ran;

        if (sb_irq_pending_ && total_insns_ >= sb_irq_due_insn_ && (emu_->x86.R_EFLG & 0x200u)) {
            sb_irq_pending_ = false;
            uint16_t seg0f = mem_read16(0x0F * 4 + 2);
            if (seg0f != kHleStubSeg && seg0f != 0) deliver_hw_irq(0x0F);
            else deliver_hw_irq(0x0D);
        }

        if (pit_insn_accum_ >= insns_per_irq) {
            pit_insn_accum_ -= insns_per_irq;
            if (emu_->x86.R_EFLG & 0x200u) {
                uint16_t seg08 = mem_read16(0x08 * 4 + 2);
                if (seg08 != kHleStubSeg && seg08 != 0) deliver_hw_irq(0x08);
                else deliver_hw_irq(0x1C);
            }
        }
    }
    return done;
}

uint64_t DosMachine::effective_cpu_speed_ips() const {
    if (auto_turbo_in_flight_) {
        if (disk_turbo_sec_ > 0.0) {
            return std::max<uint64_t>(flight_cpu_speed_ips_, 25000000ULL);
        }
        if (in_3d_flight_session_ || read_flight_telemetry().in_flight) {
            return flight_cpu_speed_ips_;
        }
    }
    return cpu_speed_ips_;
}

uint64_t DosMachine::step_realtime(double dt_sec) {
    if (!emu_ || paused_ || exited_) return 0;

    // Fast-Boot Skip Intro: run at uncapped native speed (~1.5M insns/frame) with virtual PIT ticks
    // and inject Space/Esc as Intro1 & Credits load until War Room opens (~1.5s real time!)
    if (fast_boot_active_) {
        if (total_insns_ > 320000000ULL) {
            // Safety cap in case a title uses a different menu file
            fast_boot_active_ = false;
            audio_.set_fast_boot_mute(false);
        } else {
            auto wall_start = std::chrono::steady_clock::now();
            uint64_t done = 0;
            // Run up to 14ms per host frame so window stays responsive while fast-booting!
            while (done < 2500000ULL && fast_boot_active_ && !paused_ && !exited_) {
                if (fast_boot_key_at_ > 0 && total_insns_ >= fast_boot_key_at_) {
                    if (fast_boot_stage_ >= 3) {
                        // War Room finished rendering (or fallback elapsed): exit fast-boot!
                        fast_boot_key_at_ = 0;
                        fast_boot_active_ = false;
                        audio_.set_fast_boot_mute(false);
                        log("FAST-BOOT COMPLETE -> War Room ready!");
                        break;
                    }
                    uint8_t sc = (fast_boot_stage_ == 1) ? 0x39 : 0x01; // Space for Intro, Esc for Title/Credits
                    uint8_t asc = (fast_boot_stage_ == 1) ? ' ' : 27;
                    push_key(sc, asc, true);
                    step(20000);
                    push_key(sc, asc, false);
                    // Re-arm pulse in case the intro/credits faded in after the first press
                    fast_boot_key_at_ = total_insns_ + 3500000ULL;
                }
                uint64_t ran = step(50000);
                on_frame_tick();
                done += ran;
                if (ran == 0) break;
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - wall_start).count();
                if (elapsed_us > 14000) break;
            }
            return done;
        }
    }

    dt_sec = std::clamp(dt_sec, 0.0, 0.05); // Cap dt at 50ms on lag spikes
    if (disk_turbo_sec_ > 0.0) {
        disk_turbo_sec_ = std::max(0.0, disk_turbo_sec_ - dt_sec);
    }
    auto wall_start = std::chrono::steady_clock::now();

    poll_ipx_udp();

    // Smooth host-driven Time Compression (1x..8x): scale simulated DOS clocks & CPU budget cleanly in flight!
    bool apply_tc = (time_scale_ > 1.01f) && (in_3d_flight_session_ || read_flight_telemetry().in_flight);
    double tc = apply_tc ? static_cast<double>(std::clamp(time_scale_, 1.0f, 8.0f)) : 1.0;
    double sim_dt = std::min(dt_sec * tc, 0.40);

    // 2. Real-time BIOS Data Area timer (18.2065 Hz, scaled by Time Compression)
    bda_time_accum_ += sim_dt;
    constexpr double kBdaPeriod = 1.0 / 18.2065;
    while (bda_time_accum_ >= kBdaPeriod) {
        bda_time_accum_ -= kBdaPeriod;
        bda_ticks_++;
        mem_write16(0x046C, static_cast<uint16_t>(bda_ticks_ & 0xFFFF));
        mem_write16(0x046E, static_cast<uint16_t>(bda_ticks_ >> 16));
    }

    // 3. Real-time CPU budget (auto-turbo in flight!) + real-time PIT IRQ0 + per-slice VGA 70.08Hz vblank phase
    constexpr double kVgaPeriod = 1.0 / 70.08;
    uint64_t active_ips = effective_cpu_speed_ips();
    // During Time Compression (2x..8x), cap per-simulated-second CPU rate to ~9 MIPS (still faster than a 486DX-33!)
    // so 4x/8x advance smoothly at 60 FPS without saturating a single CPU core!
    uint64_t sim_ips = apply_tc ? std::min<uint64_t>(active_ips, 9000000ULL) : active_ips;
    insn_budget_accum_ += sim_dt * static_cast<double>(sim_ips);
    uint64_t max_burst = std::max<uint64_t>(active_ips / 20,
                                            static_cast<uint64_t>(static_cast<double>(sim_ips) * tc / 16.0));
    if (insn_budget_accum_ > static_cast<double>(max_burst)) {
        insn_budget_accum_ = static_cast<double>(max_burst);
    }
    uint64_t target_insns = static_cast<uint64_t>(insn_budget_accum_);
    insn_budget_accum_ -= static_cast<double>(target_insns);

    uint32_t reload = (pit_reload0_ == 0) ? 65536u : static_cast<uint32_t>(pit_reload0_);
    double pit_period = static_cast<double>(reload) / 1193182.0;
    pit_time_accum_ += sim_dt;

    // Support high-frequency PIT0 interrupts up to ~30 kHz (Falcon 3.0 drives
    // Sound Blaster Direct DAC 0x10 voice comms at 11,256.4 Hz = ~188 ticks/16.6ms!)
    int irqs_due = 0;
    int max_irqs = apply_tc ? 768 : 512;
    while (pit_time_accum_ >= pit_period && irqs_due < max_irqs) {
        pit_time_accum_ -= pit_period;
        irqs_due++;
    }
    if (pit_time_accum_ > 0.030 * tc) pit_time_accum_ = 0.0;

    int slices = std::max(apply_tc ? static_cast<int>(8 * tc) : 8, irqs_due);
    double slice_dt = sim_dt / static_cast<double>(slices);
    uint64_t done = 0;
    uint64_t slice_insns = std::max<uint64_t>(80, target_insns / static_cast<uint64_t>(slices));

    const int64_t soft_budget_us = apply_tc ? 13000 : 11000;
    const int64_t hard_budget_us = apply_tc ? 15000 : 13000;
    for (int s = 0; s < slices && !paused_ && !exited_; ++s) {
        // Wall-clock guardrail: never spend >11-15ms in CPU stepping so host 60+ FPS stays smooth!
        bool over_soft_budget = false;
        if (s > 0 && (s & 3) == 0) {
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - wall_start).count();
            if (elapsed_us > hard_budget_us) {
                // Return undelivered PIT ticks back to accumulator
                if (s < irqs_due) {
                    pit_time_accum_ = std::min(0.025 * tc, pit_time_accum_ + static_cast<double>(irqs_due - s) * pit_period);
                }
                break;
            }
            if (elapsed_us > soft_budget_us) {
                if (s >= irqs_due) break;
                over_soft_budget = true;
            }
        }
        // Update 70.08 Hz CRT vertical retrace phase per sub-slice
        vga_time_accum_ += slice_dt;
        while (vga_time_accum_ >= kVgaPeriod) vga_time_accum_ -= kVgaPeriod;
        vga_.set_vblank(vga_time_accum_ >= (kVgaPeriod * 0.85));

        // Safely deliver queued keyboard & mouse events at clean instruction boundary when IF=1
        if ((emu_->x86.R_EFLG & 0x200u) && !in_mouse_cb_) {
            if (!pending_hw_scancodes_.empty()) {
                last_scancode_ = pending_hw_scancodes_.front();
                pending_hw_scancodes_.pop_front();
                deliver_hw_irq(0x09);
            }
            if (pending_mouse_mask_ != 0 && (emu_->x86.R_EFLG & 0x200u)) {
                uint16_t mask = pending_mouse_mask_;
                pending_mouse_mask_ = 0;
                invoke_mouse_callback(mask);
            }
        }

        uint64_t chunk = 0;
        if (!over_soft_budget && done < target_insns) {
            chunk = std::min(target_insns - done, slice_insns);
        } else if (s < irqs_due) {
            // Still have PIT IRQ ticks due (e.g. Direct DAC voice sample): run minimal slice for ISR
            chunk = 120;
        } else {
            break;
        }
        if (chunk == 0) break;

        emu_->max_instr = emu_->x86.R_TSC + chunk;
        uint64_t before = emu_->x86.R_TSC;
        x86emu_run(emu_, X86EMU_RUN_MAX_INSTR | X86EMU_RUN_NO_CODE);
        uint64_t ran = emu_->x86.R_TSC - before;
        done += ran;
        total_insns_ += ran;

        if (sb_irq_pending_ && total_insns_ >= sb_irq_due_insn_ && (emu_->x86.R_EFLG & 0x200u)) {
            sb_irq_pending_ = false;
            uint16_t seg0f = mem_read16(0x0F * 4 + 2);
            if (seg0f != kHleStubSeg && seg0f != 0) deliver_hw_irq(0x0F);
            else deliver_hw_irq(0x0D);
        }

        if (s < irqs_due && (emu_->x86.R_EFLG & 0x200u)) {
            uint16_t seg08 = mem_read16(0x08 * 4 + 2);
            if (seg08 != kHleStubSeg && seg08 != 0) deliver_hw_irq(0x08);
            else deliver_hw_irq(0x1C);
        }
    }
    return done;
}

void DosMachine::on_frame_tick() {
    vga_.on_vblank_tick();
    poll_ipx_udp();
    bda_ticks_++;
    mem_write16(0x046C, static_cast<uint16_t>(bda_ticks_ & 0xFFFF));
    mem_write16(0x046E, static_cast<uint16_t>(bda_ticks_ >> 16));
}

void DosMachine::push_key(uint8_t scancode, uint8_t ascii, bool pressed) {
    uint8_t hw = pressed ? scancode : static_cast<uint8_t>(scancode | 0x80);
    last_scancode_ = hw;
    pending_hw_scancodes_.push_back(hw);
    if (pressed) {
        key_queue_.push_back({scancode, ascii});
    }
}

void DosMachine::invoke_mouse_callback(uint16_t event_mask) {
    if (!emu_ || in_mouse_cb_ || mouse_cb_seg_ == 0 || (event_mask & mouse_cb_mask_) == 0) return;

    in_mouse_cb_ = true;

    // Place INT FFh trampoline at F000:03F0
    constexpr uint16_t kTrampOff = 0x03F0;
    mem_write8((static_cast<uint32_t>(kHleStubSeg) << 4) + kTrampOff + 0, 0xCD);
    mem_write8((static_cast<uint32_t>(kHleStubSeg) << 4) + kTrampOff + 1, 0xFF);

    // Save full CPU state & max_instr
    auto saved_x86 = emu_->x86;
    uint64_t saved_max_instr = emu_->max_instr;

    // Push far return address (F000:03F0) onto guest stack
    uint16_t sp = saved_x86.R_SP;
    uint32_t ss_base = static_cast<uint32_t>(saved_x86.R_SS) << 4;
    sp -= 2; mem_write16(ss_base + sp, kHleStubSeg);
    sp -= 2; mem_write16(ss_base + sp, kTrampOff);
    emu_->x86.R_SP = sp;

    // Set MS Mouse driver callback registers
    emu_->x86.R_AX = event_mask;
    emu_->x86.R_BX = mouse_buttons_;
    emu_->x86.R_CX = static_cast<uint16_t>(std::clamp(mouse_x_ * 2, mouse_min_x_, mouse_max_x_));
    emu_->x86.R_DX = static_cast<uint16_t>(std::clamp(mouse_y_, mouse_min_y_, mouse_max_y_));
    emu_->x86.R_SI = 0;
    emu_->x86.R_DI = 0;
    // Clear IF inside callback so nested IRQs cannot re-enter during mouse handler
    emu_->x86.R_EFLG &= ~0x200u;
    x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, mouse_cb_seg_);
    emu_->x86.R_IP = mouse_cb_off_;

    emu_->max_instr = emu_->x86.R_TSC + 50000;
    x86emu_run(emu_, X86EMU_RUN_MAX_INSTR | X86EMU_RUN_NO_CODE);

    // Restore CPU state
    uint64_t new_tsc = emu_->x86.R_TSC;
    emu_->x86 = saved_x86;
    emu_->x86.R_TSC = new_tsc;
    emu_->max_instr = saved_max_instr;
    in_mouse_cb_ = false;
}

void DosMachine::set_mouse_state(int x_320, int y_200, uint8_t buttons_mask) {
    int nx = std::clamp(x_320, 0, 319);
    int ny = std::clamp(y_200, 0, 199);
    uint16_t ev = 0;
    if (nx != mouse_x_ || ny != mouse_y_) ev |= 0x01;
    if ((buttons_mask & 1) && !(mouse_buttons_ & 1)) ev |= 0x02;
    if (!(buttons_mask & 1) && (mouse_buttons_ & 1)) ev |= 0x04;
    if ((buttons_mask & 2) && !(mouse_buttons_ & 2)) ev |= 0x08;
    if (!(buttons_mask & 2) && (mouse_buttons_ & 2)) ev |= 0x10;
    mouse_x_ = nx;
    mouse_y_ = ny;
    mouse_buttons_ = buttons_mask;
    pending_mouse_mask_ |= ev;
}

void DosMachine::render_vga_rgba_400(uint32_t* out_rgba) const {
    vga_.render_rgba_400(mem_.data(), out_rgba);
}

CpuSnapshot DosMachine::snapshot() const {
    CpuSnapshot s;
    if (emu_) {
        s.cs = emu_->x86.R_CS;
        s.ip = emu_->x86.R_IP;
        s.ss = emu_->x86.R_SS;
        s.sp = emu_->x86.R_SP;
        s.ds = emu_->x86.R_DS;
        s.es = emu_->x86.R_ES;
        s.eax = emu_->x86.R_EAX;
        s.ebx = emu_->x86.R_EBX;
        s.ecx = emu_->x86.R_ECX;
        s.edx = emu_->x86.R_EDX;
        s.esi = emu_->x86.R_ESI;
        s.edi = emu_->x86.R_EDI;
        s.ebp = emu_->x86.R_EBP;
        s.eflags = emu_->x86.R_EFLG;
    }
    s.total_insns = total_insns_;
    s.exited = exited_;
    s.exit_code = exit_code_;
    s.ems_pages_used = ems_pages_used_;
    s.ems_pages_total = EMS_TOTAL_PAGES;
    return s;
}

uint8_t DosMachine::active_dos_joystick_mode() const {
    if (joy_struct_base_ != 0 && joy_struct_base_ + 0x72 < MEM_SIZE) {
        return mem_[joy_struct_base_ + 0x71];
    }
    return 0;
}

bool DosMachine::apply_auto_joystick_calibration(bool write_def_file, int force_mode) {
    uint8_t mode = (force_mode >= 0) ? static_cast<uint8_t>(force_mode) : hotas_.recommended_gameport_mode();

    // Locate Falcon 3.0's in-RAM joystick driver segment base across FALCONCD/HORNETCD/MIGCD:
    // Unique signature "KENR\xCC\x00\xCC\x00" preceded by axis-mask table {01 03 07 0B 0F} at base + 0x6C!
    auto find_joy_base = [&]() -> uint32_t {
        if (joy_struct_base_ != 0 && joy_struct_base_ + 0x7C < MEM_SIZE &&
            mem_[joy_struct_base_ + 0x6C] == 0x01 && mem_[joy_struct_base_ + 0x6D] == 0x03 &&
            mem_[joy_struct_base_ + 0x6E] == 0x07 && mem_[joy_struct_base_ + 0x6F] == 0x0B &&
            mem_[joy_struct_base_ + 0x70] == 0x0F) {
            return joy_struct_base_;
        }
        static constexpr uint8_t kKenr[8] = {'K', 'E', 'N', 'R', 0xCC, 0x00, 0xCC, 0x00};
        static constexpr uint8_t kTbl[5]  = {0x01, 0x03, 0x07, 0x0B, 0x0F};
        for (uint32_t addr = 0x1000; addr + 8 < 0x9F000; ++addr) {
            if (std::memcmp(&mem_[addr], kKenr, 8) == 0) {
                uint32_t search_lo = (addr > 0x500) ? (addr - 0x500) : 0x1000;
                for (uint32_t t = addr - 5; t >= search_lo; --t) {
                    if (std::memcmp(&mem_[t], kTbl, 5) == 0 && t >= 0x6C) {
                        joy_struct_base_ = t - 0x6C;
                        return joy_struct_base_;
                    }
                    if (t == search_lo) break;
                }
            }
        }
        return 0;
    };

    auto joy_def_filename = [&]() -> const char* {
        if (profile_.title == FalconTitle::Hornet) return "joystich.def";
        if (profile_.title == FalconTitle::MiG29)  return "joysticm.def";
        return "joystick.def";
    };

    uint32_t base = find_joy_base();
    if (mode == 0) {
        // All axes cleared or Keyboard Only requested -> disable joystick driver cleanly across Falcon/Hornet/MiG
        if (base != 0) {
            mem_[base + 0x66] = 0;
            mem_[base + 0x71] = 0;
            mem_[base + 0x72] = 0;
            mem_[base + 0x74] = 0;
            mem_[base + 0x75] = 0; // Polling active flag (critical for Hornet & MiG-29!)
        }
        if (write_def_file && !game_dir_.empty()) {
            std::error_code ec;
            fs::remove(fs::path(game_dir_) / joy_def_filename(), ec);
        }
        return base != 0;
    }

    // Build exact 100-byte Falcon 3.0 joystick.def matching our deterministic virtual gameport 0x201:
    //   Each axis (stride 22 B at offsets 0, 22, 44, 66 -> DS:0x0004, 0x001A, 0x0030, 0x0046):
    //     +0x04: center = 127, +0x06: pos_span = +127, +0x08: neg_span = -126 (0xFF82)
    //     +0x0A/+0x0C/+0x0E: running center/raw = 127, +0x12: +127, +0x14: -126
    //   Byte 98 (DS:0x0066 -> copied to DS:0x0071): active mode (1=X+Y, 2=+Yaw, 3=+Thr, 4=All 4)
    std::array<uint8_t, 100> jdef{};
    auto wr16 = [&](size_t off, uint16_t v) {
        jdef[off + 0] = static_cast<uint8_t>(v & 0xFFu);
        jdef[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    };
    for (size_t ax = 0; ax < 4; ++ax) {
        size_t o = ax * 22;
        wr16(o + 0x04, 127);
        wr16(o + 0x06, 127);
        wr16(o + 0x08, static_cast<uint16_t>(static_cast<int16_t>(-126)));
        wr16(o + 0x0A, 127);
        wr16(o + 0x0C, 127);
        wr16(o + 0x0E, 127);
        wr16(o + 0x10, 0);
        wr16(o + 0x12, 127);
        wr16(o + 0x14, static_cast<uint16_t>(static_cast<int16_t>(-126)));
    }
    jdef[98] = mode;
    jdef[99] = 0;

    if (base != 0) {
        std::memcpy(&mem_[base + 0x04], jdef.data(), jdef.size());
        mem_[base + 0x71] = mode; // Activate joystick driver immediately in RAM (1=XY, 2=+Yaw, 3=+Thr, 4=All 4)
        mem_[base + 0x72] = mode; // Saved active joystick mode
        mem_[base + 0x74] = 0xFF; // Driver initialized flag (REQUIRED by 0x15022 analog throttle getter!)
        mem_[base + 0x75] = 0xFF; // Gameport polling active flag
        mem_[base + 0x78] = 0;    // Analog gameport coolie hat off (handled directly by HOTAS mapper)
        mem_[base + 0x7A] = 0;    // Standard gameport buttons
    }

    if (write_def_file && !game_dir_.empty()) {
        fs::path def_path = fs::path(game_dir_) / joy_def_filename();
        std::ofstream out(def_path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(jdef.data()), static_cast<std::streamsize>(jdef.size()));
        }
    }

    log(std::string("Auto-calibrated virtual gameport joystick (mode=") + std::to_string(mode) +
        (base ? ", RAM patched" : "") + (write_def_file ? (std::string(", wrote ") + joy_def_filename() + ")") : ")"));
    return true;
}

} // namespace falcon
