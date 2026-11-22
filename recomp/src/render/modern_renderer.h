#pragma once

#include <SDL3/SDL_opengl.h>

#include "render/cockpit_3d.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace falcon {

enum class RenderMode : int {
    ClassicVga = 0,
    Modern3D = 1,
    HybridCockpit = 2,
    VrStereoPreview = 3
};

enum class LightingMode : int {
    AutoFromFalcon = 0, // Automatically tracks Falcon 3.0's live Day / Dusk / Night VGA palette!
    Day = 1,
    Dusk = 2,
    Night = 3
};

enum class SmartScalingMode : int {
    Off = 0,          // 1:1 true physical geometry (no scaling)
    Serfoss2003 = 1,  // Classic Falcon BMS / Gary Serfoss (2003) quadratic slant-range curve
    Why485FovRes = 2, // Why485 / Chihirobelmo FOV + Viewport-Resolution-aware Smart Scaling (Recommended)
    LegacyLinear = 3  // Original simple linear distance boost
};

struct WorldEntity {
    double world_x = 0.0;   // North (0.5 ft units)
    double world_y = 0.0;   // East  (0.5 ft units)
    double world_z = 0.0;   // Alt   (0.5 ft units)
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    uint16_t flight_id = 0;
    uint16_t flags = 0;
    uint16_t type_id = 0;   // RAM +0x26 & 0x7FFF
    bool is_player = false;
    bool is_ground = false;
};

struct WorldMissile {
    uint16_t slot_id = 0;
    double world_x = 0.0;   // North (0.5 ft units)
    double world_y = 0.0;   // East  (0.5 ft units)
    double world_z = 0.0;   // Alt   (0.5 ft units)
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    uint16_t age_ticks = 0;
    uint16_t kind_id = 0;
    bool is_sam = false;
};

struct FlightTelemetry {
    bool in_flight = false;
    // Player F-16 6-DOF state (0x47364)
    double world_x = 0.0;    // North (0.5 ft units)
    double world_y = 0.0;    // East  (0.5 ft units)
    float altitude_ft = 5000.0f;
    float agl_ft = 5000.0f;
    float yaw_deg = 0.0f;    // 0..360 (0=N, 90=E, 270=W)
    float pitch_deg = 0.0f;  // -90..+90
    float roll_deg = 0.0f;   // -180..+180
    float airspeed_kts = 0.0f;
    float mach = 0.0f;
    float g_load = 1.0f;

    // Active 3D camera state (0x45C10) — tracks Forward Cockpit, Side/Rear, Padlock, Chase, Orbit, Flyby, Enemy!
    bool has_camera = false;
    bool is_external_cam = false;
    double cam_x = 0.0;
    double cam_y = 0.0;
    float cam_alt_ft = 5000.0f;
    float cam_yaw_deg = 0.0f;
    float cam_pitch_deg = 0.0f;
    float cam_roll_deg = 0.0f;
    int world_y_min = 0;     // Top VGA row of 3D world (0..199)
    int world_y_max = 95;    // Bottom VGA row of 3D world (0..199)

    // Active Falcon 3.0 View Mode (0x13E28): 0=Fwd, 2=Left, 4=Right, 6=Rear, 18=External, 26=Padlock(8)
    uint16_t view_mode = 0;
    bool padlock_active = false;
    bool padlock_has_target = false;
    bool padlock_target_is_missile = false;
    double padlock_target_x = 0.0; // North (0.5 ft units)
    double padlock_target_y = 0.0; // East  (0.5 ft units)
    double padlock_target_z = 0.0; // Alt   (0.5 ft units)
    float padlock_range_nm = 0.0f;
    char padlock_target_name[16]{};

    std::vector<WorldEntity> entities;     // Live aircraft (0x43AB4, stride 0x2C)
    std::vector<WorldEntity> ground_units; // Live ground vehicles, armor, AAA & SAM sites (0x4403E, stride 0x36)
    std::vector<WorldMissile> missiles;    // Active in-flight SAMs & AAMs (0x42170, stride 0x2A)
};

struct VrCameraConfig {
    float fov_deg = 38.6f;          // Matches Falcon 3.0 exact full-screen vertical FOV
    float ipd_mm = 63.0f;
    float head_yaw_deg = 0.0f;
    float head_pitch_deg = 0.0f;
    float terrain_scale = 1.0f;
    float aircraft_scale = 1.0f;    // Smart Scaling strength multiplier (1.0 = exact Serfoss / Why485 curve)
    SmartScalingMode smart_scaling_mode = SmartScalingMode::Why485FovRes;
    bool smart_scale_ground = true; // Apply non-overlapping Smart Scaling to distant 3D ground structures
    float smooth_rate = 18.0f;      // 60/120+ FPS telemetry smoothing rate (0 = off)
    float hud_cy_trim_px = 0.0f;    // Optional vertical VGA pixel trim for HUD/TD box alignment
    bool match_falcon_fov = true;   // Lock camera frustum to Falcon 3.0 HUD projection (1:1 alignment!)
    bool use_original_hud = true;   // Composite live original Falcon 3.0 green HUD (all real avionics!)
    bool sync_hud_no_lag = true;    // Zero-lag lock to raw camera when Original HUD/Cockpit is overlaid (keeps TD box dead-centered during rolls!)
    bool show_vector_hud = false;   // Optional custom OpenGL vector HUD
    bool show_clouds = true;
    bool show_aircraft = true;
    bool show_ground_objects = true; // 3D buildings, hangars, bridges, & ILS runways from MAPOBJ + ILS
    bool show_ground_units = true;   // Live armored columns, AAA & SAM batteries from RAM (0x4403E)
    bool show_missiles_trails = true;// Active SAMs/AAMs + long-range high-visibility smoke plumes & rocket flares
    float missile_trail_lifetime = 11.0f; // Seconds SAM/AAM smoke trails linger in the sky
    bool show_explosions = true;     // 3D retro fireballs, SAM/AAM airbursts & kill smoke plumes
    bool show_contrails = true;      // High-altitude twin wingtip condensation trails (>= 20,000 ft MSL)
    bool show_roads_rivers = true;   // Tigris/Euphrates rivers & Kuwait/Iraq highway network from MAPOBJ.*
    bool retro_desert_facets = true; // Subtle flat-shaded low-poly desert tiles for depth perception
    bool low_alt_sand_dots = true;   // Classic retro low-altitude desert stipple / scrub dots (<6500 ft)
    bool horizon_fog = true;         // Authentic Falcon 3.0 dusty desert distance haze (GL_FOG)
    bool ground_shadows = true;      // Flat retro cloud & aircraft ground shadows
    bool wireframe_terrain = false;
    bool enhanced_3d_padlock = true; // Unclamped 3D Padlock (8) inside full 360-deg pixel-art 3D F-16C cockpit + X-Ray Red Box!
    bool keep_padlock_top_strip = true; // Keep Falcon 3.0's compact top Padlock info bar (y=0..34)
    bool virtual_cockpit_freelook = true; // Render full 3D pixel-art F-16C cockpit during mouse/head free-look too!
    bool hybrid_fill_widescreen = true;   // Hybrid Mode: expand raw 3D world across full 16:9 window around 1:1 4:3 2D cockpit!
    bool widescreen_3d_cockpit = true;    // Modern 3D Mode: full 16:9 3D Virtual F-16 Cockpit (uncheck for pure HUD-only view)
    float widescreen_fov_deg = 48.0f;     // Vertical FOV for Modern 3D Widescreen F-16 Cockpit (40..64 deg)
    LightingMode lighting_mode = LightingMode::AutoFromFalcon;
};

class ModernRenderer {
public:
    ModernRenderer();
    ~ModernRenderer();

    // Load Falcon 3.0 native 3D theater files from game_data/
    bool load_theater_map(const std::string& map_path);
    bool load_isldat(const std::string& isldat_path);
    bool load_alt_terrain(const std::string& alt_path);
    bool load_objects(const std::string& objects_path);
    bool load_mapobj_models(const std::string& mapobj_path);
    bool load_mapobj_world(const std::string& mapobj_irq_path);
    bool load_ils_runways(const std::string& ils_path);
    bool load_cloud_bitmap(const std::string& alt_path) { return load_alt_terrain(alt_path); }

    // Update live 256-color VGA DAC palette so 3D models & terrain use authentic in-game colors
    void set_vga_palette(const uint8_t* dac_768);

    // Smoothly interpolate raw DOS flight telemetry to full 60/120/144 FPS host refresh rate
    void update_telemetry(const FlightTelemetry& raw, float dt_sec);

    // Render 3D scene into the given OpenGL viewport
    // cy_vga: optical horizon row in 0..200 VGA space (47 = Forward Cockpit, 85 = Chase, 100 = centered, 117 = Padlock)
    void render_3d_view(int x, int y, int w, int h, const FlightTelemetry& tel,
                        float eye_offset_m = 0.0f, float cy_vga = 100.0f, bool vga_frustum = false,
                        bool widescreen_cockpit = false, float vga_frustum_aspect = 4.0f / 3.0f);

    // Render crisp high-res OpenGL vector HUD overlay (optional fallback)
    void render_vector_hud(int x, int y, int w, int h, const FlightTelemetry& tel);

    // Render according to selected RenderMode
    void render_frame(int win_w, int win_h, GLuint vga_tex, GLuint vga_cockpit_tex, GLuint vga_hud_tex,
                      const FlightTelemetry& raw_tel, float dt_sec);

    // Spawn 3D combat countermeasures & tracer effects (world-space physics behind/ahead of F-16C!)
    void spawn_chaff(const FlightTelemetry& tel);
    void spawn_flare(const FlightTelemetry& tel);
    void spawn_gun_tracer(const FlightTelemetry& tel);

    // 360-degree Retro Pixel-Art 3D Virtual Cockpit (receives 4 harvested native sprites from DosMachine)
    Cockpit3D& cockpit_3d() { return cockpit_3d_; }
    const Cockpit3D& cockpit_3d() const { return cockpit_3d_; }

    RenderMode& mode() { return mode_; }
    RenderMode mode() const { return mode_; }
    VrCameraConfig& vr_config() { return vr_cfg_; }
    const VrCameraConfig& vr_config() const { return vr_cfg_; }
    const std::string& loaded_theater() const { return loaded_theater_; }
    const FlightTelemetry& smoothed_telemetry() const { return smooth_tel_; }
    size_t isldat_face_count() const { return isldat_tris_.size() / 3; }
    size_t loaded_model_count() const { return models_.size(); }
    size_t ground_object_count() const { return map_structures_.size(); }
    size_t road_leaf_count() const { return map_features_.size(); }
    float current_daylight() const { return lit_.daylight; }
    float current_night_amount() const { return lit_.night_amount; }
    float current_sunset_amount() const { return lit_.sunset_amount; }
    float current_display_ppd() const { return last_display_ppd_; }
    float compute_smart_scale(float slant_km, bool is_ground) const;
    std::string entity_type_name(uint16_t type_id, bool is_ground = false) const;

private:
    static constexpr int kSkyBandCount = 14; // Exact Falcon 3.0 ALTPAL sky+horizon ramp: 204,203..200,194..186
    struct SceneLighting {
        float daylight = 1.0f;      // 1.0 = noon, 0.45 = dusk, 0.14 = moonlit night
        float night_amount = 0.0f;  // 0.0 = day, 1.0 = full night (stars, runway lights, city glow)
        float sunset_amount = 0.0f; // 0.0 = neutral day/night, 1.0 = vivid Falcon 3.0 sunset/dawn warmth
        float sky_bands[kSkyBandCount][3]{}; // [0]=horizon(204) .. [13]=zenith(186)
        float sky_top[3]     = {0.553f, 0.600f, 0.949f};
        float sky_mid[3]     = {0.698f, 0.714f, 0.965f};
        float horizon[3]     = {0.682f, 0.616f, 0.584f};
        float ground_fill[3] = {0.660f, 0.580f, 0.500f};
        float ground_mul[3]  = {1.0f, 1.0f, 1.0f};
        float water_mul[3]   = {1.0f, 1.0f, 1.0f};
        float cloud_rgb[3]   = {1.0f, 1.0f, 0.933f};
    };
    SceneLighting lit_{};
    bool has_smooth_chase_player_ = false;
    float smooth_chase_ex_ = 0.0f, smooth_chase_ey_ = 0.0f, smooth_chase_ez_ = 0.0f;
    float last_dt_sec_ = 0.016f;
    float last_display_ppd_ = 28.0f; // Live viewport vertical pixels per degree (viewport_h / vfov_deg)

    void update_scene_lighting();
    void draw_night_stars();
    struct IslTriVert {
        double wx = 0.0; // North (0.5 ft units)
        double wy = 0.0; // East  (0.5 ft units)
    };

    struct ObjVec3 {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };
    struct ObjFace {
        uint8_t color_idx = 0;
        uint8_t layer_idx = 0;
        ObjVec3 normal{};
        std::vector<uint16_t> verts;
    };
    struct ObjModel {
        std::string name;
        bool is_ground_decal = false; // Flat ground-surface model (runways 26..49, taxiways 103, pads)
        std::vector<ObjVec3> verts;
        std::vector<ObjFace> faces;
    };

    RenderMode mode_ = RenderMode::ClassicVga;
    VrCameraConfig vr_cfg_;

    // Fallback strategic map (KUWAIT.MAP)
    int map_w_ = 0;
    int map_h_ = 0;
    std::vector<uint8_t> map_heights_;

    // Native 1:1 Falcon 3.0 3D terrain & objects:
    // 1. ISLDAT.* layered polygons (w1==1 water, w1==0 land/islands over water, w1==2 grey urban/city polygons)
    std::vector<IslTriVert> isldat_tris_;       // w1 == 1: Water (bays, channels, lakes)
    std::vector<IslTriVert> isldat_land_tris_;  // w1 == 0: Land / Islands (e.g. Bubiyan & Failaka Islands!)
    std::vector<IslTriVert> isldat_urban_tris_; // w1 == 2: Urban / City / Concrete aprons (Kuwait City, airbases)
    // 2. ALT.* 128x128 cloud density (0..3) + 32x32 tile index table + 16x16 elevation tiles
    std::vector<uint8_t> cloud_grid_;
    std::array<uint16_t, 32 * 32> alt_tile_grid_{};
    std::vector<uint8_t> alt_tile_data_; // N * 256 bytes starting at ALT.*[0x1800]
    int alt_tile_count_ = 0;
    bool has_alt_terrain_ = false;
    // 3. game_data/objects 3D aircraft/vehicle models & game_data/mapobj 3D ground building models
    std::vector<ObjModel> models_;
    std::vector<ObjModel> mapobj_models_;

    // 4. MAPOBJ.* road/river features & placed 3D ground structures + ILS.* runways
    struct MapFeature {
        double wx = 0.0;
        double wy = 0.0;
        uint8_t lo_mask = 0;
        uint8_t hi_mask = 0;
    };
    struct RoadSegment {
        double wx0 = 0.0, wy0 = 0.0;
        double wx1 = 0.0, wy1 = 0.0;
        uint8_t kind = 0; // 0 = road/highway, 1 = river (hi & 0x40), 2 = railroad / range boundary (hi & 0x80)
    };
    struct MapStructure {
        double wx = 0.0;
        double wy = 0.0;
        uint8_t model_id = 0;
        float yaw_deg = 0.0f;
    };
    struct IlsRunway {
        double wx = 0.0;
        double wy = 0.0;
        float alt_ft = 0.0f;
        float heading_deg = 0.0f;
        bool has_mapobj_runway = false;
    };
    void sync_ils_mapobj_coverage();
    std::vector<MapFeature> map_features_;
    std::vector<RoadSegment> road_segments_;
    std::vector<MapStructure> map_structures_;
    std::vector<IlsRunway> ils_runways_;

    // Live VGA DAC palette (RGB 0..1) + raw 6-bit DAC for live 3D Cockpit sprite re-paletting
    std::array<float, 256 * 3> vga_rgb_{};
    std::array<uint8_t, 768> vga_dac_raw_{};
    bool has_vga_pal_ = false;
    Cockpit3D cockpit_3d_;

    // Smoothed 3D Padlock head angles (world-space yaw/pitch so transitions never snap violently)
    bool has_smooth_padlock_ = false;
    float smooth_pad_yaw_ = 0.0f;
    float smooth_pad_pitch_ = 0.0f;
    float smooth_pad_roll_ = 0.0f;

    void render_3d_cockpit_pass(int x, int y, int w, int h, const FlightTelemetry& tel,
                                float cam_yaw, float cam_pitch, float cam_roll,
                                float cy_vga, bool vga_frustum,
                                GLuint live_fwd_tex, bool use_live_fwd,
                                bool widescreen_cockpit = false,
                                float vga_frustum_aspect = 4.0f / 3.0f,
                                bool show_modern_aux = false);
    float sim_time_sec_ = 0.0f;
    void draw_padlock_xray_box(int x, int y, int w, int h, const FlightTelemetry& tel,
                               float cam_yaw, float cam_pitch, float cam_roll,
                               float cy_vga, bool vga_frustum, float top_clip_vga_y,
                               float vga_frustum_aspect = 4.0f / 3.0f);
    void draw_native_padlock_strip(int px, int py, int pw, int ph, const FlightTelemetry& tel);
    GLuint padlock_strip_tex_ = 0;

    std::string loaded_theater_ = "None";

    FlightTelemetry smooth_tel_{};
    bool has_smooth_tel_ = false;

    void draw_vga_quad(int x, int y, int w, int h, GLuint tex,
                       float v_min = 0.0f, float v_max = 1.0f, bool alpha_blend = false);
    void draw_hud_overlay_fullscreen(int x, int y, int w, int h, GLuint vga_hud_tex);
    void draw_sky_backdrop(float pitch_deg, float roll_deg, float horizon_ndc_y, float vfov_deg, float aspect);
    void draw_alt_terrain(const FlightTelemetry& tel);
    void draw_retro_desert_facets(const FlightTelemetry& tel);
    void draw_low_alt_sand_dots(const FlightTelemetry& tel);
    void draw_isldat_water(const FlightTelemetry& tel);
    void draw_roads_and_rivers(const FlightTelemetry& tel);
    void draw_ground_structures(const FlightTelemetry& tel);
    void draw_ils_runways(const FlightTelemetry& tel);
    void draw_clouds(const FlightTelemetry& tel);
    struct WorldFx {
        enum class Kind : uint8_t { Chaff = 0, Flare = 1, Tracer = 2 };
        Kind kind = Kind::Flare;
        double wx = 0.0, wy = 0.0, wz = 0.0; // World coords (0.5 ft units)
        double vx = 0.0, vy = 0.0, vz = 0.0; // Velocity (0.5 ft/sec)
        float age = 0.0f;
        float max_age = 3.5f;
        uint32_t seed = 0;
    };
    std::vector<WorldFx> world_fx_;
    uint32_t fx_rng_ = 0x9E3779B9u;

    struct TrailPoint {
        double wx = 0.0, wy = 0.0, wz = 0.0;
        float age = 0.0f;
        float max_age = 11.0f;
        bool is_sam = false;
    };
    struct ActiveMissileTrail {
        uint16_t slot_id = 0;
        double last_wx = 0.0, last_wy = 0.0, last_wz = 0.0;
        float sample_accum = 0.0f;
        bool active_this_frame = false;
        bool exploded = false;
        bool is_sam = false;
        std::vector<TrailPoint> points;
    };
    std::vector<ActiveMissileTrail> missile_trails_;

    struct ExplosionFx {
        double wx = 0.0, wy = 0.0, wz = 0.0;
        float age = 0.0f;
        float max_age = 4.5f;
        float base_radius_km = 0.045f;
        bool is_ground_kill = false;
        uint32_t seed = 0;
    };
    std::vector<ExplosionFx> explosions_;

    struct TrackedUnit {
        uint32_t key = 0;
        double wx = 0.0, wy = 0.0, wz = 0.0;
        bool is_ground = false;
    };
    std::vector<TrackedUnit> prev_combat_units_;
    int combat_track_frames_ = 0;

    struct ContrailPoint {
        double wx = 0.0, wy = 0.0, wz = 0.0;
        float age = 0.0f;
    };
    struct AircraftContrail {
        uint32_t key = 0;
        double last_wx = 0.0, last_wy = 0.0, last_wz = 0.0;
        float sample_accum = 0.0f;
        bool active_this_frame = false;
        std::vector<ContrailPoint> left_pts;
        std::vector<ContrailPoint> right_pts;
    };
    std::vector<AircraftContrail> contrails_;

    void spawn_explosion(double wx, double wy, double wz, float radius_km, bool is_ground_kill);
    void update_world_fx(float dt_sec, bool in_flight);
    void update_missile_trails(const FlightTelemetry& tel, float dt_sec);
    void update_explosions_and_contrails(const FlightTelemetry& tel, float dt_sec);
    void draw_world_fx(const FlightTelemetry& tel);
    void draw_explosions(const FlightTelemetry& tel);
    void draw_contrails(const FlightTelemetry& tel);
    void draw_world_entities(const FlightTelemetry& tel);
    void draw_ground_units(const FlightTelemetry& tel);
    void draw_missiles_and_trails(const FlightTelemetry& tel);
    void draw_hud_digit_string(float x, float y, float scale, const std::string& text);
    bool is_alt_water_at(double wx, double wy) const;
    int resolve_model_index(uint16_t type_id) const;
    int resolve_ground_model_index(uint16_t type_id) const;
};

} // namespace falcon
