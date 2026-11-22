#include "render/cockpit_3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace falcon {

namespace {
constexpr float kPi = 3.14159265358979323846f;

inline uint32_t pack_rgba(int r, int g, int b, int a = 255) {
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    a = std::clamp(a, 0, 255);
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(r);
}

// Deterministic classic 1991 2x2 / hash pixel dither for authentic Spectrum HoloByte pixel-art grain
inline int pixel_dither(int x, int y, int amp) {
    static constexpr int kBayer[4][4] = {
        {-6,  2, -4,  4},
        { 6, -2,  4, -4},
        {-3,  5, -5,  3},
        { 5, -3,  3, -5}
    };
    return (kBayer[y & 3][x & 3] * amp) / 6;
}

GLuint create_nearest_rgba_tex(int w, int h, const uint32_t* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return tex;
}

// Helper: draw a simple 3D shaded box in aircraft body coordinates
void draw_box(float x0, float y0, float z0, float x1, float y1, float z1,
              float r, float g, float b) {
    glBegin(GL_QUADS);
    // Top (+Y)
    glColor3f(r * 1.05f, g * 1.05f, b * 1.05f);
    glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    // Bottom (-Y)
    glColor3f(r * 0.65f, g * 0.65f, b * 0.65f);
    glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1); glVertex3f(x1, y0, z0); glVertex3f(x0, y0, z0);
    // Left (-X)
    glColor3f(r * 0.85f, g * 0.85f, b * 0.85f);
    glVertex3f(x0, y0, z1); glVertex3f(x0, y0, z0); glVertex3f(x0, y1, z0); glVertex3f(x0, y1, z1);
    // Right (+X)
    glColor3f(r * 0.85f, g * 0.85f, b * 0.85f);
    glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z0);
    // Front (-Z)
    glColor3f(r * 0.92f, g * 0.92f, b * 0.92f);
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
    // Rear (+Z)
    glColor3f(r * 0.78f, g * 0.78f, b * 0.78f);
    glVertex3f(x1, y0, z1); glVertex3f(x0, y0, z1); glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1);
    glEnd();
}

} // namespace

Cockpit3D::~Cockpit3D() {
    for (GLuint& t : view_tex_) {
        if (t) { glDeleteTextures(1, &t); t = 0; }
    }
    if (tex_floor_)     { glDeleteTextures(1, &tex_floor_); tex_floor_ = 0; }
    if (tex_nose_)      { glDeleteTextures(1, &tex_nose_); tex_nose_ = 0; }
    if (tex_wing_)      { glDeleteTextures(1, &tex_wing_); tex_wing_ = 0; }
    if (tex_bulkhead_)  { glDeleteTextures(1, &tex_bulkhead_); tex_bulkhead_ = 0; }
    if (tex_left_aux_)  { glDeleteTextures(1, &tex_left_aux_); tex_left_aux_ = 0; }
    if (tex_right_aux_) { glDeleteTextures(1, &tex_right_aux_); tex_right_aux_ = 0; }
    if (tex_pedestal_)  { glDeleteTextures(1, &tex_pedestal_); tex_pedestal_ = 0; }
}

void Cockpit3D::set_view_indices(ViewSlot slot, const uint8_t* indices_64k) {
    int s = static_cast<int>(slot);
    if (s < 0 || s >= ViewCount || !indices_64k) return;
    std::memcpy(raw_idx_[s].data(), indices_64k, kSpriteW * kSpriteH);
    has_view_[s] = true;
    dirty_view_[s] = true;
}

bool Cockpit3D::has_any_views() const {
    for (bool b : has_view_) if (b) return true;
    return false;
}

bool Cockpit3D::has_all_views() const {
    for (bool b : has_view_) if (!b) return false;
    return true;
}

void Cockpit3D::clear_views() {
    for (int i = 0; i < ViewCount; ++i) {
        has_view_[i] = false;
        dirty_view_[i] = false;
    }
}

void Cockpit3D::sync_palette(const uint8_t* dac_768) {
    if (!dac_768) return;
    ensure_gl_resources();
    bool pal_changed = !has_dac_ || (std::memcmp(last_dac_.data(), dac_768, 768) != 0);
    if (pal_changed) {
        std::memcpy(last_dac_.data(), dac_768, 768);
        has_dac_ = true;
        for (int i = 0; i < ViewCount; ++i) {
            if (has_view_[i]) dirty_view_[i] = true;
        }
    }
    for (int i = 0; i < ViewCount; ++i) {
        if (has_view_[i] && dirty_view_[i]) {
            upload_view_texture(i, last_dac_.data());
            dirty_view_[i] = false;
        }
    }
}

void Cockpit3D::upload_view_texture(int slot, const uint8_t* dac_768) {
    if (!view_tex_[slot]) {
        glGenTextures(1, &view_tex_[slot]);
        glBindTexture(GL_TEXTURE_2D, view_tex_[slot]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    static std::array<uint32_t, kSpriteW * kSpriteH> rgba{};
    const auto& src = raw_idx_[slot];
    for (int y = 0; y < kSpriteH; ++y) {
        for (int x = 0; x < kSpriteW; ++x) {
            uint8_t idx = src[y * kSpriteW + x];
            // Strip Falcon 3.0's 1px Mode-13h left-column black border (x==0) in side/rear views
            if (x == 0 && idx == 0 && y < 130) {
                rgba[y * kSpriteW + x] = 0x00000000u;
                continue;
            }
            // In Falcon 3.0's Forward view (ViewForward), exclude dynamic green HUD strokes (idx 56)
            // when cached statically, so stale HUD numerals don't appear in Padlock/Free-Look!
            if (slot == ViewForward && y < 103 && x >= 76 && x <= 244 && idx == 56) {
                rgba[y * kSpriteW + x] = 0x00000000u;
                continue;
            }
            if (idx < 128) {
                int r = (dac_768[idx * 3 + 0] * 255) / 63;
                int g = (dac_768[idx * 3 + 1] * 255) / 63;
                int b = (dac_768[idx * 3 + 2] * 255) / 63;
                rgba[y * kSpriteW + x] = pack_rgba(r, g, b, 255);
            } else {
                rgba[y * kSpriteW + x] = 0x00000000u;
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, view_tex_[slot]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSpriteW, kSpriteH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void Cockpit3D::ensure_gl_resources() {
    if (gl_ready_) return;
    build_procedural_textures();
    gl_ready_ = true;
}

void Cockpit3D::build_procedural_textures() {
    // 1. Cockpit Floor & Rudder Pedals (64x64 crisp 1991 pixel art)
    {
        constexpr int S = 64;
        std::vector<uint32_t> px(S * S);
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                int d = pixel_dither(x, y, 7);
                int base = 44 + d;
                // Ribbed metal floor plates & center cable channel
                if (x >= 26 && x <= 37) base = 54 + d;
                if (x == 26 || x == 37 || (y % 16 == 0)) base -= 16;
                // Metallic rudder pedal bars near front (y = 8..22)
                if (y >= 8 && y <= 20 && ((x >= 8 && x <= 20) || (x >= 43 && x <= 55))) {
                    base = ((y % 3) == 0) ? 105 : 72;
                    if (x == 8 || x == 20 || x == 43 || x == 55) base = 40;
                }
                // Subtle rivets along side seams
                if ((x == 4 || x == 59) && (y % 8 == 4)) base = 98;
                // Yellow-black caution kickplate stripe along far left/right floor edges
                if ((x <= 2 || x >= 61) && (((x + y) / 4) & 1)) {
                    px[y * S + x] = pack_rgba(148 + d, 118 + d, 32);
                } else {
                    px[y * S + x] = pack_rgba(base, base + 2, base + 5);
                }
            }
        }
        tex_floor_ = create_nearest_rgba_tex(S, S, px.data());
    }

    // 2. Forward Nose Cone, Anti-Glare Coaming & M61A1 Vulcan Gun Port (128x128 pixel art)
    {
        constexpr int S = 128;
        std::vector<uint32_t> px(S * S);
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                int d = pixel_dither(x, y, 8);
                // y=0 is forward radome tip, y=127 is near windshield glare shield
                int r = 128 + d, g = 136 + d, b = 144 + d; // USAF Compass Ghost Grey (FS 36270)
                if (y < 36) {
                    // Darker F-16C radome cone
                    r = 86 + d; g = 92 + d; b = 100 + d;
                    if (y == 35) { r = 52; g = 56; b = 62; } // Radome seam ring
                } else if (y > 92 && x >= 34 && x <= 93) {
                    // Matte charcoal anti-glare strip immediately ahead of HUD
                    r = 46 + d / 2; g = 48 + d / 2; b = 52 + d / 2;
                }
                // Longitudinal panel seams & subtle rivets
                if (x == 32 || x == 95 || y == 64 || y == 92) {
                    r -= 24; g -= 24; b -= 24;
                }
                if ((x == 34 || x == 93) && (y % 10 == 5)) {
                    r += 22; g += 22; b += 22;
                }
                // M61A1 Vulcan 20mm rotary cannon fairing & gas vents on left strake (x = 14..26, y = 70..112)
                if (x >= 14 && x <= 26 && y >= 70 && y <= 112) {
                    r = 62 + d; g = 66 + d; b = 70 + d;
                    if (x >= 17 && x <= 23 && y >= 74 && y <= 96 && (y % 4 == 0)) {
                        r = 22; g = 24; b = 26; // Dark gun port cooling louvers!
                    }
                }
                px[y * S + x] = pack_rgba(r, g, b);
            }
        }
        tex_nose_ = create_nearest_rgba_tex(S, S, px.data());
    }

    // 3. F-16C Swept Delta Wing & Tail Surface (128x128 Compass Ghost Grey pixel art + USAF insignia)
    {
        constexpr int S = 128;
        std::vector<uint32_t> px(S * S);
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                int d = pixel_dither(x, y, 7);
                // x=0 wing root, x=127 wingtip; y=0 leading edge, y=127 trailing edge
                int r = 134 + d, g = 142 + d, b = 150 + d;
                // Darker Compass Ghost Grey forward leading-edge flap strip
                if (y < 22) { r -= 16; g -= 16; b -= 14; }
                // Trailing-edge flaperon & hinge seam
                if (y > 96) { r -= 12; g -= 12; b -= 10; }
                if (y == 22 || y == 96 || x == 42 || x == 86 || y == 58) {
                    r -= 28; g -= 28; b -= 28;
                }
                // Classic dark grey "NO STEP" walkway outline near wing root
                if (x >= 10 && x <= 40 && y >= 28 && y <= 90) {
                    if (x == 10 || x == 40 || y == 28 || y == 90) {
                        r = 56; g = 60; b = 64;
                    }
                }
                // Low-visibility retro pixel-art USAF Star-and-Bar roundel near mid-wing (cx=68, cy=58)
                int dx = x - 68, dy = y - 58;
                if ((dx * dx + dy * dy <= 10 * 10) || (std::abs(dx) <= 20 && std::abs(dy) <= 4)) {
                    r = 88 + d / 2; g = 94 + d / 2; b = 102 + d / 2;
                    if ((dx * dx + dy * dy <= 6 * 6) || (std::abs(dx) <= 17 && std::abs(dy) <= 2)) {
                        r = 160 + d / 2; g = 166 + d / 2; b = 172 + d / 2;
                    }
                }
                // Rivet dots along spar
                if ((y == 24 || y == 94) && (x % 8 == 4)) {
                    r += 20; g += 20; b += 20;
                }
                px[y * S + x] = pack_rgba(r, g, b);
            }
        }
        tex_wing_ = create_nearest_rgba_tex(S, S, px.data());
    }

    // 4. Side Bulkhead & Sill Bridge Panels (64x64 matching Falcon 3.0 grey metal & cyan sill trim)
    {
        constexpr int S = 64;
        std::vector<uint32_t> px(S * S);
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                int d = pixel_dither(x, y, 7);
                int r = 82 + d, g = 86 + d, b = 90 + d;
                if (y < 6) {
                    // Iconic Falcon 3.0 cyan-tinted glass rim + bright metal sill edge
                    r = 95 + d; g = 185 + d; b = 195 + d;
                } else if (y < 14) {
                    // Dark riveted canopy sill channel
                    r = 38 + d; g = 42 + d; b = 46 + d;
                    if (y == 10 && (x % 12 == 6)) { r = 175; g = 180; b = 185; } // Rivet!
                } else if (y < 20) {
                    r = 142 + d; g = 146 + d; b = 150 + d; // Light metallic sill rail highlight
                } else {
                    // Dark side-console switches & ribbed bulkhead
                    r = 66 + d; g = 70 + d; b = 74 + d;
                    if (y % 12 == 0 || x % 16 == 0) { r -= 18; g -= 18; b -= 18; }
                    if ((y % 12 == 6) && (x % 16 == 8)) { r = 130; g = 132; b = 135; } // Toggle switch!
                }
                px[y * S + x] = pack_rgba(r, g, b);
            }
        }
        tex_bulkhead_ = create_nearest_rgba_tex(S, S, px.data());
    }

    // Helper lambdas for crisp 1991 pixel-art aux panels & pedestal
    auto fill_box = [](std::vector<uint32_t>& buf, int W, int H, int x0, int y0, int x1, int y1, uint32_t col) {
        int a0 = std::max(0, x0), a1 = std::min(W - 1, x1);
        int b0 = std::max(0, y0), b1 = std::min(H - 1, y1);
        for (int y = b0; y <= b1; ++y)
            for (int x = a0; x <= a1; ++x)
                buf[y * W + x] = col;
    };
    auto draw_bezel = [&](std::vector<uint32_t>& buf, int W, int H, int x0, int y0, int x1, int y1, uint32_t fill) {
        fill_box(buf, W, H, x0, y0, x1, y1, fill);
        for (int x = x0; x <= x1; ++x) {
            if (x >= 0 && x < W) {
                if (y0 >= 0 && y0 < H) buf[y0 * W + x] = pack_rgba(120, 126, 134);
                if (y1 >= 0 && y1 < H) buf[y1 * W + x] = pack_rgba(26, 28, 32);
            }
        }
        for (int y = y0; y <= y1; ++y) {
            if (y >= 0 && y < H) {
                if (x0 >= 0 && x0 < W) buf[y * W + x0] = pack_rgba(104, 110, 118);
                if (x1 >= 0 && x1 < W) buf[y * W + x1] = pack_rgba(24, 26, 30);
            }
        }
    };
    auto draw_dial = [](std::vector<uint32_t>& buf, int W, int H, int cx, int cy, int rad, uint32_t face_col) {
        for (int y = cy - rad - 2; y <= cy + rad + 2; ++y) {
            if (y < 0 || y >= H) continue;
            for (int x = cx - rad - 2; x <= cx + rad + 2; ++x) {
                if (x < 0 || x >= W) continue;
                int dx = x - cx, dy = y - cy;
                int d2 = dx * dx + dy * dy;
                if (d2 <= rad * rad) {
                    buf[y * W + x] = face_col;
                } else if (d2 <= (rad + 2) * (rad + 2)) {
                    int rim = (dx + dy < 0) ? 118 : 34;
                    buf[y * W + x] = pack_rgba(rim, rim + 2, rim + 5);
                }
            }
        }
        // 12 crisp white dial tick marks around circumference
        for (int k = 0; k < 12; ++k) {
            float a = static_cast<float>(k) * (kPi / 6.0f);
            int tx = cx + static_cast<int>(std::round(std::cos(a) * static_cast<float>(rad - 2)));
            int ty = cy + static_cast<int>(std::round(std::sin(a) * static_cast<float>(rad - 2)));
            if (tx >= 0 && tx < W && ty >= 0 && ty < H) buf[ty * W + tx] = pack_rgba(215, 220, 224);
        }
    };
    auto draw_toggle = [&](std::vector<uint32_t>& buf, int W, int H, int cx, int cy) {
        fill_box(buf, W, H, cx - 2, cy - 2, cx + 2, cy + 2, pack_rgba(26, 28, 32));
        fill_box(buf, W, H, cx - 1, cy - 4, cx + 1, cy, pack_rgba(182, 188, 194));
        fill_box(buf, W, H, cx, cy - 4, cx + 1, cy - 3, pack_rgba(232, 236, 240));
    };

    // 5. F-16A/C Left Auxiliary Panel (128x128 pixel art: Master Arm, EMER JETT, Gear Wheel + 3-Green, CMDS)
    {
        constexpr int S = 128;
        std::vector<uint32_t> px(S * S);
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                int d = pixel_dither(x, y, 6);
                int b = 58 + d;
                // Top cyan-rimmed riveted canopy sill rail (y = 0..10)
                if (y <= 3) {
                    px[y * S + x] = pack_rgba(88 + d, 186 + d, 196 + d);
                } else if (y <= 8) {
                    int r = ((y == 6) && (x % 14 == 7)) ? 175 : (38 + d);
                    px[y * S + x] = pack_rgba(r, r + 2, r + 4);
                } else if (y <= 11) {
                    px[y * S + x] = pack_rgba(124 + d, 128 + d, 134 + d);
                } else {
                    px[y * S + x] = pack_rgba(b, b + 3, b + 6);
                }
            }
        }
        // Rightmost vertical strip (closest to main glare shield): IFF / LASER / ALT REL / MASTER ARM / AUTOPILOT
        draw_bezel(px, S, S, 86, 14, 124, 90, pack_rgba(32, 35, 39));
        fill_box(px, S, S, 94, 20, 102, 28, pack_rgba(210, 214, 218)); // IFF IDENT button
        draw_toggle(px, S, S, 98, 42);  // LASER ARM
        draw_toggle(px, S, S, 114, 42); // ALT REL
        draw_toggle(px, S, S, 98, 62);  // MASTER ARM
        fill_box(px, S, S, 110, 56, 118, 60, pack_rgba(220, 180, 36)); // ARM amber guard
        draw_toggle(px, S, S, 96, 80);  // PITCH AP
        draw_toggle(px, S, S, 114, 80); // ROLL AP

        // Yellow-black striped EMER STORES JETTISON guarded button (x = 42..78, y = 18..48)
        draw_bezel(px, S, S, 42, 18, 78, 48, pack_rgba(28, 30, 34));
        for (int y = 22; y <= 44; ++y) {
            for (int x = 46; x <= 74; ++x) {
                bool yel = (((x + y) / 5) & 1) == 0;
                px[y * S + x] = yel ? pack_rgba(232, 188, 32) : pack_rgba(22, 22, 24);
            }
        }
        fill_box(px, S, S, 53, 28, 67, 38, pack_rgba(44, 46, 50)); // Center jettison plunger

        // Iconic white-wheeled LANDING GEAR handle (x = 12..38, y = 24..72)
        draw_bezel(px, S, S, 10, 18, 38, 76, pack_rgba(34, 38, 42));
        fill_box(px, S, S, 21, 26, 27, 56, pack_rgba(92, 96, 102)); // Metal shaft
        draw_dial(px, S, S, 24, 60, 10, pack_rgba(226, 230, 232));  // White gear wheel knob!
        fill_box(px, S, S, 22, 58, 26, 62, pack_rgba(140, 144, 150));

        // F-16A 3-Green Landing Gear Silhouette Indicator + Anti-Skid / Stores / CMDS lower panel
        draw_bezel(px, S, S, 10, 80, 78, 122, pack_rgba(30, 33, 37));
        // Tiny green gear-status lamps (nose + left + right)
        fill_box(px, S, S, 24, 88, 28, 92, pack_rgba(32, 212, 64));
        fill_box(px, S, S, 16, 100, 20, 104, pack_rgba(32, 212, 64));
        fill_box(px, S, S, 32, 100, 36, 104, pack_rgba(32, 212, 64));
        // Anti-skid / Stores CAT I-III / Lights / CMDS toggles
        draw_toggle(px, S, S, 50, 94);
        draw_toggle(px, S, S, 66, 94);
        draw_toggle(px, S, S, 50, 112);
        draw_toggle(px, S, S, 66, 112);
        draw_bezel(px, S, S, 84, 94, 124, 122, pack_rgba(30, 33, 37));
        draw_toggle(px, S, S, 96, 108);
        draw_toggle(px, S, S, 112, 108);

        tex_left_aux_ = create_nearest_rgba_tex(S, S, px.data());
    }

    // 6. F-16 Right Auxiliary Panel + Front 4x8 CAUTION/WARNING Matrix (128x128 pixel art)
    {
        constexpr int S = 128;
        std::vector<uint32_t> px(S * S);
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                int d = pixel_dither(x, y, 6);
                int b = 58 + d;
                if (y <= 3) {
                    px[y * S + x] = pack_rgba(88 + d, 186 + d, 196 + d);
                } else if (y <= 8) {
                    int r = ((y == 6) && (x % 14 == 7)) ? 175 : (38 + d);
                    px[y * S + x] = pack_rgba(r, r + 2, r + 4);
                } else if (y <= 11) {
                    px[y * S + x] = pack_rgba(124 + d, 128 + d, 134 + d);
                } else {
                    px[y * S + x] = pack_rgba(b, b + 3, b + 6);
                }
            }
        }
        // Iconic 4x8 CAUTION / WARNING Annunciator Matrix moved right up front (x = 6..66, y = 16..92)
        draw_bezel(px, S, S, 6, 16, 66, 92, pack_rgba(14, 16, 18));
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 4; ++col) {
                int bx0 = 9 + col * 14;
                int by0 = 19 + row * 9;
                fill_box(px, S, S, bx0, by0, bx0 + 11, by0 + 6, pack_rgba(32, 28, 18));
                // Subtle unlit dim-amber legend pixels inside each warning cell
                fill_box(px, S, S, bx0 + 2, by0 + 2, bx0 + 9, by0 + 4, pack_rgba(68, 56, 26));
            }
        }
        // Round Instrument Dials on outer right: HYD PRESS A&B, FUEL QTY, LIQUID OXY, EPU FUEL, CLOCK
        draw_dial(px, S, S, 96, 30, 15, pack_rgba(18, 20, 24));  // HYD A&B
        fill_box(px, S, S, 96, 20, 97, 30, pack_rgba(232, 232, 224)); // Hyd needle
        draw_dial(px, S, S, 96, 66, 15, pack_rgba(18, 20, 24));  // FUEL QTY
        fill_box(px, S, S, 87, 66, 96, 67, pack_rgba(232, 198, 46)); // Fuel needle
        draw_dial(px, S, S, 24, 108, 12, pack_rgba(18, 24, 20)); // LIQUID OXY (green status lamp)
        fill_box(px, S, S, 22, 108, 26, 112, pack_rgba(36, 204, 72));
        draw_dial(px, S, S, 56, 108, 12, pack_rgba(18, 20, 24)); // EPU FUEL
        fill_box(px, S, S, 56, 100, 57, 108, pack_rgba(220, 220, 212));
        draw_dial(px, S, S, 96, 104, 14, pack_rgba(16, 18, 22)); // CLOCK
        fill_box(px, S, S, 96, 94, 97, 104, pack_rgba(230, 234, 238));
        fill_box(px, S, S, 96, 104, 104, 105, pack_rgba(230, 234, 238));

        tex_right_aux_ = create_nearest_rgba_tex(S, S, px.data());
    }

    // 7. Lower Center Pedestal (128x64 pixel art: Backup Airspeed, Backup ADI bezel, HSI Compass bezel)
    {
        constexpr int W = 128, H = 64;
        std::vector<uint32_t> px(W * H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                int d = pixel_dither(x, y, 6);
                int b = 52 + d;
                if (y <= 2) b = 82 + d; // Top pedestal bevel under MFD
                px[y * W + x] = pack_rgba(b, b + 2, b + 5);
            }
        }
        draw_bezel(px, W, H, 4, 4, 123, 59, pack_rgba(38, 41, 46));
        draw_dial(px, W, H, 26, 30, 19, pack_rgba(16, 18, 22));  // Left: Backup Airspeed / Mach
        draw_dial(px, W, H, 64, 30, 21, pack_rgba(22, 36, 54));  // Center: Backup ADI Ball housing
        draw_dial(px, W, H, 102, 30, 19, pack_rgba(16, 18, 22)); // Right: HSI Compass housing
        // Subtle yellow-black ACES II ejection handle top bar at very bottom center
        for (int y = 55; y <= 62; ++y) {
            for (int x = 48; x <= 80; ++x) {
                bool yel = (((x + y) / 4) & 1) == 0;
                px[y * W + x] = yel ? pack_rgba(232, 188, 32) : pack_rgba(20, 20, 22);
            }
        }
        tex_pedestal_ = create_nearest_rgba_tex(W, H, px.data());
    }
}

void Cockpit3D::draw_forward_panel(GLuint tex, float r, float g, float b) {
    if (!tex) return;
    // Exact 1:1 optical frustum dimensions at z = -0.85 m matching Falcon 3.0's Forward Cockpit (cy=53, fy=285.75)!
    constexpr float kZ = -0.85f;
    constexpr float kHalfW = 0.405f;
    constexpr float kTopY  = 0.160f;
    constexpr float kBotY  = -0.440f;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(r, g, b, 1.0f);

    glBegin(GL_QUADS);
    // Trim 1.5 VGA px off left/right edges (u=0.005..0.995) to clip Falcon's 1-px black border line!
    glTexCoord2f(0.005f, 1.0f); glVertex3f(-kHalfW, kBotY, kZ);
    glTexCoord2f(0.995f, 1.0f); glVertex3f( kHalfW, kBotY, kZ);
    glTexCoord2f(0.995f, 0.0f); glVertex3f( kHalfW, kTopY, kZ);
    glTexCoord2f(0.005f, 0.0f); glVertex3f(-kHalfW, kTopY, kZ);
    glEnd();

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

void Cockpit3D::draw_aux_panels_and_pedestal(float r, float g, float b, float daylight,
                                             const LiveCockpitState* st) {
    // Match Falcon 3.0's live Day steel-grey & iconic Night warm-orange cockpit backlighting!
    float night_t = std::clamp((0.62f - daylight) / 0.45f, 0.0f, 1.0f);
    float pr = std::min(1.0f, r * (1.0f - night_t) + (0.78f * night_t));
    float pg = std::min(1.0f, g * (1.0f - night_t) + (0.38f * night_t));
    float pb = std::min(1.0f, b * (1.0f - night_t) + (0.16f * night_t));

    glEnable(GL_TEXTURE_2D);
    glColor3f(pr, pg, pb);

    // 1. LEFT AUXILIARY PANEL (gentler angle facing pilot: x=-0.400 -> -0.620, z=-0.848 -> -0.775)
    if (tex_left_aux_) {
        glBindTexture(GL_TEXTURE_2D, tex_left_aux_);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.610f, -0.545f, -0.775f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(-0.400f, -0.530f, -0.848f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(-0.400f, -0.175f, -0.848f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.625f, -0.210f, -0.775f);
        glEnd();
    }

    // 2. RIGHT AUXILIARY PANEL + FRONT 4x8 CAUTION/WARNING MATRIX (x=+0.400 -> +0.620, z=-0.848 -> -0.775)
    if (tex_right_aux_) {
        glBindTexture(GL_TEXTURE_2D, tex_right_aux_);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex3f( 0.400f, -0.530f, -0.848f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f( 0.610f, -0.545f, -0.775f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f( 0.625f, -0.210f, -0.775f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f( 0.400f, -0.175f, -0.848f);
        glEnd();
    }

    // 3. LOWER CENTER PEDESTAL (directly below MFD: y = -0.438..-0.575 so all 3 live dials stay cleanly visible!)
    if (tex_pedestal_) {
        glBindTexture(GL_TEXTURE_2D, tex_pedestal_);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.402f, -0.575f, -0.822f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f( 0.402f, -0.575f, -0.822f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f( 0.402f, -0.438f, -0.848f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.402f, -0.438f, -0.848f);
        glEnd();
    }

    glDisable(GL_TEXTURE_2D);

    // 4. OPTION B: Continuous riveted cyan-rimmed canopy sill rail flowing cleanly from Forward glare shield -> Side consoles!
    float lit = 0.28f + 0.72f * daylight;
    for (int side = -1; side <= 1; side += 2) {
        float s = static_cast<float>(side);
        glLineWidth(2.0f);
        glColor3f(0.36f * lit, 0.78f * lit, 0.84f * lit);
        glBegin(GL_LINE_STRIP);
        glVertex3f(s * 0.402f, -0.174f, -0.846f);
        glVertex3f(s * 0.625f, -0.209f, -0.773f);
        glVertex3f(s * 0.560f, -0.180f,  0.480f);
        glEnd();
    }
    glLineWidth(1.0f);

    if (!st) return;

    // 5. LIVE ANIMATED INSTRUMENTS ON CENTER PEDESTAL (Airspeed needle, Orange/Blue Backup ADI, HSI Compass!)
    auto ped_pt = [](float u, float v, float& ox, float& oy, float& oz) {
        ox = -0.402f + u * 0.804f;
        oy = -0.438f - v * 0.137f;
        oz = -0.845f + v * 0.026f;
    };
    constexpr float kDeg2Rad = kPi / 180.0f;

    // 5a. Live Backup Airspeed needle (center u = 26/128 = 0.203, v = 30/64 = 0.469)
    {
        float cx, cy, cz;
        ped_pt(26.0f / 128.0f, 30.0f / 64.0f, cx, cy, cz);
        float ang = (std::clamp(st->airspeed_kts, 0.0f, 800.0f) / 800.0f) * 2.0f * kPi - kPi * 0.5f;
        float rad = 0.042f;
        glLineWidth(2.0f);
        glColor3f(0.95f, 0.96f, 0.92f);
        glBegin(GL_LINES);
        glVertex3f(cx, cy, cz);
        glVertex3f(cx + std::cos(ang) * rad, cy - std::sin(ang) * rad * 0.95f, cz);
        glEnd();
    }

    // 5b. Live F-16A Blue/Brown Backup ADI Horizon Ball (center u = 0.5, v = 30/64 = 0.469)
    {
        float cx, cy, cz;
        ped_pt(0.50f, 30.0f / 64.0f, cx, cy, cz);
        float rad = 0.046f;
        float roll_r = -st->roll_deg * kDeg2Rad;
        float pitch_off = std::clamp(st->pitch_deg / 45.0f, -0.75f, 0.75f) * rad;
        float cr = std::cos(roll_r), sr = std::sin(roll_r);
        // Draw sky (upper half) & ground (lower half) clipped inside circular ADI bezel
        constexpr int kSeg = 16;
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(0.20f * lit, 0.48f * lit, 0.76f * lit); // ADI sky blue
        glVertex3f(cx, cy, cz);
        for (int i = 0; i <= kSeg; ++i) {
            float a = kPi * (static_cast<float>(i) / static_cast<float>(kSeg)); // 0..pi (upper half)
            float lx = std::cos(a) * rad;
            float ly = std::clamp(std::sin(a) * rad + pitch_off, -rad, rad);
            glVertex3f(cx + lx * cr - ly * sr, cy + lx * sr + ly * cr, cz);
        }
        glEnd();
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(0.56f * lit, 0.34f * lit, 0.16f * lit); // ADI earth brown/orange
        glVertex3f(cx, cy, cz);
        for (int i = 0; i <= kSeg; ++i) {
            float a = kPi + kPi * (static_cast<float>(i) / static_cast<float>(kSeg)); // pi..2pi (lower half)
            float lx = std::cos(a) * rad;
            float ly = std::clamp(std::sin(a) * rad + pitch_off, -rad, rad);
            glVertex3f(cx + lx * cr - ly * sr, cy + lx * sr + ly * cr, cz);
        }
        glEnd();
        // Crisp white horizon line & fixed yellow F-16 miniature aircraft bars
        glLineWidth(1.8f);
        glColor3f(0.95f, 0.96f, 0.98f);
        glBegin(GL_LINES);
        glVertex3f(cx - rad * 0.85f * cr - pitch_off * sr, cy - rad * 0.85f * sr + pitch_off * cr, cz + 0.001f);
        glVertex3f(cx + rad * 0.85f * cr - pitch_off * sr, cy + rad * 0.85f * sr + pitch_off * cr, cz + 0.001f);
        glEnd();
        glColor3f(1.0f, 0.86f, 0.18f);
        glBegin(GL_LINES);
        glVertex3f(cx - 0.026f, cy, cz + 0.002f); glVertex3f(cx - 0.007f, cy, cz + 0.002f);
        glVertex3f(cx + 0.007f, cy, cz + 0.002f); glVertex3f(cx + 0.026f, cy, cz + 0.002f);
        glVertex3f(cx - 0.004f, cy, cz + 0.002f); glVertex3f(cx + 0.004f, cy, cz + 0.002f);
        glEnd();
    }

    // 5c. Live HSI Compass Heading Needle (center u = 102/128 = 0.797, v = 30/64 = 0.469)
    {
        float cx, cy, cz;
        ped_pt(102.0f / 128.0f, 30.0f / 64.0f, cx, cy, cz);
        float ang = -st->yaw_deg * kDeg2Rad + kPi * 0.5f;
        float rad = 0.040f;
        glLineWidth(2.0f);
        glColor3f(0.96f, 0.82f, 0.24f);
        glBegin(GL_LINES);
        glVertex3f(cx - std::cos(ang) * rad * 0.55f, cy - std::sin(ang) * rad * 0.55f, cz);
        glVertex3f(cx + std::cos(ang) * rad,         cy + std::sin(ang) * rad,         cz);
        glEnd();
    }

    // 6. LIVE CAUTION / WARNING LAMP HIGHLIGHTS ON FRONT-RIGHT 4x8 MATRIX!
    // Right Aux quad: u=0 is x=+0.400, z=-0.848; u=1 is x=+0.565..0.585, z=-0.745
    auto draw_warn_cell = [&](int col, int row, float wr, float wg, float wb) {
        float u0 = (9.0f + col * 14.0f) / 128.0f;
        float u1 = (20.0f + col * 14.0f) / 128.0f;
        float v0 = (19.0f + row * 9.0f) / 128.0f;
        float v1 = (25.0f + row * 9.0f) / 128.0f;
        auto pt = [](float u, float v, float& ox, float& oy, float& oz) {
            float tx = 0.400f + u * (0.625f - 0.400f);
            float bx = 0.400f + u * (0.610f - 0.400f);
            ox = tx + v * (bx - tx);
            float ty = -0.175f + u * (-0.210f - (-0.175f));
            float by = -0.530f + u * (-0.545f - (-0.530f));
            oy = ty + v * (by - ty);
            oz = -0.848f + u * (-0.775f - (-0.848f)) + 0.004f;
        };
        float x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3;
        pt(u0, v0, x0, y0, z0); pt(u1, v0, x1, y1, z1);
        pt(u1, v1, x2, y2, z2); pt(u0, v1, x3, y3, z3);
        glColor3f(wr, wg, wb);
        glBegin(GL_QUADS);
        glVertex3f(x0, y0, z0); glVertex3f(x1, y1, z1);
        glVertex3f(x2, y2, z2); glVertex3f(x3, y3, z3);
        glEnd();
    };
    bool blink = (std::fmod(st->sim_time_sec, 0.55f) < 0.30f);
    if (st->missile_warning && blink) draw_warn_cell(0, 0, 1.0f, 0.22f, 0.14f); // Red Threat/Missile alert!
    if (st->agl_ft < 350.0f && blink) draw_warn_cell(1, 0, 1.0f, 0.72f, 0.12f); // Amber Pull-Up / Low Alt!
    if (std::abs(st->g_load) > 6.5f)  draw_warn_cell(0, 1, 1.0f, 0.78f, 0.16f); // Amber Over-G!
    if (st->airspeed_kts > 40.0f && st->airspeed_kts < 145.0f && blink) {
        draw_warn_cell(2, 0, 1.0f, 0.65f, 0.12f); // Amber Low Speed / Stall!
    }
    glLineWidth(1.0f);
}

void Cockpit3D::draw_side_consoles(float r, float g, float b, bool wide_modern_layout) {
    // Left Console (Falcon View 4: gloved left hand on throttle, fuel/compass/AUTR panel, riveted cyan canopy sill)
    // Right Console (Falcon View 6: gloved right hand on sidestick, caution warning panel, riveted cyan canopy sill)
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(r, g, b, 1.0f);

    float fx = wide_modern_layout ? 0.61f : 0.405f;
    float fx_top = wide_modern_layout ? 0.63f : 0.46f;
    float fz = wide_modern_layout ? -0.775f : -0.84f;
    float bx = wide_modern_layout ? 0.56f : 0.44f;
    float bx_top = wide_modern_layout ? 0.58f : 0.50f;
    float by_bot = wide_modern_layout ? -0.58f : -0.46f;
    float fy_bot = wide_modern_layout ? -0.55f : -0.44f;
    float y_top = wide_modern_layout ? 0.02f : 0.14f;

    // Left Console Wall (-X): view4 sprite has front-left on right edge (u=1), rear-left on left edge (u=0)
    if (has_view_[ViewLeft] && view_tex_[ViewLeft]) {
        glBindTexture(GL_TEXTURE_2D, view_tex_[ViewLeft]);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(-bx,     by_bot, 0.50f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(-fx,     fy_bot, fz);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(-fx_top, y_top,  fz);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(-bx_top, y_top,  0.50f);
        glEnd();
    }

    // Right Console Wall (+X): view6 sprite has front-right on left edge (u=0), rear-right on right edge (u=1)
    if (has_view_[ViewRight] && view_tex_[ViewRight]) {
        glBindTexture(GL_TEXTURE_2D, view_tex_[ViewRight]);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex3f( fx,     fy_bot, fz);
        glTexCoord2f(1.0f, 1.0f); glVertex3f( bx,     by_bot, 0.50f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f( bx_top, y_top,  0.50f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f( fx_top, y_top,  fz);
        glEnd();
    }

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

void Cockpit3D::draw_rear_cockpit(float r, float g, float b) {
    // Rear 6-o'clock sprite (Falcon View 5: ACES II ejection seat headrest, rear canopy bow arch, rear deck & tail)
    if (!view_tex_[ViewRear]) return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, view_tex_[ViewRear]);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(r, g, b, 1.0f);

    // Placed at z = +0.54 m behind pilot's seat; u=0 is pilot's right (+X when looking back!), u=1 is pilot's left (-X)
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex3f( 0.48f, -0.46f, 0.52f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(-0.48f, -0.46f, 0.52f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(-0.48f,  0.34f, 0.52f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f( 0.48f,  0.34f, 0.52f);
    glEnd();

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

void Cockpit3D::draw_tub_floor_and_bridges(float r, float g, float b, bool wide_modern_layout) {
    glEnable(GL_TEXTURE_2D);

    float fl_y = wide_modern_layout ? -0.620f : -0.455f;
    float fl_x = wide_modern_layout ?  0.620f :  0.460f;
    float wall_x = wide_modern_layout ? 0.620f : 0.480f;
    float wall_fz = wide_modern_layout ? -0.775f : -0.880f;

    // 1. Watertight Cockpit Tub Floor & Rudder Pedal Footwell
    glBindTexture(GL_TEXTURE_2D, tex_floor_);
    glColor3f(r * 0.85f, g * 0.85f, b * 0.88f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-fl_x, fl_y, -0.88f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( fl_x, fl_y, -0.88f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( fl_x, fl_y,  0.54f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-fl_x, fl_y,  0.54f);
    glEnd();

    // 2. Lower Side Sill Backing & Forward-Corner Bridge Bulkheads (strictly BELOW glare shield y = -0.195!)
    glBindTexture(GL_TEXTURE_2D, tex_bulkhead_);
    glColor3f(r * 0.80f, g * 0.80f, b * 0.84f);
    glBegin(GL_QUADS);
    // Left lower outer tub wall
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-wall_x, fl_y,  0.54f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(-wall_x, fl_y,  wall_fz);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(-wall_x - 0.02f, -0.22f, wall_fz);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-wall_x - 0.02f, -0.18f, 0.54f);

    // Right lower outer tub wall
    glTexCoord2f(0.0f, 1.0f); glVertex3f( wall_x, fl_y,  wall_fz);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( wall_x, fl_y,  0.54f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( wall_x + 0.02f, -0.18f, 0.54f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f( wall_x + 0.02f, -0.22f, wall_fz);

    // Forward lower footwell bulkhead backing (strictly below glare shield y = -0.195 so zero bar peeks into sky!)
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.44f, fl_y, -0.88f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 0.44f, fl_y, -0.88f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 0.44f, -0.195f, -0.88f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.44f, -0.195f, -0.88f);

    // Rear lower seat bulkhead backing (behind Rear sprite)
    glTexCoord2f(0.0f, 1.0f); glVertex3f( wall_x, fl_y,  0.55f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(-wall_x, fl_y,  0.55f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(-wall_x, -0.16f, 0.55f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f( wall_x, -0.16f, 0.55f);
    glEnd();

    glDisable(GL_TEXTURE_2D);
}

void Cockpit3D::draw_exterior_airframe(float r, float g, float b, float daylight, float sim_time_sec) {
    // Full 3D Pixel-Art F-16C Exterior visible around the bubble canopy:
    //   - Forward Radome Nose & Anti-Glare Coaming (z = -0.88 .. -4.8 m)
    //   - LERX Strakes + Swept Delta Wings (x = +-0.52 .. +-4.75 m) + Wingtip AIM-9M Sidewinders!
    //   - Dorsal Fuselage Spine + Tall Swept Vertical Tailfin + Horizontal Stabilators!
    glEnable(GL_TEXTURE_2D);

    // 1. Forward F-16C Nose Cone & Glare Shield Deck (visible when looking down-left/down-right over nose!)
    glBindTexture(GL_TEXTURE_2D, tex_nose_);
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    // Upper nose deck sloping down from glare shield (z=-0.88, y=-0.165) to radome tip (z=-4.85, y=-0.42)
    glTexCoord2f(0.15f, 1.0f); glVertex3f(-0.44f, -0.165f, -0.88f);
    glTexCoord2f(0.85f, 1.0f); glVertex3f( 0.44f, -0.165f, -0.88f);
    glTexCoord2f(0.72f, 0.30f); glVertex3f( 0.24f, -0.29f, -3.40f);
    glTexCoord2f(0.28f, 0.30f); glVertex3f(-0.24f, -0.29f, -3.40f);

    // Left nose cheek
    glColor3f(r * 0.82f, g * 0.82f, b * 0.85f);
    glTexCoord2f(0.0f, 1.0f);  glVertex3f(-0.56f, -0.36f, -0.88f);
    glTexCoord2f(0.15f, 1.0f); glVertex3f(-0.44f, -0.165f, -0.88f);
    glTexCoord2f(0.28f, 0.30f); glVertex3f(-0.24f, -0.29f, -3.40f);
    glTexCoord2f(0.05f, 0.30f); glVertex3f(-0.30f, -0.44f, -3.40f);

    // Right nose cheek
    glTexCoord2f(0.85f, 1.0f); glVertex3f( 0.44f, -0.165f, -0.88f);
    glTexCoord2f(1.0f, 1.0f);  glVertex3f( 0.56f, -0.36f, -0.88f);
    glTexCoord2f(0.95f, 0.30f); glVertex3f( 0.30f, -0.44f, -3.40f);
    glTexCoord2f(0.72f, 0.30f); glVertex3f( 0.24f, -0.29f, -3.40f);
    glEnd();

    // Pointed F-16C Radome Nose Tip (z = -3.40 .. -4.95 m)
    glBegin(GL_TRIANGLES);
    glColor3f(r * 0.90f, g * 0.90f, b * 0.92f);
    glTexCoord2f(0.28f, 0.30f); glVertex3f(-0.24f, -0.27f, -3.40f);
    glTexCoord2f(0.72f, 0.30f); glVertex3f( 0.24f, -0.27f, -3.40f);
    glTexCoord2f(0.50f, 0.00f); glVertex3f( 0.00f, -0.42f, -4.95f);

    glColor3f(r * 0.74f, g * 0.74f, b * 0.78f);
    glTexCoord2f(0.05f, 0.30f); glVertex3f(-0.30f, -0.44f, -3.40f);
    glTexCoord2f(0.28f, 0.30f); glVertex3f(-0.24f, -0.27f, -3.40f);
    glTexCoord2f(0.50f, 0.00f); glVertex3f( 0.00f, -0.42f, -4.95f);

    glTexCoord2f(0.72f, 0.30f); glVertex3f( 0.24f, -0.27f, -3.40f);
    glTexCoord2f(0.95f, 0.30f); glVertex3f( 0.30f, -0.44f, -3.40f);
    glTexCoord2f(0.50f, 0.00f); glVertex3f( 0.00f, -0.42f, -4.95f);
    glEnd();

    // 2. Left & Right LERX Strakes + Swept Delta F-16C Main Wings (40 deg LE sweep!)
    glBindTexture(GL_TEXTURE_2D, tex_wing_);
    for (int side = -1; side <= 1; side += 2) {
        float s = static_cast<float>(side);
        // LERX forward strake along cockpit sides
        glColor3f(r * 0.95f, g * 0.95f, b * 0.97f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(s * 0.50f, -0.18f, -1.60f);
        glTexCoord2f(0.2f, 0.1f); glVertex3f(s * 1.10f, -0.24f,  0.30f);
        glTexCoord2f(0.2f, 0.8f); glVertex3f(s * 1.10f, -0.24f,  3.55f);
        glTexCoord2f(0.0f, 0.8f); glVertex3f(s * 0.56f, -0.16f,  3.55f);

        // Main Swept Delta Wing (upper surface)
        glColor3f(r * 1.02f, g * 1.02f, b * 1.04f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(s * 1.08f, -0.24f,  0.35f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(s * 4.72f, -0.26f,  2.75f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(s * 4.72f, -0.27f,  3.60f);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(s * 1.08f, -0.24f,  3.55f);

        // Main Wing underside (for inverted/angled looks)
        glColor3f(r * 0.68f, g * 0.68f, b * 0.72f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(s * 1.08f, -0.32f,  0.35f);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(s * 1.08f, -0.32f,  3.55f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(s * 4.72f, -0.30f,  3.60f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(s * 4.72f, -0.30f,  2.75f);

        // Wing leading-edge thickness strip
        glColor3f(r * 0.78f, g * 0.78f, b * 0.82f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(s * 1.08f, -0.24f,  0.35f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(s * 4.72f, -0.26f,  2.75f);
        glTexCoord2f(1.0f, 0.1f); glVertex3f(s * 4.72f, -0.30f,  2.75f);
        glTexCoord2f(0.0f, 0.1f); glVertex3f(s * 1.08f, -0.32f,  0.35f);

        // Horizontal Stabilator (swept tailplane)
        glColor3f(r * 0.94f, g * 0.94f, b * 0.97f);
        glTexCoord2f(0.1f, 0.0f); glVertex3f(s * 0.76f, -0.18f,  5.05f);
        glTexCoord2f(0.9f, 0.0f); glVertex3f(s * 2.75f, -0.22f,  6.20f);
        glTexCoord2f(0.9f, 0.9f); glVertex3f(s * 2.75f, -0.22f,  6.95f);
        glTexCoord2f(0.1f, 0.9f); glVertex3f(s * 0.76f, -0.18f,  6.85f);
        glEnd();
    }

    // 3. Rear Fuselage Dorsal Spine & Tall F-16C Vertical Tailfin (visible when checking 6 o'clock!)
    glColor3f(r * 0.88f, g * 0.88f, b * 0.92f);
    glBegin(GL_QUADS);
    // Dorsal spine behind canopy
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.46f, -0.02f,  0.56f);
    glTexCoord2f(0.3f, 0.0f); glVertex3f( 0.46f, -0.02f,  0.56f);
    glTexCoord2f(0.3f, 1.0f); glVertex3f( 0.42f, -0.08f,  6.80f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.42f, -0.08f,  6.80f);

    // Tall F-16C Vertical Stabilizer Fin (both sides!)
    glColor3f(r * 0.92f, g * 0.92f, b * 0.96f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.04f, -0.06f,  4.10f);
    glTexCoord2f(0.6f, 1.0f); glVertex3f(-0.04f, -0.06f,  6.85f);
    glTexCoord2f(0.5f, 0.0f); glVertex3f(-0.03f,  2.50f,  7.05f);
    glTexCoord2f(0.1f, 0.0f); glVertex3f(-0.03f,  2.50f,  6.15f);

    glColor3f(r * 0.84f, g * 0.84f, b * 0.88f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f( 0.04f, -0.06f,  4.10f);
    glTexCoord2f(0.1f, 0.0f); glVertex3f( 0.03f,  2.50f,  6.15f);
    glTexCoord2f(0.5f, 0.0f); glVertex3f( 0.03f,  2.50f,  7.05f);
    glTexCoord2f(0.6f, 1.0f); glVertex3f( 0.04f, -0.06f,  6.85f);
    glEnd();

    glDisable(GL_TEXTURE_2D);

    // 4. Crisp 3D Wingtip Launch Rails + AIM-9M Sidewinder Missiles (iconic F-16C silhouette on 3/9 o'clock!)
    for (int side = -1; side <= 1; side += 2) {
        float wx = static_cast<float>(side) * 4.78f;
        // Dark gunmetal LAU-129 wingtip rail
        draw_box(wx - 0.04f, -0.30f, 2.55f, wx + 0.04f, -0.23f, 3.70f, r * 0.32f, g * 0.34f, b * 0.36f);
        // White/light-grey AIM-9M missile body
        float mx = static_cast<float>(side) * 4.86f;
        draw_box(mx - 0.045f, -0.31f, 1.75f, mx + 0.045f, -0.22f, 4.15f, r * 0.88f, g * 0.88f, b * 0.86f);
        // Dark infrared seeker dome & forward canards
        draw_box(mx - 0.035f, -0.30f, 1.58f, mx + 0.035f, -0.23f, 1.76f, r * 0.22f, g * 0.24f, b * 0.26f);
        draw_box(mx - 0.14f,  -0.275f, 1.85f, mx + 0.14f,  -0.255f, 2.08f, r * 0.35f, g * 0.36f, b * 0.38f);
        // Rear rollerons/fins
        draw_box(mx - 0.18f,  -0.275f, 3.80f, mx + 0.18f,  -0.255f, 4.14f, r * 0.35f, g * 0.36f, b * 0.38f);
        draw_box(mx - 0.015f, -0.44f,  3.80f, mx + 0.015f, -0.09f,  4.14f, r * 0.35f, g * 0.36f, b * 0.38f);

        // Option C: Underwing 370-gal external fuel tank (inner pylon, strictly aft at z=+0.95..+3.35 m!)
        float tx = static_cast<float>(side) * 1.65f;
        draw_box(tx - 0.04f, -0.52f, 1.45f, tx + 0.04f, -0.26f, 2.75f, r * 0.42f, g * 0.44f, b * 0.46f);
        draw_box(tx - 0.17f, -0.78f, 1.05f, tx + 0.17f, -0.48f, 3.25f, r * 0.68f, g * 0.70f, b * 0.73f);
        // Underwing AIM-120 AMRAAM on mid-wing pylon (z=+1.35..+3.25 m)
        float ax = static_cast<float>(side) * 2.85f;
        draw_box(ax - 0.03f, -0.44f, 1.65f, ax + 0.03f, -0.26f, 2.85f, r * 0.38f, g * 0.40f, b * 0.42f);
        draw_box(ax - 0.055f, -0.52f, 1.35f, ax + 0.055f, -0.42f, 3.25f, r * 0.84f, g * 0.86f, b * 0.88f);
    }

    // 4b. Option C: Dusk / Night Port (Red) & Starboard (Green) Wingtip Navigation & Anti-Collision Strobe Lights!
    if (daylight < 0.72f) {
        float night_str = std::clamp((0.72f - daylight) / 0.50f, 0.20f, 1.0f);
        bool strobe_on = (std::fmod(sim_time_sec, 1.35f) < 0.09f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int side = -1; side <= 1; side += 2) {
            float lx = static_cast<float>(side) * 4.74f;
            float ly = -0.22f;
            float lz = 2.68f;
            float nr = (side < 0) ? 1.0f : 0.12f;
            float ng = (side < 0) ? 0.14f : 1.0f;
            float nb = (side < 0) ? 0.10f : 0.32f;
            // Soft outer halo
            glPointSize(16.0f);
            glBegin(GL_POINTS);
            glColor4f(nr, ng, nb, 0.38f * night_str);
            glVertex3f(lx, ly, lz);
            glEnd();
            // Bright nav lamp core
            glPointSize(6.5f);
            glBegin(GL_POINTS);
            glColor4f(std::min(1.0f, nr + 0.35f), std::min(1.0f, ng + 0.35f), std::min(1.0f, nb + 0.35f), 0.95f * night_str);
            glVertex3f(lx, ly, lz);
            glEnd();
            // Periodic crisp white anti-collision strobe pulse!
            if (strobe_on) {
                glPointSize(24.0f);
                glBegin(GL_POINTS);
                glColor4f(1.0f, 1.0f, 0.96f, 0.55f * night_str);
                glVertex3f(lx, ly + 0.02f, lz + 0.15f);
                glEnd();
                glPointSize(10.0f);
                glBegin(GL_POINTS);
                glColor4f(1.0f, 1.0f, 1.0f, 0.98f * night_str);
                glVertex3f(lx, ly + 0.02f, lz + 0.15f);
                glEnd();
            }
        }
        glPointSize(1.0f);
        glDisable(GL_BLEND);
    }

    // 5. Thin F-16C Nose Pitot Spike at radome tip
    glLineWidth(2.0f);
    glColor3f(r * 0.55f, g * 0.58f, b * 0.62f);
    glBegin(GL_LINES);
    glVertex3f(0.0f, -0.42f, -4.95f);
    glVertex3f(0.0f, -0.43f, -5.55f);
    glEnd();
    glLineWidth(1.0f);
}

void Cockpit3D::draw_canopy_frame_and_glint(float daylight) {
    // 1. Solid 3D Rear Canopy Bow Arch curving over the pilot's head from Left sill to Right sill!
    float lit = 0.24f + 0.76f * daylight;
    constexpr int kSegs = 20;
    constexpr float kZ0 = 0.36f, kZ1 = 0.44f;
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= kSegs; ++i) {
        float a = kPi * (static_cast<float>(i) / static_cast<float>(kSegs)); // 0 (right) -> pi (left)
        float cx = std::cos(a) * 0.50f;
        float cy = -0.02f + std::sin(a) * 0.47f;
        float shade = 0.65f + 0.35f * std::sin(a);
        glColor3f(0.56f * lit * shade, 0.58f * lit * shade, 0.61f * lit * shade);
        glVertex3f(cx, cy, kZ0);
        glVertex3f(cx, cy, kZ1);
    }
    glEnd();

    // 2. Subtle iconic Falcon 3.0 cyan bubble-canopy glass rim sheen along the upper canopy bow & sills
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(2.0f);
    glColor4f(0.32f * lit, 0.82f * lit, 0.90f * lit, 0.35f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= kSegs; ++i) {
        float a = kPi * (static_cast<float>(i) / static_cast<float>(kSegs));
        glVertex3f(std::cos(a) * 0.49f, -0.01f + std::sin(a) * 0.45f, kZ0 - 0.01f);
    }
    glEnd();
    glLineWidth(1.0f);
    glDisable(GL_BLEND);
}

void Cockpit3D::render(float daylight, const float ground_mul[3],
                       GLuint live_fwd_cockpit_tex, bool use_live_forward,
                       const LiveCockpitState* live_state,
                       bool show_modern_aux) {
    ensure_gl_resources();

    // Subtle lighting tint for procedural airframe/floor matching live Day / Sunset / Night palette
    float dm = 0.22f + 0.78f * daylight;
    float air_r = std::min(1.0f, dm * (0.65f + 0.35f * ground_mul[0]));
    float air_g = std::min(1.0f, dm * (0.65f + 0.35f * ground_mul[1]));
    float air_b = std::min(1.0f, dm * (0.65f + 0.35f * ground_mul[2]));

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);

    // 1. Exterior 3D pixel-art F-16 airframe (nose, wings, underwing tanks/AMRAAMs, wingtip AIM-9s, nav/strobe lights, tailfin)
    draw_exterior_airframe(air_r, air_g, air_b, daylight, live_state ? live_state->sim_time_sec : 0.0f);

    // 2. Watertight cockpit floor, footwell, & side backing bulkheads
    draw_tub_floor_and_bridges(air_r, air_g, air_b, show_modern_aux);

    // 3. Falcon 3.0's 4 authentic Spectrum HoloByte pixel-art cockpit panels (+ optional Modern 3D Aux & Live Pedestal!)
    GLuint fwd_tex = (use_live_forward && live_fwd_cockpit_tex != 0)
                     ? live_fwd_cockpit_tex
                     : (has_view_[ViewForward] ? view_tex_[ViewForward] : 0);
    draw_forward_panel(fwd_tex, 1.0f, 1.0f, 1.0f);
    if (show_modern_aux) {
        draw_aux_panels_and_pedestal(air_r, air_g, air_b, daylight, live_state);
    }
    if (has_view_[ViewLeft] || has_view_[ViewRight]) {
        draw_side_consoles(1.0f, 1.0f, 1.0f, show_modern_aux);
    }
    if (has_view_[ViewRear]) {
        draw_rear_cockpit(1.0f, 1.0f, 1.0f);
    }

    // 4. 3D overhead rear canopy bow arch & subtle cyan glass rim glint
    draw_canopy_frame_and_glint(daylight);
}

} // namespace falcon
