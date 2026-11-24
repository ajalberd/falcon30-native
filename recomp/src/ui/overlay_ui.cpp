#include "ui/overlay_ui.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace falcon {

namespace {
const char* hat_dir_name(uint8_t mask) {
    if (mask & SDL_HAT_UP) return "UP";
    if (mask & SDL_HAT_DOWN) return "DOWN";
    if (mask & SDL_HAT_LEFT) return "LEFT";
    if (mask & SDL_HAT_RIGHT) return "RIGHT";
    return "?";
}
} // namespace

OverlayUI::OverlayUI(DosMachine& machine, AudioEngine& audio, HotasMapper& hotas,
                     NetUdp& net, ModernRenderer& renderer, F16FlightModel& flight_model,
                     HeadTracker& head_tracker, std::function<bool()> save_settings_cb,
                     std::function<void()> open_launcher_cb, bool* show_launcher_ptr,
                     TimeCompSettings* time_comp_ptr)
    : machine_(machine), audio_(audio), hotas_(hotas), net_(net),
      renderer_(renderer), flight_model_(flight_model), head_tracker_(head_tracker),
      save_settings_cb_(std::move(save_settings_cb)),
      open_launcher_cb_(std::move(open_launcher_cb)),
      show_launcher_ptr_(show_launcher_ptr),
      time_comp_ptr_(time_comp_ptr) {
    std::snprintf(rom_dir_buf_, sizeof(rom_dir_buf_), "%s", audio_.config().cm32l_rom_dir.c_str());
}

void OverlayUI::draw(const FlightTelemetry& tel, double now_sec) {
    now_sec_ = now_sec;

    // Always draw crisp corner Time Compression pill when active (>1x) or on brief Auto-Drop alert (even if F12 is hidden!)
    {
        float ts = tel.in_flight ? machine_.time_scale() : 1.0f;
        bool show_alert = (time_comp_ptr_ && !time_comp_ptr_->alert_text.empty() && now_sec < time_comp_ptr_->alert_until_sec);
        if (ts > 1.01f || show_alert) {
            ImGuiIO& io = ImGui::GetIO();
            float top_y = visible_ ? 28.0f : 10.0f;
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 14.0f, top_y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.08f, 0.12f, 0.82f));
            ImGui::PushStyleColor(ImGuiCol_Border, ts > 1.01f ? ImVec4(0.25f, 0.95f, 0.48f, 0.90f) : ImVec4(0.98f, 0.72f, 0.20f, 0.95f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 5.0f));
            ImGuiWindowFlags pf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            if (ImGui::Begin("##TimeCompBadge", nullptr, pf)) {
                if (ts > 1.01f) {
                    const char* hint = (time_comp_ptr_ && time_comp_ptr_->mode == TimeCompMode::HoldToBoost)
                                           ? "Release TAB -> 1x" : "TAB cycle | Shift+TAB down";
                    ImGui::TextColored(ImVec4(0.30f, 0.98f, 0.50f, 1.0f), "TIME %.0fx", ts);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", hint);
                } else if (show_alert) {
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "%s", time_comp_ptr_->alert_text.c_str());
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }

    if (!visible_) return;

    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.45f, 1.0f), "FALCON 3.0 NATIVE");
        ImGui::Separator();

        if (ImGui::BeginMenu("Title")) {
            if (ImGui::MenuItem("Falcon 3.0 (F-16C)", nullptr, machine_.current_exe() == "FALCONCD.EXE")) {
                machine_.load_exe(machine_.game_dir(), "FALCONCD.EXE");
            }
            if (ImGui::MenuItem("Hornet: Naval Strike (F/A-18)", nullptr, machine_.current_exe() == "HORNETCD.EXE")) {
                machine_.load_exe(machine_.game_dir(), "HORNETCD.EXE");
            }
            if (ImGui::MenuItem("MiG-29: Deadly Adversary", nullptr, machine_.current_exe() == "MIGCD.EXE")) {
                machine_.load_exe(machine_.game_dir(), "MIGCD.EXE");
            }
            if (open_launcher_cb_ && ImGui::MenuItem("Return to Trilogy Launcher...")) {
                open_launcher_cb_();
            }
            if (time_comp_ptr_ && ImGui::BeginMenu("Time Compression (TAB)")) {
                bool is_tap  = (time_comp_ptr_->mode == TimeCompMode::TapToCycle);
                bool is_hold = (time_comp_ptr_->mode == TimeCompMode::HoldToBoost);
                if (ImGui::MenuItem("Tap TAB to Cycle (1x / 2x / 4x / 8x)", nullptr, is_tap)) {
                    time_comp_ptr_->mode = TimeCompMode::TapToCycle;
                }
                if (ImGui::MenuItem("Hold TAB for Smooth Boost", nullptr, is_hold)) {
                    time_comp_ptr_->mode = TimeCompMode::HoldToBoost;
                    machine_.set_time_scale(1.0f);
                }
                if (is_hold) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Hold Boost: 2x", nullptr, time_comp_ptr_->hold_mult == 2.0f)) time_comp_ptr_->hold_mult = 2.0f;
                    if (ImGui::MenuItem("Hold Boost: 4x", nullptr, time_comp_ptr_->hold_mult == 4.0f)) time_comp_ptr_->hold_mult = 4.0f;
                    if (ImGui::MenuItem("Hold Boost: 8x", nullptr, time_comp_ptr_->hold_mult == 8.0f)) time_comp_ptr_->hold_mult = 8.0f;
                }
                ImGui::Separator();
                ImGui::MenuItem("Auto-Drop to 1x on Missile / Low Alt / Fire", nullptr, &time_comp_ptr_->auto_drop);
                ImGui::Separator();
                float cur = machine_.time_scale();
                if (ImGui::MenuItem("Set 1x (Normal)", nullptr, cur < 1.5f)) machine_.set_time_scale(1.0f);
                if (ImGui::MenuItem("Set 2x", nullptr, cur >= 1.5f && cur < 3.0f)) machine_.set_time_scale(2.0f);
                if (ImGui::MenuItem("Set 4x", nullptr, cur >= 3.0f && cur < 6.0f)) machine_.set_time_scale(4.0f);
                if (ImGui::MenuItem("Set 8x", nullptr, cur >= 6.0f)) machine_.set_time_scale(8.0f);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (show_launcher_ptr_) {
                ImGui::MenuItem("Show Launcher on Startup", nullptr, show_launcher_ptr_);
            }
            bool skip = machine_.skip_intro();
            if (ImGui::MenuItem("Skip Intro Video on Launch", nullptr, &skip)) {
                machine_.set_skip_intro(skip);
            }
            if (save_settings_cb_ && ImGui::MenuItem("Save Settings Now (falcon3_config.json)")) {
                config_status_ = save_settings_cb_() ? "Saved falcon3_config.json!" : "Save failed!";
            }
            if (ImGui::MenuItem("Restart Current Title")) {
                machine_.load_exe(machine_.game_dir(), machine_.current_exe());
            }
            ImGui::EndMenu();
        }

        ImGui::MenuItem("3D / VR Renderer", nullptr, &show_render_);
        ImGui::MenuItem("Flight Model Mod", nullptr, &show_flight_model_);
        ImGui::MenuItem("HOTAS Mapper", nullptr, &show_hotas_);
        ImGui::MenuItem("Audio", nullptr, &show_audio_);
        ImGui::MenuItem("Multiplayer (UDP)", nullptr, &show_multiplayer_);
        ImGui::MenuItem("RE Debugger", nullptr, &show_debugger_);
        ImGui::Separator();
        ImGui::TextDisabled("[F9] Recenter Head | [F11] Render Mode | [F12] Overlay");
        ImGui::EndMainMenuBar();
    }

    if (show_render_) draw_render_vr_window(tel);
    if (show_flight_model_) draw_flight_model_window(tel);
    if (show_hotas_) draw_hotas_window();
    if (show_audio_) draw_audio_window();
    if (show_multiplayer_) draw_multiplayer_window();
    if (show_debugger_) draw_debugger_window();
}

void OverlayUI::draw_render_vr_window(const FlightTelemetry& tel) {
    ImGui::SetNextWindowSize(ImVec2(410, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("3D / VR Renderer & Live Telemetry", &show_render_)) {
        ImGui::End();
        return;
    }

    int m = static_cast<int>(renderer_.mode());
    const char* modes[] = {
        "Classic VGA (320x200 Mode X)",
        "Modern 3D HD Retro + Original HUD",
        "Hybrid (HD Retro 3D + Full Cockpit/HUD)",
        "VR Stereo Preview (Side-by-Side)"
    };
    if (ImGui::Combo("Render Mode", &m, modes, 4)) {
        renderer_.mode() = static_cast<RenderMode>(m);
    }

    auto& vr = renderer_.vr_config();
    ImGui::Checkbox("Match Falcon Exact FOV (1:1 HUD Alignment)", &vr.match_falcon_fov);
    if (!vr.match_falcon_fov) {
        ImGui::SliderFloat("Custom FOV (deg)", &vr.fov_deg, 30.0f, 110.0f, "%.1f");
    }
    ImGui::Checkbox("Original Falcon HUD", &vr.use_original_hud);
    ImGui::SameLine();
    ImGui::Checkbox("Zero-Lag HUD Sync", &vr.sync_hud_no_lag);
    ImGui::Checkbox("Simple Vector HUD", &vr.show_vector_hud);
    ImGui::Checkbox("3D Aircraft & Objects", &vr.show_aircraft);
    ImGui::SameLine();
    ImGui::Checkbox("Clouds", &vr.show_clouds);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &vr.wireframe_terrain);
    ImGui::Checkbox("3D Buildings & Runways", &vr.show_ground_objects);
    ImGui::SameLine();
    ImGui::Checkbox("Roads & Rivers", &vr.show_roads_rivers);
    ImGui::Checkbox("Ground Vehicles & SAM Sites", &vr.show_ground_units);
    ImGui::SameLine();
    ImGui::Checkbox("SAM/AAM Missiles & Smoke Trails", &vr.show_missiles_trails);
    if (vr.show_missiles_trails) {
        ImGui::SliderFloat("Missile Smoke Trail Lifetime", &vr.missile_trail_lifetime, 3.0f, 20.0f, "%.0f sec");
    }
    ImGui::Checkbox("3D Explosions & Fireballs", &vr.show_explosions);
    ImGui::SameLine();
    ImGui::Checkbox("High-Alt Contrails (>=20k ft)", &vr.show_contrails);
    ImGui::Checkbox("Retro Desert Facets", &vr.retro_desert_facets);
    ImGui::SameLine();
    ImGui::Checkbox("Low-Alt Sand Dots", &vr.low_alt_sand_dots);
    ImGui::Checkbox("Horizon Fog", &vr.horizon_fog);
    ImGui::SameLine();
    ImGui::Checkbox("Ground Shadows", &vr.ground_shadows);
    {
        int lm = static_cast<int>(vr.lighting_mode);
        const char* lm_names[] = {
            "Auto (Match Falcon Day/Sunset/Dawn/Night)",
            "Force Day",
            "Force Sunset / Dusk (Kuwait ALTPAL)",
            "Force Night (Stars + Runway Lights)"
        };
        if (ImGui::Combo("Lighting / Time-of-Day", &lm, lm_names, 4)) {
            vr.lighting_mode = static_cast<LightingMode>(lm);
        }
        if (renderer_.current_night_amount() > 0.35f) {
            ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f),
                               "NIGHT / DUSK LIGHTING ACTIVE (Daylight: %.0f%% | Stars & Runway Lights ON)",
                               renderer_.current_daylight() * 100.0f);
        } else if (renderer_.current_sunset_amount() > 0.30f) {
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.56f, 1.0f),
                               "SUNSET / DAWN GRADIENT ACTIVE (Live 14-Band ALTPAL Sky & Shading)");
        }
    }
    {
        int sm = static_cast<int>(vr.smart_scaling_mode);
        const char* sm_names[] = {
            "Off (1:1 True Scale)",
            "Serfoss 2003 (Falcon BMS Classic)",
            "Why485 / Chihirobelmo (FOV + Resolution Aware)",
            "Legacy Linear Boost"
        };
        if (ImGui::Combo("Smart Scaling", &sm, sm_names, 4)) {
            vr.smart_scaling_mode = static_cast<SmartScalingMode>(sm);
        }
        if (vr.smart_scaling_mode != SmartScalingMode::Off) {
            ImGui::Checkbox("Apply Smart Scaling to Ground Units / Buildings", &vr.smart_scale_ground);
            ImGui::SliderFloat("Smart Scaling Strength", &vr.aircraft_scale, 0.5f, 2.5f, "%.2fx");
            ImGui::TextDisabled("Display: %.1f px/deg (vs 60 eye) | Air 1NM=%.2fx 3NM=%.2fx 5NM=%.2fx | Gnd 3NM=%.2fx",
                                renderer_.current_display_ppd(),
                                renderer_.compute_smart_scale(1.852f, false),
                                renderer_.compute_smart_scale(5.556f, false),
                                renderer_.compute_smart_scale(9.260f, false),
                                renderer_.compute_smart_scale(5.556f, true));
        }
    }
    ImGui::SliderFloat("HUD Vertical Trim (VGA px)", &vr.hud_cy_trim_px, -12.0f, 12.0f, "%+.1f px");
    ImGui::SliderFloat("Terrain Relief", &vr.terrain_scale, 0.5f, 3.0f, "%.2f");
    ImGui::SliderFloat("Camera Smoothing (60+ FPS)", &vr.smooth_rate, 0.0f, 35.0f, "%.1f");
    ImGui::SliderFloat("VR IPD (mm)", &vr.ipd_mm, 52.0f, 74.0f, "%.1f");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f), "360-deg 3D Virtual Cockpit & Unclamped Padlock (8)");
    ImGui::Checkbox("Hybrid Mode: Fill Widescreen 3D World Around 4:3 Cockpit", &vr.hybrid_fill_widescreen);
    ImGui::Checkbox("Modern 3D Mode: Full 16:9 3D F-16 Cockpit (Uncheck = HUD-Only)", &vr.widescreen_3d_cockpit);
    if (vr.widescreen_3d_cockpit) {
        ImGui::SliderFloat("Modern 3D Cockpit FOV", &vr.widescreen_fov_deg, 40.0f, 64.0f, "%.1f deg");
    }
    ImGui::Checkbox("Enhanced 3D Padlock (8 = Lock/Cycle, 3 = Forward, Live 2D HUD + X-Ray)", &vr.enhanced_3d_padlock);
    ImGui::SameLine();
    ImGui::Checkbox("Keep Top Padlock Strip", &vr.keep_padlock_top_strip);
    ImGui::Checkbox("3D Virtual Cockpit Free-Look (Right-Mouse Drag; Mid-Click / '3' Recenters)", &vr.virtual_cockpit_freelook);
    ImGui::TextDisabled("3D Cockpit Native Sprites: %s",
                        renderer_.cockpit_3d().has_all_views() ? "ALL 4 VIEWS READY (Fwd/Left/Right/Rear)"
                        : (renderer_.cockpit_3d().has_any_views() ? "Partial" : "Waiting for flight..."));
    ImGui::SliderFloat("Head Look Yaw", &vr.head_yaw_deg, -175.0f, 175.0f, "%.1f");
    ImGui::SliderFloat("Head Look Pitch", &vr.head_pitch_deg, -68.0f, 82.0f, "%.1f");
    if (ImGui::SmallButton("Recenter Head (F9)")) {
        vr.head_yaw_deg = 0.0f;
        vr.head_pitch_deg = 0.0f;
        head_tracker_.recenter();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.45f, 0.88f, 1.0f, 1.0f), "OpenTrack / TrackIR Head Tracking (UDP)");
    {
        auto& htc = head_tracker_.config();
        bool ot_conn = head_tracker_.is_connected(now_sec_);
        ImGui::Checkbox("Enable OpenTrack UDP Receiver", &htc.enabled);
        ImGui::SameLine();
        if (!htc.enabled) {
            ImGui::TextDisabled("[Disabled]");
        } else if (ot_conn) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "[CONNECTED: Yaw %+.1f | Pitch %+.1f]",
                               head_tracker_.yaw_deg(), head_tracker_.pitch_deg());
        } else if (head_tracker_.is_listening()) {
            ImGui::TextDisabled("[Listening on UDP %d]", htc.udp_port);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "[Bind failed on UDP %d]", htc.udp_port);
        }
        if (htc.enabled) {
            ImGui::SetNextItemWidth(110.0f);
            ImGui::InputInt("UDP Port", &htc.udp_port);
            if (htc.udp_port < 1024) htc.udp_port = 1024;
            if (htc.udp_port > 65535) htc.udp_port = 65535;
            ImGui::SameLine();
            if (ImGui::SmallButton("Recenter OpenTrack")) {
                head_tracker_.recenter();
            }
            ImGui::SliderFloat("TrackIR Yaw Scale", &htc.yaw_scale, 0.3f, 3.0f, "%.2fx");
            ImGui::SliderFloat("TrackIR Pitch Scale", &htc.pitch_scale, 0.3f, 3.0f, "%.2fx");
            ImGui::Checkbox("Invert Yaw", &htc.invert_yaw);
            ImGui::SameLine();
            ImGui::Checkbox("Invert Pitch", &htc.invert_pitch);
        }
    }
    if (!config_status_.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s", config_status_.c_str());
    }

    ImGui::Separator();
    ImGui::Text("Host FPS: %.1f | Active Sim Speed: %.1f MIPS%s",
                ImGui::GetIO().Framerate,
                static_cast<float>(machine_.effective_cpu_speed_ips()) / 1000000.0f,
                (machine_.auto_turbo() && tel.in_flight) ? " [FLIGHT TURBO]" : "");
    bool turbo = machine_.auto_turbo();
    if (ImGui::Checkbox("Auto-Turbo CPU in Flight (20 MIPS, Wall-Clock Physics)", &turbo)) {
        machine_.set_auto_turbo(turbo);
    }
    float flt_mips = static_cast<float>(machine_.flight_cpu_speed_ips()) / 1000000.0f;
    if (ImGui::SliderFloat("Flight CPU Speed (MIPS)", &flt_mips, 4.0f, 35.0f, "%.1f MIPS")) {
        machine_.set_flight_cpu_speed_ips(static_cast<uint64_t>(flt_mips * 1000000.0f));
    }

    ImGui::Separator();
    ImGui::Text("Theater: %s | Buildings: %zu | Roads: %zu | Models: %zu",
                renderer_.loaded_theater().c_str(), renderer_.ground_object_count(),
                renderer_.road_leaf_count(), renderer_.loaded_model_count());
    struct TheaterDef { const char* map; const char* ext; };
    const TheaterDef theaters[] = {
        {"KUWAIT.MAP", "IRQ"}, {"BOSNIA.MAP", "BOS"}, {"ISRAEL.MAP", "ISR"},
        {"KOREA.MAP", "KOR"}, {"PANAMA.MAP", "PAN"}, {"NELLIS.MAP", "NEL"},
        {"JAPAN.MAP", "HOK"}, {"INDIA.MAP", "PAK"}
    };
    if (ImGui::BeginCombo("Switch Theater", renderer_.loaded_theater().c_str())) {
        for (const auto& th : theaters) {
            if (ImGui::Selectable(th.map, renderer_.loaded_theater() == th.map)) {
                fs::path dir = machine_.game_dir();
                renderer_.load_theater_map((dir / th.map).string());
                renderer_.load_alt_terrain((dir / (std::string("ALT.") + th.ext)).string());
                renderer_.load_isldat((dir / (std::string("ISLDAT.") + th.ext)).string());
                renderer_.load_mapobj_world((dir / (std::string("MAPOBJ.") + th.ext)).string());
                renderer_.load_ils_runways((dir / (std::string("ILS.") + th.ext)).string());
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    if (tel.in_flight) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                           "LIVE TELEMETRY | %s | Air: %zu | Gnd: %zu | Missiles: %zu",
                           tel.is_external_cam ? "EXTERNAL CAM" : "COCKPIT CAM",
                           tel.entities.size(), tel.ground_units.size(), tel.missiles.size());
        ImGui::Text("ALT: %.0f ft MSL (%.0f ft AGL) | KIAS: %.0f kts (M %.2f)",
                    tel.altitude_ft, tel.agl_ft, tel.airspeed_kts, tel.mach);
        ImGui::Text("HDG: %.1f deg | PITCH: %+.1f deg | ROLL: %+.1f deg",
                    tel.yaw_deg, tel.pitch_deg, tel.roll_deg);
        ImGui::Text("CAM (0x45C10): HDG %.1f | PITCH %+.1f | ROLL %+.1f | VIEW=%u | Y=[%d..%d]",
                    tel.cam_yaw_deg, tel.cam_pitch_deg, tel.cam_roll_deg, tel.view_mode, tel.world_y_min, tel.world_y_max);
        if (tel.padlock_active) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                               "PADLOCK (8): %s | Range: %.2f NM %s",
                               tel.padlock_target_name[0] ? tel.padlock_target_name : "ACQUIRING",
                               tel.padlock_range_nm,
                               tel.padlock_target_is_missile ? "[MISSILE/SAM]" : "");
        }
    } else {
        ImGui::TextDisabled("Live Telemetry: Waiting for 3D cockpit flight...");
    }

    ImGui::End();
}

void OverlayUI::draw_flight_model_window(const FlightTelemetry& tel) {
    ImGui::SetNextWindowSize(ImVec2(430, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("F-16C Flight Model Mod (NASA TP-1538 + FLCS)", &show_flight_model_)) {
        ImGui::End();
        return;
    }

    auto& cfg = flight_model_.config();
    bool was_enabled = cfg.enabled;
    if (ImGui::Checkbox("Enable Native F-16C Block 30/40 Flight Model", &cfg.enabled)) {
        if (!was_enabled && cfg.enabled) {
            flight_model_.reset(); // Re-sync cleanly from Falcon's current position & speed
        }
    }
    ImGui::TextWrapped("Replaces Falcon 3.0's 1991 flight physics with a 240 Hz C++ 6-DOF F-16C model "
                       "(NASA TP-1538 wind-tunnel aero + 1G auto-trim FLCS + 25.5 deg AOA limiter), "
                       "keeping Falcon's real HUD, radar, weapons, AI & campaign 100%% intact.");

    ImGui::Separator();
    int eng = cfg.engine_f110_ge100 ? 0 : 1;
    const char* eng_names[] = {
        "GE F110-GE-100 (Block 30/40 — 28,600 lbf AB)",
        "P&W F100-PW-220 (Block 25/32 — 23,770 lbf AB)"
    };
    if (ImGui::Combo("Engine", &eng, eng_names, 2)) {
        cfg.engine_f110_ge100 = (eng == 0);
    }
    ImGui::Checkbox("FLCS CAT III Limiter (5.5G / 18 deg AOA)", &cfg.cat3_limiter);
    ImGui::Checkbox("FLCS 25.5 deg AOA Limiter (Uncheck for MPO Deep-Stall)", &cfg.aoa_limiter);
    ImGui::SliderFloat("Max Roll Rate (deg/s)", &cfg.max_roll_rate_dps, 120.0f, 320.0f, "%.0f");
    ImGui::SliderFloat("Pitch Stick Sensitivity", &cfg.stick_pitch_sens, 0.4f, 1.8f, "%.2f");
    ImGui::SliderFloat("Roll Stick Sensitivity", &cfg.stick_roll_sens, 0.4f, 1.8f, "%.2f");

    ImGui::Separator();
    const auto& st = flight_model_.state();
    if (cfg.enabled && tel.in_flight && st.initialized) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.45f, 1.0f), "NATIVE FLCS ACTIVE (240 Hz, Direct HOTAS + Arrows)");
        ImGui::Text("G-Load: %+.2f G (Max %.1f G) | AOA: %+.1f deg | Beta: %+.1f deg",
                    st.g_load, st.max_g, st.aoa_deg, st.beta_deg);
        ImGui::Text("Turn Rate: %.1f deg/s | Ps (Energy): %+.0f ft/s",
                    st.turn_rate_dps, st.ps_fps);
        ImGui::Text("Thrust: %.0f lbf | Drag: %.0f lbf | Weight: %.0f lbs",
                    st.thrust_lbf, st.drag_lbf, st.weight_lbs);
        ImGui::Text("Throttle: %.0f%% (RPM %.0f%%) | KIAS: %.0f | Mach: %.2f",
                    st.throttle_cmd * 100.0, st.rpm_pct, st.kias, st.mach);
    } else if (cfg.enabled) {
        ImGui::TextDisabled("Armed — waiting for 3D cockpit flight to engage FLCS...");
    } else {
        ImGui::TextDisabled("Inactive — using original Falcon 3.0 flight model.");
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "SAM & Threat Realism Mod (Non-Destructive Live RAM Patch)");
    int sp = static_cast<int>(machine_.sam_mod_preset());
    const char* sam_presets[] = {
        "Vanilla Falcon 3.0 (Broken 80x SA-7 & Super Radar SAMs)",
        "Historical Realistic (F3MSL SA-7B + Real Gulf War Radar SAMs)",
        "Early Cold War (F3MSL SA-7A Strela-2A + Relaxed SAMs)",
        "Training / Low SAM Threat"
    };
    if (ImGui::Combo("SAM Preset", &sp, sam_presets, 4)) {
        machine_.set_sam_mod_preset(static_cast<SamModPreset>(sp));
    }
    SamModStatus sst = machine_.read_sam_mod_status();
    if (sst.table_found) {
        ImGui::Text("SAM (R)  RAM: Burn=%u | Turn=%u | Blast=%u | ECM/CM=%u",
                    sst.sam_r_burn, sst.sam_r_turn, sst.sam_r_blast, sst.sam_r_cm);
        ImGui::Text("SAM (IR) RAM: Burn=%u | Turn=%u | Agi=0x%04X | FlareCM=%u",
                    sst.sam_ir_burn, sst.sam_ir_turn, sst.sam_ir_agi, sst.sam_ir_cm);
    } else {
        ImGui::TextDisabled("Armed — patches missile table automatically as soon as 3D flight overlay loads.");
    }
    ImGui::End();
}

void OverlayUI::draw_multiplayer_window() {
    ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Modern Multiplayer (UDP / IPX Bridge)", &show_multiplayer_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Status:");
    ImGui::SameLine();
    ImGui::TextColored(net_.mode() != NetMode::Offline ? ImVec4(0.3f, 0.95f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "%s", net_.status().c_str());

    ImGui::InputInt("UDP Port", &net_port_);
    ImGui::InputText("Host IP (Join)", join_ip_buf_, sizeof(join_ip_buf_));

    if (ImGui::Button("Host Game")) {
        net_.start_host(static_cast<uint16_t>(net_port_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Join Host")) {
        net_.join_host(join_ip_buf_, static_cast<uint16_t>(net_port_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Send Ping")) {
        net_.send_ping();
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        net_.disconnect();
    }

    ImGui::Separator();
    ImGui::Text("TX Packets: %llu | RX Packets: %llu | Peers: %zu",
                static_cast<unsigned long long>(net_.tx_packets()),
                static_cast<unsigned long long>(net_.rx_packets()),
                net_.peers().size());
    for (const auto& p : net_.peers()) {
        ImGui::BulletText("%s:%u", p.ip.c_str(), p.port);
    }
    ImGui::TextDisabled("In-game Novell IPX (INT 7Ah) automatically bridges over UDP.");
    ImGui::End();
}

void OverlayUI::draw_hotas_window() {
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Multi-HOTAS Mapper", &show_hotas_)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Refresh Devices")) {
        hotas_.refresh_devices();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Profile")) {
        bool ok = hotas_.save_profile("falcon3_hotas.json");
        if (ok && machine_.auto_bypass_joy_cal() && hotas_.has_any_axis_bound()) {
            machine_.apply_auto_joystick_calibration(true);
        }
        profile_status_ = ok ? "Saved falcon3_hotas.json" : "Save failed";
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Profile")) {
        bool ok = hotas_.load_profile("falcon3_hotas.json");
        if (ok && machine_.auto_bypass_joy_cal()) {
            machine_.apply_auto_joystick_calibration(true);
        }
        profile_status_ = ok ? "Loaded falcon3_hotas.json" : "No profile found";
    }
    if (!profile_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", profile_status_.c_str());
    }

    {
        int im = static_cast<int>(machine_.flight_input_mode());
        const char* im_names[] = {
            "Auto Seamless (HOTAS + Keyboard Both Work — Recommended)",
            "Force HOTAS Only (Pre-Calibrated 0x201)",
            "Force Keyboard Only (Arrows + +/- Throttle)"
        };
        if (ImGui::Combo("Flight Input Override", &im, im_names, 3)) {
            auto mode = static_cast<FlightInputMode>(im);
            machine_.set_flight_input_mode(mode);
            if (mode == FlightInputMode::ForceKeyboard) {
                machine_.apply_auto_joystick_calibration(true, 0);
                profile_status_ = "Forced Keyboard Only";
            } else if (hotas_.has_any_axis_bound()) {
                machine_.apply_auto_joystick_calibration(true);
                profile_status_ = (mode == FlightInputMode::AutoSeamless)
                                  ? "Auto Seamless (HOTAS + Keyboard active)" : "Forced HOTAS Only";
            }
        }
        if (ImGui::SmallButton("Save Pre-Calibrated joystick.def")) {
            uint8_t m = hotas_.recommended_gameport_mode();
            machine_.apply_auto_joystick_calibration(true, m > 0 ? m : 1);
            profile_status_ = "Saved pre-calibrated joystick.def + RAM patch!";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Simulate Joy Btn 1")) {
            hotas_.pulse_gameport_button(10);
            profile_status_ = "Pulsed Gameport Button 1";
        }
        uint8_t dos_jm = machine_.active_dos_joystick_mode();
        const char* jm_str = (dos_jm == 0) ? "Keyboard Active" :
                             (dos_jm == 1) ? "HOTAS Stick (Kbd Throttle)" :
                             (dos_jm == 2) ? "HOTAS Stick+Rudder (Kbd Throttle)" :
                             (dos_jm == 3) ? "HOTAS Stick+Throttle" : "4-Axis Full HOTAS";
        ImGui::TextDisabled("Live Control State: %s (Overrides Falcon's 1991 CONFIG screen)", jm_str);
    }

    if (hotas_.is_capturing()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                           "Listening for NEW input delta... (Always-ON toggle switches ignored!)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) hotas_.cancel_capture();
    }

    if (ImGui::BeginTabBar("HotasTabs")) {
        if (ImGui::BeginTabItem("Flight Axes")) {
            auto& bindings = hotas_.axis_bindings();
            for (int i = 0; i < static_cast<int>(FlightAxis::Count); ++i) {
                ImGui::PushID(i);
                auto axis = static_cast<FlightAxis>(i);
                auto& b = bindings[i];
                ImGui::Text("%s", flight_axis_name(axis));
                ImGui::SameLine(160);
                if (b.axis_index >= 0) {
                    std::string short_dev = b.device_name;
                    if (short_dev.size() > 28) short_dev = short_dev.substr(0, 26) + "..";
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "%s [Ax %d]", short_dev.c_str(), b.axis_index);
                } else {
                    ImGui::TextDisabled("Unbound");
                }
                ImGui::SameLine(440);
                if (ImGui::SmallButton(hotas_.capturing_axis() == i ? "Move Axis..." : "Bind")) {
                    hotas_.start_axis_capture(axis);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    b.axis_index = -1;
                    b.device_guid.clear();
                    b.device_name.clear();
                    if (machine_.auto_bypass_joy_cal()) {
                        machine_.apply_auto_joystick_calibration(hotas_.has_any_axis_bound());
                    }
                }

                float val = hotas_.axis_value(axis);
                float bar = (axis == FlightAxis::Throttle) ? val : (val + 1.0f) * 0.5f;
                ImGui::ProgressBar(bar, ImVec2(160, 14));
                ImGui::SameLine();
                ImGui::Checkbox("Inv", &b.invert);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                ImGui::SliderFloat("DZ", &b.deadzone, 0.0f, 0.25f, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                ImGui::SliderFloat("Curve", &b.curve, 0.0f, 1.0f, "%.2f");
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Buttons & Hats")) {
            auto& pinky = hotas_.pinky_shift_binding();
            ImGui::Text("Pinky / Shift Layer:");
            ImGui::SameLine();
            if (pinky.source == InputSourceType::Button) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s Btn %d %s",
                                   pinky.device_name.c_str(), pinky.index + 1,
                                   hotas_.pinky_shift_active() ? "[ACTIVE]" : "");
            } else {
                ImGui::TextDisabled("None (Optional)");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(hotas_.capturing_pinky() ? "Press..." : "Bind Pinky")) {
                hotas_.start_pinky_capture();
            }
            ImGui::Separator();

            if (ImGui::BeginChild("BtnScroll", ImVec2(0, 0), false)) {
                auto& btns = hotas_.button_bindings();
                for (int i = 0; i < static_cast<int>(btns.size()); ++i) {
                    ImGui::PushID(1000 + i);
                    auto& bb = btns[i];
                    ImGui::Text("%s", bb.action_name.c_str());
                    ImGui::SameLine(230);
                    if (bb.source == InputSourceType::Button) {
                        std::string sd = bb.device_name;
                        if (sd.size() > 14) sd = sd.substr(0, 12) + "..";
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Btn %d (%s)", bb.index + 1, sd.c_str());
                    } else if (bb.source == InputSourceType::Hat) {
                        std::string sd = bb.device_name;
                        if (sd.size() > 12) sd = sd.substr(0, 10) + "..";
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Hat %d %s (%s)", bb.index + 1, hat_dir_name(bb.hat_mask), sd.c_str());
                    } else {
                        ImGui::TextDisabled("--");
                    }
                    ImGui::SameLine(430);
                    ImGui::Checkbox("Shf", &bb.require_pinky_shift);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(hotas_.capturing_button() == i ? "..." : "Bind")) {
                        hotas_.start_button_capture(i);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        bb.source = InputSourceType::None;
                        bb.index = -1;
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Connected Devices")) {
            const auto& devs = hotas_.devices();
            if (devs.empty()) {
                ImGui::TextDisabled("No joysticks/HOTAS detected. Plug in a device or click Refresh.");
            } else {
                for (const auto& d : devs) {
                    if (ImGui::TreeNode(d.name.c_str())) {
                        ImGui::Text("GUID: %s", d.guid.c_str());
                        ImGui::Text("Axes: %d | Buttons: %d | Hats: %d", d.num_axes, d.num_buttons, d.num_hats);
                        for (int a = 0; a < d.num_axes; ++a) {
                            char lbl[32];
                            std::snprintf(lbl, sizeof(lbl), "Axis %d: %.2f", a, d.axis_values[a]);
                            ImGui::ProgressBar((d.axis_values[a] + 1.0f) * 0.5f, ImVec2(220, 12), lbl);
                        }
                        ImGui::TreePop();
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void OverlayUI::draw_audio_window() {
    ImGui::SetNextWindowSize(ImVec2(430, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Engine (CM-32L / SB16)", &show_audio_)) {
        ImGui::End();
        return;
    }

    auto& cfg = audio_.config();
    int mode = static_cast<int>(cfg.music_backend);
    const char* modes[] = {"Roland CM-32L / MT-32 (Munt)", "Sound Blaster 16 / OPL3 (Legacy)", "Muted"};
    if (ImGui::Combo("Music Synth", &mode, modes, 3)) {
        cfg.music_backend = static_cast<MusicBackend>(mode);
    }
    ImGui::Checkbox("Enable Sound Blaster 16 Digital SFX", &cfg.enable_sb16_sfx);

    ImGui::Separator();
    ImGui::Text("CM-32L Status:");
    ImGui::SameLine();
    ImGui::TextColored(audio_.mt32_ready() ? ImVec4(0.3f, 0.95f, 0.4f, 1.0f) : ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                       "%s", audio_.mt32_status().c_str());
    ImGui::Text("MIDI Bytes RX: %llu | Live Partials: %u / %u",
                static_cast<unsigned long long>(audio_.midi_bytes_received()),
                audio_.mt32_active_partials(), audio_.mt32_partial_limit());
    ImGui::Text("Sample Proxy: %s | Voices: %u / %d | Baked ROM Samples: %u",
                audio_.proxy_ready() ? "ACTIVE (ROM-Backed)" : "OFF (Needs Valid CM-32L ROM)",
                audio_.proxy_active_voices(), AudioEngine::kMaxProxyVoices,
                audio_.proxy_baked_count());
    ImGui::TextDisabled("%s", audio_.last_midi_summary().c_str());

    ImGui::Separator();
    ImGui::Checkbox("64-Voice CM-32L Sample Proxy in Flight (Zero Cutouts — Baked from Your ROM)", &cfg.use_cm32l_sample_proxy);
    if (cfg.use_cm32l_sample_proxy) {
        ImGui::Checkbox("Telemetry-Driven Engine / AB / Wind / Chaff / Flare (Zero DOS Cutouts!)", &cfg.telemetry_flight_audio);
        ImGui::Checkbox("3D Spatial Audio & Doppler Flybys (Stereo Pan + Passing Jets)", &cfg.spatial_3d_audio);
        ImGui::Checkbox("Radio Voice Ducking (Dip Engine When AWACS/Wingman Speaks)", &cfg.audio_radio_ducking);
        ImGui::Checkbox("Warm Cockpit Bass Body (Uncheck for Pure Dry 1988 CM-32L)", &cfg.audio_warm_enhance);
        ImGui::Checkbox("M61 Cannon Anti-Drop (Keep Baked CM-32L Gun Alive Over RWR/Comms)", &cfg.gun_proxy_anti_drop);
    } else {
        const int partial_vals[] = {32, 64, 128, 256};
        const char* partial_labels[] = {
            "32 Partials (Authentic CM-32L — Recommended)",
            "64 Partials (2x Polyphony + Anti-Stack)",
            "128 Partials (4x Polyphony + Anti-Stack)",
            "256 Partials (Max Polyphony + Anti-Stack)"
        };
        int cur_idx = 0;
        for (int i = 0; i < 4; ++i) {
            if (cfg.mt32_max_partials == partial_vals[i]) cur_idx = i;
        }
        if (ImGui::Combo("Direct CM-32L Polyphony", &cur_idx, partial_labels, 4)) {
            cfg.mt32_max_partials = partial_vals[cur_idx];
            audio_.load_mt32_roms(rom_dir_buf_);
        }
        ImGui::Checkbox("Nice Partial Mixing (Prevent Overlap Phase Cancel)", &cfg.nice_partial_mixing);
    }
    ImGui::Checkbox("Optional Synthetic M61A1 Vulcan Layer (OFF = Pure CM-32L Gun)", &cfg.polyphonic_gun_sfx);

    ImGui::InputText("ROM Folder", rom_dir_buf_, sizeof(rom_dir_buf_));
    if (ImGui::Button("Reload CM-32L ROMs")) {
        audio_.load_mt32_roms(rom_dir_buf_);
    }

    ImGui::Separator();
    ImGui::SliderFloat("Master Volume", &cfg.master_volume, 0.0f, 1.0f);
    ImGui::SliderFloat("Engine / AB / 3D Flyby Bus", &cfg.engine_bus_volume, 0.0f, 1.5f);
    ImGui::SliderFloat("Canopy Wind / G-Buffet", &cfg.wind_volume, 0.0f, 1.5f);
    ImGui::SliderFloat("Weapons / Chaff / Flare / Booms Bus", &cfg.weapons_bus_volume, 0.0f, 1.5f);
    ImGui::SliderFloat("Avionics / Locks / RWR Bus", &cfg.avionics_bus_volume, 0.0f, 1.5f);
    ImGui::SliderFloat("Radio Comms (SB16) Bus", &cfg.sb16_sfx_volume, 0.0f, 1.5f);
    ImGui::SliderFloat("Menu Music (Live CM-32L)", &cfg.mt32_volume, 0.0f, 1.2f);

    ImGui::End();
}

void OverlayUI::draw_debugger_window() {
    ImGui::SetNextWindowSize(ImVec2(480, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CPU / DOS HLE Debugger", &show_debugger_)) {
        ImGui::End();
        return;
    }

    auto snap = machine_.snapshot();
    if (ImGui::Button(machine_.paused() ? "Resume" : "Pause")) {
        machine_.set_paused(!machine_.paused());
    }
    ImGui::SameLine();
    if (ImGui::Button("Step 1")) {
        bool was = machine_.paused();
        machine_.set_paused(false);
        machine_.step(1);
        machine_.set_paused(was);
    }
    ImGui::SameLine();
    ImGui::Text("Insns: %llu | VGA: 320x%d | EMS: %d/%d",
                static_cast<unsigned long long>(snap.total_insns),
                machine_.vga().visible_height(),
                snap.ems_pages_used, snap.ems_pages_total);

    float mips = static_cast<float>(machine_.cpu_speed_ips()) / 1000000.0f;
    if (ImGui::SliderFloat("Menu CPU Speed (MIPS)", &mips, 0.5f, 15.0f, "%.1f M insns/s")) {
        machine_.set_cpu_speed_ips(static_cast<uint64_t>(mips * 1000000.0f));
    }

    ImGui::Separator();
    ImGui::Text("CS:IP = %04X:%04X   SS:SP = %04X:%04X   DS=%04X ES=%04X",
                snap.cs, snap.ip, snap.ss, snap.sp, snap.ds, snap.es);
    ImGui::Text("AX=%04X BX=%04X CX=%04X DX=%04X  SI=%04X DI=%04X BP=%04X FL=%04X",
                snap.eax & 0xFFFF, snap.ebx & 0xFFFF, snap.ecx & 0xFFFF, snap.edx & 0xFFFF,
                snap.esi & 0xFFFF, snap.edi & 0xFFFF, snap.ebp & 0xFFFF, snap.eflags & 0xFFFF);

    if (snap.exited) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "PROCESS EXITED (code %u)", snap.exit_code);
    }

    ImGui::Separator();
    if (ImGui::BeginChild("LogScroll", ImVec2(0, 0), true)) {
        for (const auto& line : machine_.log_lines()) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

} // namespace falcon
