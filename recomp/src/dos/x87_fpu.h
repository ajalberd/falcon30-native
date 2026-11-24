#pragma once

#include <array>
#include <cstdint>

struct x86emu_s;
typedef struct x86emu_s x86emu_t;

namespace falcon {

// Complete IEEE-754 80387 x87 Math Coprocessor emulator for real-mode MS-DOS execution.
// Unlocks Falcon 3.0 / Hornet / MiG-29's genuine "HI-FIDELITY" (Max) Flight Model!
class X87Fpu {
public:
    X87Fpu() { reset(); }

    void reset();

    // Execute an x87 ESC instruction (op1 in 0xD8..0xDF) intercepted via libx86emu #UD (INT 0x06).
    // Returns true if executed cleanly (advancing emu->x86.R_IP past ModRM/disp).
    bool execute(x86emu_t* emu, uint8_t* mem, uint32_t mem_size, uint8_t op1);

    uint64_t ops_executed() const { return ops_executed_; }
    uint64_t unknown_ops() const { return unknown_ops_; }

private:
    std::array<double, 8> regs_{};
    uint16_t cw_ = 0x037F; // Control Word
    uint16_t sw_ = 0x0000; // Status Word (TOP in bits 11..13)
    uint16_t tw_ = 0xFFFF; // Tag Word (2 bits/reg: 3=empty)
    int top_ = 0;

    uint64_t ops_executed_ = 0;
    uint64_t unknown_ops_ = 0;

    double& st(int i) { return regs_[(top_ + i) & 7]; }
    double  st(int i) const { return regs_[(top_ + i) & 7]; }
    void set_tag(int phys_reg, double v);
    void set_tag_empty(int phys_reg);
    void push(double v);
    double pop();
    uint16_t read_sw() const;
    void set_cc(bool c0, bool c1, bool c2, bool c3);
    void compare(double a, double b);
    double round_with_cw(double v) const;
    int64_t round_to_i64(double v) const;

    static double read_f32(const uint8_t* mem, uint32_t addr);
    static double read_f64(const uint8_t* mem, uint32_t addr);
    static double read_f80(const uint8_t* mem, uint32_t addr);
    static double read_bcd80(const uint8_t* mem, uint32_t addr);
    static void write_f32(uint8_t* mem, uint32_t addr, double v);
    static void write_f64(uint8_t* mem, uint32_t addr, double v);
    static void write_f80(uint8_t* mem, uint32_t addr, double v);
    static void write_bcd80(uint8_t* mem, uint32_t addr, double v);
};

} // namespace falcon
