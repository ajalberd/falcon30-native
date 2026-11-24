#include "dos/x87_fpu.h"

#include <x86emu.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace falcon {

namespace {

inline uint16_t rd16(const uint8_t* mem, uint32_t a) {
    return static_cast<uint16_t>(mem[a] | (static_cast<uint16_t>(mem[a + 1]) << 8));
}

inline int16_t rds16(const uint8_t* mem, uint32_t a) {
    return static_cast<int16_t>(rd16(mem, a));
}

inline uint32_t rd32(const uint8_t* mem, uint32_t a) {
    return static_cast<uint32_t>(mem[a]) |
           (static_cast<uint32_t>(mem[a + 1]) << 8) |
           (static_cast<uint32_t>(mem[a + 2]) << 16) |
           (static_cast<uint32_t>(mem[a + 3]) << 24);
}

inline int32_t rds32(const uint8_t* mem, uint32_t a) {
    return static_cast<int32_t>(rd32(mem, a));
}

inline int64_t rds64(const uint8_t* mem, uint32_t a) {
    uint64_t lo = rd32(mem, a);
    uint64_t hi = rd32(mem, a + 4);
    return static_cast<int64_t>(lo | (hi << 32));
}

inline void wr16(uint8_t* mem, uint32_t a, uint16_t v) {
    mem[a + 0] = static_cast<uint8_t>(v & 0xFF);
    mem[a + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void wr32(uint8_t* mem, uint32_t a, uint32_t v) {
    mem[a + 0] = static_cast<uint8_t>(v & 0xFF);
    mem[a + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    mem[a + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    mem[a + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline void wr64(uint8_t* mem, uint32_t a, uint64_t v) {
    wr32(mem, a, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    wr32(mem, a + 4, static_cast<uint32_t>(v >> 32));
}

// Decode 16-bit real-mode ModRM effective address and return total extra bytes (ModRM + disp)
uint32_t decode_ea16(x86emu_t* emu, const uint8_t* mem, uint32_t pc, uint8_t modrm, uint16_t& extra_out) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm  = modrm & 7;
    extra_out = 1; // ModRM byte itself

    uint16_t off = 0;
    bool use_ss = false;
    if (mod == 0 && rm == 6) {
        off = rd16(mem, pc + 1);
        extra_out += 2;
    } else {
        switch (rm) {
            case 0: off = static_cast<uint16_t>(emu->x86.R_BX + emu->x86.R_SI); break;
            case 1: off = static_cast<uint16_t>(emu->x86.R_BX + emu->x86.R_DI); break;
            case 2: off = static_cast<uint16_t>(emu->x86.R_BP + emu->x86.R_SI); use_ss = true; break;
            case 3: off = static_cast<uint16_t>(emu->x86.R_BP + emu->x86.R_DI); use_ss = true; break;
            case 4: off = emu->x86.R_SI; break;
            case 5: off = emu->x86.R_DI; break;
            case 6: off = emu->x86.R_BP; use_ss = true; break;
            case 7: off = emu->x86.R_BX; break;
        }
        if (mod == 1) {
            int8_t d8 = static_cast<int8_t>(mem[pc + 1]);
            off = static_cast<uint16_t>(off + static_cast<int16_t>(d8));
            extra_out += 1;
        } else if (mod == 2) {
            int16_t d16 = rds16(mem, pc + 1);
            off = static_cast<uint16_t>(off + d16);
            extra_out += 2;
        }
    }

    uint32_t seg_base = 0;
    if (emu->x86.default_seg != nullptr) {
        seg_base = emu->x86.default_seg->base;
    } else {
        seg_base = static_cast<uint32_t>(use_ss ? emu->x86.R_SS : emu->x86.R_DS) << 4;
    }
    return (seg_base + off) & 0xFFFFFu;
}

} // namespace

void X87Fpu::reset() {
    regs_.fill(0.0);
    cw_ = 0x037F;
    sw_ = 0x0000;
    tw_ = 0xFFFF;
    top_ = 0;
    ops_executed_ = 0;
    unknown_ops_ = 0;
}

void X87Fpu::set_tag(int phys_reg, double v) {
    uint16_t tag = 0; // Valid
    if (std::isnan(v) || std::isinf(v)) tag = 2; // Special
    else if (v == 0.0) tag = 1;                  // Zero
    int shift = (phys_reg & 7) * 2;
    tw_ = static_cast<uint16_t>((tw_ & ~(3u << shift)) | (tag << shift));
}

void X87Fpu::set_tag_empty(int phys_reg) {
    int shift = (phys_reg & 7) * 2;
    tw_ = static_cast<uint16_t>(tw_ | (3u << shift));
}

void X87Fpu::push(double v) {
    top_ = (top_ - 1) & 7;
    regs_[top_] = v;
    set_tag(top_, v);
}

double X87Fpu::pop() {
    double v = regs_[top_];
    set_tag_empty(top_);
    top_ = (top_ + 1) & 7;
    return v;
}

uint16_t X87Fpu::read_sw() const {
    return static_cast<uint16_t>((sw_ & ~0x3800u) | (static_cast<uint16_t>(top_ & 7) << 11));
}

void X87Fpu::set_cc(bool c0, bool c1, bool c2, bool c3) {
    sw_ &= ~0x4700u;
    if (c0) sw_ |= 0x0100u;
    if (c1) sw_ |= 0x0200u;
    if (c2) sw_ |= 0x0400u;
    if (c3) sw_ |= 0x4000u;
}

void X87Fpu::compare(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        set_cc(true, false, true, true);
    } else if (a > b) {
        set_cc(false, false, false, false);
    } else if (a < b) {
        set_cc(true, false, false, false);
    } else {
        set_cc(false, false, false, true);
    }
}

double X87Fpu::round_with_cw(double v) const {
    if (!std::isfinite(v)) return v;
    int rc = (cw_ >> 10) & 3;
    switch (rc) {
        case 0: return std::nearbyint(v); // Nearest-even (default)
        case 1: return std::floor(v);     // Down (-inf)
        case 2: return std::ceil(v);      // Up (+inf)
        case 3: return std::trunc(v);     // Toward zero (C cast!)
    }
    return std::nearbyint(v);
}

int64_t X87Fpu::round_to_i64(double v) const {
    if (!std::isfinite(v)) return std::numeric_limits<int64_t>::min();
    double r = round_with_cw(v);
    if (r >= 9223372036854775808.0 || r < -9223372036854775808.0) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(r);
}

double X87Fpu::read_f32(const uint8_t* mem, uint32_t addr) {
    uint32_t u = rd32(mem, addr);
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return static_cast<double>(f);
}

double X87Fpu::read_f64(const uint8_t* mem, uint32_t addr) {
    uint64_t lo = rd32(mem, addr);
    uint64_t hi = rd32(mem, addr + 4);
    uint64_t u = lo | (hi << 32);
    double d = 0.0;
    std::memcpy(&d, &u, 8);
    return d;
}

double X87Fpu::read_f80(const uint8_t* mem, uint32_t addr) {
    uint64_t mant = static_cast<uint64_t>(rd32(mem, addr)) |
                    (static_cast<uint64_t>(rd32(mem, addr + 4)) << 32);
    uint16_t exp_s = rd16(mem, addr + 8);
    bool sign = (exp_s & 0x8000u) != 0;
    int exp = static_cast<int>(exp_s & 0x7FFFu);

    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) {
        if ((mant & 0x7FFFFFFFFFFFFFFFULL) == 0) {
            return sign ? -std::numeric_limits<double>::infinity()
                        :  std::numeric_limits<double>::infinity();
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    double val = std::ldexp(static_cast<double>(mant), (exp == 0 ? 1 : exp) - 16383 - 63);
    return sign ? -val : val;
}

double X87Fpu::read_bcd80(const uint8_t* mem, uint32_t addr) {
    int64_t acc = 0;
    int64_t mul = 1;
    for (int i = 0; i < 9; ++i) {
        uint8_t b = mem[addr + i];
        acc += static_cast<int64_t>(b & 0x0Fu) * mul;
        mul *= 10;
        acc += static_cast<int64_t>((b >> 4) & 0x0Fu) * mul;
        mul *= 10;
    }
    if (mem[addr + 9] & 0x80u) acc = -acc;
    return static_cast<double>(acc);
}

void X87Fpu::write_f32(uint8_t* mem, uint32_t addr, double v) {
    float f = static_cast<float>(v);
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    wr32(mem, addr, u);
}

void X87Fpu::write_f64(uint8_t* mem, uint32_t addr, double v) {
    uint64_t u = 0;
    std::memcpy(&u, &v, 8);
    wr64(mem, addr, u);
}

void X87Fpu::write_f80(uint8_t* mem, uint32_t addr, double v) {
    uint16_t exp_s = std::signbit(v) ? 0x8000u : 0x0000u;
    double av = std::fabs(v);
    uint64_t mant = 0;
    if (std::isnan(av)) {
        exp_s |= 0x7FFFu;
        mant = 0xC000000000000000ULL;
    } else if (std::isinf(av)) {
        exp_s |= 0x7FFFu;
        mant = 0x8000000000000000ULL;
    } else if (av == 0.0) {
        mant = 0;
    } else {
        int e = 0;
        double frac = std::frexp(av, &e); // av = frac * 2^e, frac in [0.5, 1.0)
        int exp80 = (e - 1) + 16383;
        if (exp80 <= 0) {
            mant = 0;
        } else if (exp80 >= 0x7FFF) {
            exp_s |= 0x7FFFu;
            mant = 0x8000000000000000ULL;
        } else {
            exp_s |= static_cast<uint16_t>(exp80);
            mant = static_cast<uint64_t>(std::ldexp(frac, 64));
        }
    }
    wr64(mem, addr, mant);
    wr16(mem, addr + 8, exp_s);
}

void X87Fpu::write_bcd80(uint8_t* mem, uint32_t addr, double v) {
    int64_t n = static_cast<int64_t>(std::llround(v));
    bool neg = n < 0;
    uint64_t u = neg ? static_cast<uint64_t>(-n) : static_cast<uint64_t>(n);
    for (int i = 0; i < 9; ++i) {
        uint8_t lo = static_cast<uint8_t>(u % 10); u /= 10;
        uint8_t hi = static_cast<uint8_t>(u % 10); u /= 10;
        mem[addr + i] = static_cast<uint8_t>(lo | (hi << 4));
    }
    mem[addr + 9] = neg ? 0x80u : 0x00u;
}

bool X87Fpu::execute(x86emu_t* emu, uint8_t* mem, uint32_t mem_size, uint8_t op1) {
    if (op1 < 0xD8 || op1 > 0xDF) return false;

    uint32_t pc = ((static_cast<uint32_t>(emu->x86.R_CS) << 4) + emu->x86.R_IP) & 0xFFFFFu;
    if (pc + 4 >= mem_size) return false;

    uint8_t modrm = mem[pc];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg = (modrm >> 3) & 7;
    uint8_t rm  = modrm & 7;

    uint16_t extra = 1;
    uint32_t ea = 0;
    if (mod != 3) {
        ea = decode_ea16(emu, mem, pc, modrm, extra);
    }
    emu->x86.R_IP = static_cast<uint16_t>(emu->x86.R_IP + extra);
    ops_executed_++;

    auto arith_st0 = [&](int op, double src) {
        double& d = st(0);
        switch (op) {
            case 0: d = d + src; set_tag(top_, d); break;
            case 1: d = d * src; set_tag(top_, d); break;
            case 2: compare(d, src); break;
            case 3: compare(d, src); pop(); break;
            case 4: d = d - src; set_tag(top_, d); break;
            case 5: d = src - d; set_tag(top_, d); break;
            case 6: d = d / src; set_tag(top_, d); break;
            case 7: d = src / d; set_tag(top_, d); break;
        }
    };

    switch (op1) {
        case 0xD8: {
            double src = (mod != 3) ? read_f32(mem, ea) : st(rm);
            arith_st0(reg, src);
            return true;
        }
        case 0xD9: {
            if (mod != 3) {
                switch (reg) {
                    case 0: push(read_f32(mem, ea)); break;                     // FLD m32fp
                    case 2: write_f32(mem, ea, st(0)); break;                   // FST m32fp
                    case 3: write_f32(mem, ea, pop()); break;                   // FSTP m32fp
                    case 4: {                                                   // FLDENV m14byte
                        cw_ = rd16(mem, ea + 0);
                        sw_ = rd16(mem, ea + 2);
                        top_ = (sw_ >> 11) & 7;
                        tw_ = rd16(mem, ea + 4);
                        break;
                    }
                    case 5: cw_ = rd16(mem, ea); break;                         // FLDCW m16
                    case 6: {                                                   // FNSTENV m14byte
                        wr16(mem, ea + 0, cw_);
                        wr16(mem, ea + 2, read_sw());
                        wr16(mem, ea + 4, tw_);
                        wr16(mem, ea + 6, 0);
                        wr16(mem, ea + 8, 0);
                        wr16(mem, ea + 10, 0);
                        wr16(mem, ea + 12, 0);
                        break;
                    }
                    case 7: wr16(mem, ea, cw_); break;                          // FNSTCW m16
                    default: unknown_ops_++; break;
                }
            } else {
                switch (modrm) {
                    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
                    case 0xC4: case 0xC5: case 0xC6: case 0xC7:                 // FLD ST(i)
                        push(st(rm));
                        break;
                    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {               // FXCH ST(i)
                        double tmp = st(0);
                        st(0) = st(rm);
                        st(rm) = tmp;
                        set_tag(top_, st(0));
                        set_tag((top_ + rm) & 7, st(rm));
                        break;
                    }
                    case 0xD0: break;                                           // FNOP
                    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
                    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {               // FSTP ST(i) alias
                        double v = st(0);
                        st(rm) = v;
                        set_tag((top_ + rm) & 7, v);
                        pop();
                        break;
                    }
                    case 0xE0: st(0) = -st(0); break;                           // FCHS
                    case 0xE1: st(0) = std::fabs(st(0)); break;                 // FABS
                    case 0xE4: compare(st(0), 0.0); break;                      // FTST
                    case 0xE5: {                                                // FXAM
                        int tag = (tw_ >> ((top_ & 7) * 2)) & 3;
                        double v = st(0);
                        bool neg = std::signbit(v);
                        if (tag == 3) set_cc(true, neg, false, true);           // Empty
                        else if (std::isnan(v)) set_cc(true, neg, false, false);// NaN
                        else if (std::isinf(v)) set_cc(true, neg, true, false); // Inf
                        else if (v == 0.0) set_cc(false, neg, false, true);     // Zero
                        else set_cc(false, neg, true, false);                   // Normal
                        break;
                    }
                    case 0xE8: push(1.0); break;                                // FLD1
                    case 0xE9: push(3.32192809488736234787); break;             // FLDL2T (log2(10))
                    case 0xEA: push(1.44269504088896340736); break;             // FLDL2E (log2(e))
                    case 0xEB: push(3.14159265358979323846); break;             // FLDPI
                    case 0xEC: push(0.30102999566398119521); break;             // FLDLG2 (log10(2))
                    case 0xED: push(0.69314718055994530942); break;             // FLDLN2 (ln(2))
                    case 0xEE: push(0.0); break;                                // FLDZ
                    case 0xF0: st(0) = std::exp2(st(0)) - 1.0; set_tag(top_, st(0)); break; // F2XM1
                    case 0xF1: {                                                // FYL2X
                        double x = pop();
                        st(0) = st(0) * std::log2(x);
                        set_tag(top_, st(0));
                        break;
                    }
                    case 0xF2: {                                                // FPTAN
                        st(0) = std::tan(st(0));
                        set_tag(top_, st(0));
                        push(1.0);
                        sw_ &= ~0x0400u; // C2 = 0 (in range)
                        break;
                    }
                    case 0xF3: {                                                // FPATAN
                        double x = pop();
                        st(0) = std::atan2(st(0), x);
                        set_tag(top_, st(0));
                        break;
                    }
                    case 0xF4: {                                                // FXTRACT
                        double v = st(0);
                        int e = 0;
                        double sig = std::frexp(v, &e);
                        if (v == 0.0) {
                            st(0) = -std::numeric_limits<double>::infinity();
                            push(v);
                        } else {
                            st(0) = static_cast<double>(e - 1);
                            push(sig * 2.0);
                        }
                        break;
                    }
                    case 0xF5: {                                                // FPREM1
                        double a = st(0), b = st(1);
                        double q = std::nearbyint(a / b);
                        st(0) = std::remainder(a, b);
                        set_tag(top_, st(0));
                        int64_t qi = static_cast<int64_t>(q);
                        set_cc((qi & 4) != 0, (qi & 1) != 0, false, (qi & 2) != 0);
                        break;
                    }
                    case 0xF6: top_ = (top_ - 1) & 7; break;                    // FDECSTP
                    case 0xF7: top_ = (top_ + 1) & 7; break;                    // FINCSTP
                    case 0xF8: {                                                // FPREM
                        double a = st(0), b = st(1);
                        double q = std::trunc(a / b);
                        st(0) = std::fmod(a, b);
                        set_tag(top_, st(0));
                        int64_t qi = static_cast<int64_t>(q);
                        set_cc((qi & 4) != 0, (qi & 1) != 0, false, (qi & 2) != 0);
                        break;
                    }
                    case 0xF9: {                                                // FYL2XP1
                        double x = pop();
                        st(0) = st(0) * (std::log1p(x) / 0.69314718055994530942);
                        set_tag(top_, st(0));
                        break;
                    }
                    case 0xFA: st(0) = std::sqrt(st(0)); set_tag(top_, st(0)); break; // FSQRT
                    case 0xFB: {                                                // FSINCOS
                        double a = st(0);
                        st(0) = std::sin(a);
                        set_tag(top_, st(0));
                        push(std::cos(a));
                        sw_ &= ~0x0400u;
                        break;
                    }
                    case 0xFC: st(0) = round_with_cw(st(0)); set_tag(top_, st(0)); break; // FRNDINT
                    case 0xFD: st(0) = std::ldexp(st(0), static_cast<int>(std::trunc(st(1)))); set_tag(top_, st(0)); break; // FSCALE
                    case 0xFE: st(0) = std::sin(st(0)); set_tag(top_, st(0)); sw_ &= ~0x0400u; break; // FSIN
                    case 0xFF: st(0) = std::cos(st(0)); set_tag(top_, st(0)); sw_ &= ~0x0400u; break; // FCOS
                    default: unknown_ops_++; break;
                }
            }
            return true;
        }
        case 0xDA: {
            if (mod != 3) {
                arith_st0(reg, static_cast<double>(rds32(mem, ea)));
            } else if (modrm == 0xE9) {                                         // FUCOMPP
                compare(st(0), st(1));
                pop();
                pop();
            } else {
                unknown_ops_++;
            }
            return true;
        }
        case 0xDB: {
            if (mod != 3) {
                switch (reg) {
                    case 0: push(static_cast<double>(rds32(mem, ea))); break;   // FILD m32int
                    case 2: wr32(mem, ea, static_cast<uint32_t>(static_cast<int32_t>(round_to_i64(st(0))))); break; // FIST m32int
                    case 3: wr32(mem, ea, static_cast<uint32_t>(static_cast<int32_t>(round_to_i64(pop())))); break; // FISTP m32int
                    case 5: push(read_f80(mem, ea)); break;                     // FLD m80fp
                    case 7: write_f80(mem, ea, pop()); break;                   // FSTP m80fp
                    default: unknown_ops_++; break;
                }
            } else {
                switch (modrm) {
                    case 0xE0: case 0xE1: case 0xE4: break;                     // FNENI/FNDISI/FNSETPM (NOP)
                    case 0xE2: sw_ &= ~0x80FFu; break;                          // FNCLEX
                    case 0xE3:                                                  // FNINIT
                        cw_ = 0x037F;
                        sw_ = 0x0000;
                        tw_ = 0xFFFF;
                        top_ = 0;
                        break;
                    default: unknown_ops_++; break;
                }
            }
            return true;
        }
        case 0xDC: {
            if (mod != 3) {
                arith_st0(reg, read_f64(mem, ea));
            } else {
                // Note x87 DC register encoding: reg=4 is FSUBR, reg=5 is FSUB, reg=6 is FDIVR, reg=7 is FDIV!
                double s0 = st(0);
                double& di = st(rm);
                switch (reg) {
                    case 0: di = di + s0; set_tag((top_ + rm) & 7, di); break;
                    case 1: di = di * s0; set_tag((top_ + rm) & 7, di); break;
                    case 2: compare(s0, di); break;
                    case 3: compare(s0, di); pop(); break;
                    case 4: di = s0 - di; set_tag((top_ + rm) & 7, di); break;  // FSUBR ST(i), ST(0)
                    case 5: di = di - s0; set_tag((top_ + rm) & 7, di); break;  // FSUB  ST(i), ST(0)
                    case 6: di = s0 / di; set_tag((top_ + rm) & 7, di); break;  // FDIVR ST(i), ST(0)
                    case 7: di = di / s0; set_tag((top_ + rm) & 7, di); break;  // FDIV  ST(i), ST(0)
                }
            }
            return true;
        }
        case 0xDD: {
            if (mod != 3) {
                switch (reg) {
                    case 0: push(read_f64(mem, ea)); break;                     // FLD m64fp
                    case 2: write_f64(mem, ea, st(0)); break;                   // FST m64fp
                    case 3: write_f64(mem, ea, pop()); break;                   // FSTP m64fp
                    case 4: {                                                   // FRSTOR m94byte
                        cw_ = rd16(mem, ea + 0);
                        sw_ = rd16(mem, ea + 2);
                        top_ = (sw_ >> 11) & 7;
                        tw_ = rd16(mem, ea + 4);
                        for (int i = 0; i < 8; ++i) {
                            st(i) = read_f80(mem, ea + 14 + static_cast<uint32_t>(i) * 10u);
                        }
                        break;
                    }
                    case 6: {                                                   // FNSAVE m94byte
                        wr16(mem, ea + 0, cw_);
                        wr16(mem, ea + 2, read_sw());
                        wr16(mem, ea + 4, tw_);
                        wr16(mem, ea + 6, 0);
                        wr16(mem, ea + 8, 0);
                        wr16(mem, ea + 10, 0);
                        wr16(mem, ea + 12, 0);
                        for (int i = 0; i < 8; ++i) {
                            write_f80(mem, ea + 14 + static_cast<uint32_t>(i) * 10u, st(i));
                        }
                        cw_ = 0x037F; sw_ = 0; tw_ = 0xFFFF; top_ = 0;
                        break;
                    }
                    case 7: wr16(mem, ea, read_sw()); break;                    // FNSTSW m16
                    default: unknown_ops_++; break;
                }
            } else {
                switch (reg) {
                    case 0: set_tag_empty((top_ + rm) & 7); break;              // FFREE ST(i)
                    case 2: st(rm) = st(0); set_tag((top_ + rm) & 7, st(rm)); break; // FST ST(i)
                    case 3: {                                                   // FSTP ST(i)
                        double v = st(0);
                        st(rm) = v;
                        set_tag((top_ + rm) & 7, v);
                        pop();
                        break;
                    }
                    case 4: compare(st(0), st(rm)); break;                      // FUCOM ST(i)
                    case 5: compare(st(0), st(rm)); pop(); break;               // FUCOMP ST(i)
                    default: unknown_ops_++; break;
                }
            }
            return true;
        }
        case 0xDE: {
            if (mod != 3) {
                arith_st0(reg, static_cast<double>(rds16(mem, ea)));
            } else {
                if (modrm == 0xD9) {                                            // FCOMPP
                    compare(st(0), st(1));
                    pop();
                    pop();
                } else {
                    // DE register encoding (with pop): reg=4 is FSUBRP, reg=5 is FSUBP, reg=6 is FDIVRP, reg=7 is FDIVP!
                    double s0 = st(0);
                    double& di = st(rm);
                    switch (reg) {
                        case 0: di = di + s0; set_tag((top_ + rm) & 7, di); pop(); break;
                        case 1: di = di * s0; set_tag((top_ + rm) & 7, di); pop(); break;
                        case 4: di = s0 - di; set_tag((top_ + rm) & 7, di); pop(); break; // FSUBRP
                        case 5: di = di - s0; set_tag((top_ + rm) & 7, di); pop(); break; // FSUBP
                        case 6: di = s0 / di; set_tag((top_ + rm) & 7, di); pop(); break; // FDIVRP
                        case 7: di = di / s0; set_tag((top_ + rm) & 7, di); pop(); break; // FDIVP
                        default: unknown_ops_++; break;
                    }
                }
            }
            return true;
        }
        case 0xDF: {
            if (mod != 3) {
                switch (reg) {
                    case 0: push(static_cast<double>(rds16(mem, ea))); break;   // FILD m16int
                    case 2: wr16(mem, ea, static_cast<uint16_t>(static_cast<int16_t>(round_to_i64(st(0))))); break; // FIST m16int
                    case 3: wr16(mem, ea, static_cast<uint16_t>(static_cast<int16_t>(round_to_i64(pop())))); break; // FISTP m16int
                    case 4: push(read_bcd80(mem, ea)); break;                   // FBLD m80bcd
                    case 5: push(static_cast<double>(rds64(mem, ea))); break;   // FILD m64int
                    case 6: write_bcd80(mem, ea, pop()); break;                 // FBSTP m80bcd
                    case 7: wr64(mem, ea, static_cast<uint64_t>(round_to_i64(pop()))); break; // FISTP m64int
                    default: unknown_ops_++; break;
                }
            } else if (modrm == 0xE0) {                                         // FNSTSW AX
                emu->x86.R_AX = read_sw();
            } else {
                unknown_ops_++;
            }
            return true;
        }
    }
    return false;
}

} // namespace falcon
