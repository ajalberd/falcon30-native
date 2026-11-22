#pragma once

#include <SDL3/SDL_opengl.h>

#include <array>
#include <cstdint>

namespace falcon {

// Full 360-degree Retro Pixel-Art 3D F-16C Virtual Cockpit & Airframe.
// Combines Falcon 3.0's 4 authentic Spectrum HoloByte cockpit sprites (Forward, Left, Right, Rear),
// harvested live from the user's running game (zero copyrighted pixels in repo), with matching
// palette-tinted 1991 pixel-art nose, footwell/floor, side bridges, swept wings, wingtip AIM-9s,
// and vertical tailfin — giving complete 3D spatial orientation in Padlock (8), Free-Look, and VR!
class Cockpit3D {
public:
    static constexpr int kSpriteW = 320;
    static constexpr int kSpriteH = 200;

    enum ViewSlot : int {
        ViewForward = 0,
        ViewLeft = 1,
        ViewRight = 2,
        ViewRear = 3,
        ViewCount = 4
    };

    Cockpit3D() = default;
    ~Cockpit3D();

    // Store raw 320x200 VGA palette-index buffer (0..127 = opaque cockpit, 128..255 = transparent world)
    void set_view_indices(ViewSlot slot, const uint8_t* indices_64k);
    bool has_any_views() const;
    bool has_all_views() const;
    void clear_views();

    // Re-palette harvested sprites whenever Falcon's live VGA DAC changes (Dawn / Day / Sunset / Night)
    void sync_palette(const uint8_t* dac_768);

    struct LiveCockpitState {
        float yaw_deg = 0.0f;
        float pitch_deg = 0.0f;
        float roll_deg = 0.0f;
        float airspeed_kts = 0.0f;
        float agl_ft = 5000.0f;
        float g_load = 1.0f;
        float sim_time_sec = 0.0f;
        bool missile_warning = false;
    };

    // Render the complete 3D Virtual Cockpit & Exterior Airframe from the pilot's eye (0,0,0)
    // in aircraft body space (+X Right, +Y Up, -Z Forward, +Z Rear).
    // When live_fwd_cockpit_tex != 0 and use_live_forward is true, the Forward instrument panel
    // & HUD combiner glass display Falcon 3.0's 100% REAL live 2D HUD, MFD radar, RWR, and gauges!
    void render(float daylight, const float ground_mul[3],
                GLuint live_fwd_cockpit_tex = 0, bool use_live_forward = false,
                const LiveCockpitState* live_state = nullptr,
                bool show_modern_aux = false);

private:
    void ensure_gl_resources();
    void upload_view_texture(int slot, const uint8_t* dac_768);
    void build_procedural_textures();

    void draw_forward_panel(GLuint tex, float r, float g, float b);
    void draw_aux_panels_and_pedestal(float r, float g, float b, float daylight, const LiveCockpitState* st);
    void draw_side_consoles(float r, float g, float b, bool wide_modern_layout);
    void draw_rear_cockpit(float r, float g, float b);
    void draw_tub_floor_and_bridges(float r, float g, float b, bool wide_modern_layout);
    void draw_exterior_airframe(float r, float g, float b, float daylight, float sim_time_sec);
    void draw_canopy_frame_and_glint(float daylight);

    std::array<std::array<uint8_t, kSpriteW * kSpriteH>, ViewCount> raw_idx_{};
    std::array<bool, ViewCount> has_view_{};
    std::array<bool, ViewCount> dirty_view_{};
    std::array<GLuint, ViewCount> view_tex_{};

    std::array<uint8_t, 768> last_dac_{};
    bool has_dac_ = false;

    // Matching 1991 Spectrum HoloByte pixel-art procedural textures (GL_NEAREST)
    bool gl_ready_ = false;
    GLuint tex_floor_ = 0;
    GLuint tex_nose_ = 0;
    GLuint tex_wing_ = 0;
    GLuint tex_bulkhead_ = 0;
    GLuint tex_left_aux_ = 0;
    GLuint tex_right_aux_ = 0;
    GLuint tex_pedestal_ = 0;
};

} // namespace falcon
