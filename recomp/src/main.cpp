#include "audio/audio_engine.h"
#include "dos/dos_machine.h"
#include "input/head_tracker.h"
#include "input/hotas_mapper.h"
#include "net/net_udp.h"
#include "render/modern_renderer.h"
#include "sim/f16_flight_model.h"
#include "ui/overlay_ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

uint8_t sdl_scancode_to_dos(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
        case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05;
        case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
        case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
        case SDL_SCANCODE_9: return 0x0A; case SDL_SCANCODE_0: return 0x0B;
        case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
        case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11;
        case SDL_SCANCODE_E: return 0x12; case SDL_SCANCODE_R: return 0x13;
        case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
        case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17;
        case SDL_SCANCODE_O: return 0x18; case SDL_SCANCODE_P: return 0x19;
        case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_RETURN: return 0x1C;
        case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F;
        case SDL_SCANCODE_D: return 0x20; case SDL_SCANCODE_F: return 0x21;
        case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
        case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25;
        case SDL_SCANCODE_L: return 0x26;
        case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D;
        case SDL_SCANCODE_C: return 0x2E; case SDL_SCANCODE_V: return 0x2F;
        case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
        case SDL_SCANCODE_M: return 0x32;
        case SDL_SCANCODE_SPACE: return 0x39;
        case SDL_SCANCODE_UP: return 0x48; case SDL_SCANCODE_DOWN: return 0x50;
        case SDL_SCANCODE_LEFT: return 0x4B; case SDL_SCANCODE_RIGHT: return 0x4D;
        default: return 0;
    }
}

std::string resolve_falcon_dir(const fs::path& base) {
    if (base.empty()) return "";
    if (fs::exists(base / "FALCONCD.EXE")) return fs::absolute(base).string();
    if (fs::exists(base / "FALCON3" / "FALCONCD.EXE")) return fs::absolute(base / "FALCON3").string();
    return "";
}

std::string find_game_dir(const std::string& explicit_dir, int argc, char** argv) {
    if (!explicit_dir.empty()) {
        std::string r = resolve_falcon_dir(explicit_dir);
        if (!r.empty()) return r;
    }
    if (argc > 1 && argv[1][0] != '-') {
        std::string r = resolve_falcon_dir(argv[1]);
        if (!r.empty()) return r;
    }
    if (const char* env_dir = std::getenv("FALCON3_GAME_DIR"); env_dir && env_dir[0]) {
        std::string r = resolve_falcon_dir(env_dir);
        if (!r.empty()) return r;
    }
    const char* candidates[] = {
        ".",
        "game_data",
        "../game_data",
        "../../game_data",
        "..",
        "../..",
        R"(C:\GOG Games\Falcon Gold)",
        R"(D:\GOG Games\Falcon Gold)",
        R"(E:\GOG Games\Falcon Gold)",
        R"(C:\Program Files (x86)\GOG Galaxy\Games\Falcon Gold)",
        R"(D:\Program Files (x86)\GOG Galaxy\Games\Falcon Gold)"
    };
    for (const char* c : candidates) {
        std::string r = resolve_falcon_dir(c);
        if (!r.empty()) return r;
    }
    return ".";
}

bool has_mt32_roms_in(const fs::path& dir) {
    if (dir.empty() || !fs::exists(dir) || !fs::is_directory(dir)) return false;
    bool ctrl = fs::exists(dir / "CM32L_CONTROL.ROM") || fs::exists(dir / "cm32l_control.rom") ||
                fs::exists(dir / "MT32_CONTROL.ROM")  || fs::exists(dir / "mt32_control.rom");
    bool pcm  = fs::exists(dir / "CM32L_PCM.ROM")     || fs::exists(dir / "cm32l_pcm.rom") ||
                fs::exists(dir / "MT32_PCM.ROM")      || fs::exists(dir / "mt32_pcm.rom");
    return ctrl && pcm;
}

std::string find_rom_dir(const std::string& explicit_rom_dir) {
    if (!explicit_rom_dir.empty()) return explicit_rom_dir;
    if (const char* e1 = std::getenv("MT32_ROM_DIR"); e1 && e1[0] && has_mt32_roms_in(e1)) return e1;
    if (const char* e2 = std::getenv("CM32L_ROM_DIR"); e2 && e2[0] && has_mt32_roms_in(e2)) return e2;
    const char* candidates[] = {
        "roms",
        "cm32l",
        "../roms",
        "../cm32l",
        "../../roms",
        "../../cm32l",
        R"(C:\munt\roms)",
        R"(C:\mt32-roms)"
    };
    for (const char* c : candidates) {
        if (has_mt32_roms_in(c)) return fs::absolute(c).string();
    }
    if (const char* appdata = std::getenv("APPDATA"); appdata && appdata[0]) {
        fs::path p = fs::path(appdata) / "munt" / "roms";
        if (has_mt32_roms_in(p)) return p.string();
    }
    return "roms";
}

fs::path resolve_config_dir(const std::string& game_dir) {
    std::error_code ec;
    fs::path gd = fs::weakly_canonical(fs::path(game_dir), ec);
    if (!ec && !gd.empty()) {
        std::string leaf = gd.filename().string();
        std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (leaf == "game_data" && gd.has_parent_path()) {
            return gd.parent_path();
        }
    }
    return fs::current_path(ec);
}

std::string pick_folder_native(const char* title) {
#ifdef _WIN32
    BROWSEINFOA bi{};
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return "";
    char path[MAX_PATH]{};
    bool ok = SHGetPathFromIDListA(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    return ok ? std::string(path) : "";
#else
    (void)title;
    return "";
#endif
}

void peek_saved_paths(const std::string& cfg_path, std::string& out_game_dir, std::string& out_rom_dir) {
    std::ifstream in(cfg_path);
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("paths") && j["paths"].is_object()) {
            const auto& p = j["paths"];
            if (out_game_dir.empty() && p.contains("game_dir") && p["game_dir"].is_string()) {
                out_game_dir = p["game_dir"].get<std::string>();
            }
            if (out_rom_dir.empty() && p.contains("cm32l_rom_dir") && p["cm32l_rom_dir"].is_string()) {
                out_rom_dir = p["cm32l_rom_dir"].get<std::string>();
            }
        }
        if (out_rom_dir.empty() && j.contains("audio") && j["audio"].is_object() &&
            j["audio"].contains("cm32l_rom_dir") && j["audio"]["cm32l_rom_dir"].is_string()) {
            out_rom_dir = j["audio"]["cm32l_rom_dir"].get<std::string>();
        }
    } catch (...) {}
}

bool load_host_settings(const std::string& path,
                        const std::string& explicit_rom_dir,
                        falcon::AudioEngine& audio,
                        falcon::ModernRenderer& renderer,
                        falcon::F16FlightModel& flight_model,
                        falcon::DosMachine& machine,
                        falcon::HeadTracker& head_tracker,
                        int& init_win_w,
                        int& init_win_h,
                        bool& show_launcher_on_startup,
                        std::string& last_exe) {
    std::ifstream in(path);
    if (!in) return false;
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("audio") && j["audio"].is_object()) {
            const auto& a = j["audio"];
            auto& ac = audio.config();
            if (explicit_rom_dir.empty() && a.contains("cm32l_rom_dir") && a["cm32l_rom_dir"].is_string()) {
                std::string saved_rom = a["cm32l_rom_dir"].get<std::string>();
                if (!saved_rom.empty()) ac.cm32l_rom_dir = saved_rom;
            }
            if (a.contains("music_backend")) ac.music_backend = static_cast<falcon::MusicBackend>(std::clamp(a["music_backend"].get<int>(), 0, 2));
            if (a.contains("enable_sb16_sfx")) ac.enable_sb16_sfx = a["enable_sb16_sfx"].get<bool>();
            if (a.contains("use_cm32l_sample_proxy")) ac.use_cm32l_sample_proxy = a["use_cm32l_sample_proxy"].get<bool>();
            if (a.contains("telemetry_flight_audio")) ac.telemetry_flight_audio = a["telemetry_flight_audio"].get<bool>();
            if (a.contains("spatial_3d_audio")) ac.spatial_3d_audio = a["spatial_3d_audio"].get<bool>();
            if (a.contains("audio_radio_ducking")) ac.audio_radio_ducking = a["audio_radio_ducking"].get<bool>();
            if (a.contains("audio_warm_enhance")) ac.audio_warm_enhance = a["audio_warm_enhance"].get<bool>();
            if (a.contains("gun_proxy_anti_drop")) ac.gun_proxy_anti_drop = a["gun_proxy_anti_drop"].get<bool>();
            if (a.contains("master_volume")) ac.master_volume = std::clamp(a["master_volume"].get<float>(), 0.0f, 1.5f);
            if (a.contains("mt32_volume")) ac.mt32_volume = std::clamp(a["mt32_volume"].get<float>(), 0.0f, 1.5f);
            if (a.contains("engine_bus_volume")) ac.engine_bus_volume = std::clamp(a["engine_bus_volume"].get<float>(), 0.0f, 1.5f);
            if (a.contains("wind_volume")) ac.wind_volume = std::clamp(a["wind_volume"].get<float>(), 0.0f, 1.5f);
            if (a.contains("weapons_bus_volume")) ac.weapons_bus_volume = std::clamp(a["weapons_bus_volume"].get<float>(), 0.0f, 1.5f);
            if (a.contains("avionics_bus_volume")) ac.avionics_bus_volume = std::clamp(a["avionics_bus_volume"].get<float>(), 0.0f, 1.5f);
            if (a.contains("sb16_sfx_volume")) ac.sb16_sfx_volume = std::clamp(a["sb16_sfx_volume"].get<float>(), 0.0f, 1.5f);
        }
        if (j.contains("render") && j["render"].is_object()) {
            const auto& r = j["render"];
            auto& vr = renderer.vr_config();
            if (r.contains("mode")) renderer.mode() = static_cast<falcon::RenderMode>(std::clamp(r["mode"].get<int>(), 0, 3));
            if (r.contains("hybrid_fill_widescreen")) vr.hybrid_fill_widescreen = r["hybrid_fill_widescreen"].get<bool>();
            if (r.contains("widescreen_3d_cockpit")) vr.widescreen_3d_cockpit = r["widescreen_3d_cockpit"].get<bool>();
            if (r.contains("enhanced_3d_padlock")) vr.enhanced_3d_padlock = r["enhanced_3d_padlock"].get<bool>();
            if (r.contains("keep_padlock_top_strip")) vr.keep_padlock_top_strip = r["keep_padlock_top_strip"].get<bool>();
            if (r.contains("virtual_cockpit_freelook")) vr.virtual_cockpit_freelook = r["virtual_cockpit_freelook"].get<bool>();
            if (r.contains("show_clouds")) vr.show_clouds = r["show_clouds"].get<bool>();
            if (r.contains("show_aircraft")) vr.show_aircraft = r["show_aircraft"].get<bool>();
            if (r.contains("show_ground_objects")) vr.show_ground_objects = r["show_ground_objects"].get<bool>();
            if (r.contains("show_ground_units")) vr.show_ground_units = r["show_ground_units"].get<bool>();
            if (r.contains("show_missiles_trails")) vr.show_missiles_trails = r["show_missiles_trails"].get<bool>();
            if (r.contains("show_explosions")) vr.show_explosions = r["show_explosions"].get<bool>();
            if (r.contains("show_contrails")) vr.show_contrails = r["show_contrails"].get<bool>();
            if (r.contains("show_roads_rivers")) vr.show_roads_rivers = r["show_roads_rivers"].get<bool>();
            if (r.contains("retro_desert_facets")) vr.retro_desert_facets = r["retro_desert_facets"].get<bool>();
            if (r.contains("low_alt_sand_dots")) vr.low_alt_sand_dots = r["low_alt_sand_dots"].get<bool>();
            if (r.contains("horizon_fog")) vr.horizon_fog = r["horizon_fog"].get<bool>();
            if (r.contains("ground_shadows")) vr.ground_shadows = r["ground_shadows"].get<bool>();
            if (r.contains("wireframe_terrain")) vr.wireframe_terrain = r["wireframe_terrain"].get<bool>();
            if (r.contains("smart_scaling_mode")) vr.smart_scaling_mode = static_cast<falcon::SmartScalingMode>(std::clamp(r["smart_scaling_mode"].get<int>(), 0, 3));
            if (r.contains("smart_scale_ground")) vr.smart_scale_ground = r["smart_scale_ground"].get<bool>();
            if (r.contains("fov_deg")) vr.fov_deg = std::clamp(r["fov_deg"].get<float>(), 30.0f, 75.0f);
            if (r.contains("widescreen_fov_deg")) vr.widescreen_fov_deg = std::clamp(r["widescreen_fov_deg"].get<float>(), 40.0f, 64.0f);
            if (r.contains("aircraft_scale")) vr.aircraft_scale = std::clamp(r["aircraft_scale"].get<float>(), 0.5f, 2.5f);
            if (r.contains("terrain_scale")) vr.terrain_scale = std::clamp(r["terrain_scale"].get<float>(), 0.5f, 3.0f);
            if (r.contains("hud_cy_trim_px")) vr.hud_cy_trim_px = std::clamp(r["hud_cy_trim_px"].get<float>(), -12.0f, 12.0f);
            if (r.contains("lighting_mode")) vr.lighting_mode = static_cast<falcon::LightingMode>(std::clamp(r["lighting_mode"].get<int>(), 0, 3));
        }
        if (j.contains("head_tracker") && j["head_tracker"].is_object()) {
            const auto& ht = j["head_tracker"];
            auto& htc = head_tracker.config();
            if (ht.contains("enabled")) htc.enabled = ht["enabled"].get<bool>();
            if (ht.contains("udp_port")) htc.udp_port = std::clamp(ht["udp_port"].get<int>(), 1024, 65535);
            if (ht.contains("yaw_scale")) htc.yaw_scale = std::clamp(ht["yaw_scale"].get<float>(), 0.3f, 3.0f);
            if (ht.contains("pitch_scale")) htc.pitch_scale = std::clamp(ht["pitch_scale"].get<float>(), 0.3f, 3.0f);
            if (ht.contains("invert_yaw")) htc.invert_yaw = ht["invert_yaw"].get<bool>();
            if (ht.contains("invert_pitch")) htc.invert_pitch = ht["invert_pitch"].get<bool>();
        }
        if (j.contains("flight_model") && j["flight_model"].is_object()) {
            const auto& fm = j["flight_model"];
            auto& fc = flight_model.config();
            if (fm.contains("enabled")) fc.enabled = fm["enabled"].get<bool>();
            if (fm.contains("cat3_limiter")) fc.cat3_limiter = fm["cat3_limiter"].get<bool>();
            if (fm.contains("aoa_limiter")) fc.aoa_limiter = fm["aoa_limiter"].get<bool>();
            if (fm.contains("engine_f110_ge100")) fc.engine_f110_ge100 = fm["engine_f110_ge100"].get<bool>();
            if (fm.contains("max_roll_rate_dps")) fc.max_roll_rate_dps = std::clamp(fm["max_roll_rate_dps"].get<float>(), 90.0f, 360.0f);
            if (fm.contains("stick_pitch_sens")) fc.stick_pitch_sens = std::clamp(fm["stick_pitch_sens"].get<float>(), 0.3f, 2.0f);
            if (fm.contains("stick_roll_sens")) fc.stick_roll_sens = std::clamp(fm["stick_roll_sens"].get<float>(), 0.3f, 2.0f);
        }
        if (j.contains("host") && j["host"].is_object()) {
            const auto& h = j["host"];
            if (h.contains("skip_intro")) machine.set_skip_intro(h["skip_intro"].get<bool>());
            if (h.contains("auto_turbo_in_flight")) machine.set_auto_turbo(h["auto_turbo_in_flight"].get<bool>());
            if (h.contains("flight_cpu_speed_ips")) {
                machine.set_flight_cpu_speed_ips(std::clamp<uint64_t>(h["flight_cpu_speed_ips"].get<uint64_t>(), 4000000ULL, 35000000ULL));
            }
            if (h.contains("flight_input_mode")) {
                machine.set_flight_input_mode(static_cast<falcon::FlightInputMode>(std::clamp(h["flight_input_mode"].get<int>(), 0, 2)));
            }
            if (h.contains("sam_mod_preset")) {
                machine.set_sam_mod_preset(static_cast<falcon::SamModPreset>(std::clamp(h["sam_mod_preset"].get<int>(), 0, 3)));
            }
            if (h.contains("show_launcher_on_startup")) {
                show_launcher_on_startup = h["show_launcher_on_startup"].get<bool>();
            }
            if (h.contains("last_exe") && h["last_exe"].is_string()) {
                std::string le = h["last_exe"].get<std::string>();
                if (!le.empty()) last_exe = le;
            }
            if (h.contains("window_w")) init_win_w = std::clamp(h["window_w"].get<int>(), 640, 7680);
            if (h.contains("window_h")) init_win_h = std::clamp(h["window_h"].get<int>(), 480, 4320);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool save_host_settings(const std::string& path,
                        const std::string& game_dir,
                        const falcon::AudioEngine& audio,
                        const falcon::ModernRenderer& renderer,
                        const falcon::F16FlightModel& flight_model,
                        const falcon::DosMachine& machine,
                        const falcon::HeadTracker& head_tracker,
                        int win_w,
                        int win_h,
                        bool show_launcher_on_startup,
                        const std::string& last_exe) {
    try {
        nlohmann::json j;
        const auto& ac = audio.config();
        j["paths"] = {
            {"game_dir", game_dir},
            {"cm32l_rom_dir", ac.cm32l_rom_dir},
        };
        j["audio"] = {
            {"cm32l_rom_dir", ac.cm32l_rom_dir},
            {"music_backend", static_cast<int>(ac.music_backend)},
            {"enable_sb16_sfx", ac.enable_sb16_sfx},
            {"use_cm32l_sample_proxy", ac.use_cm32l_sample_proxy},
            {"telemetry_flight_audio", ac.telemetry_flight_audio},
            {"spatial_3d_audio", ac.spatial_3d_audio},
            {"audio_radio_ducking", ac.audio_radio_ducking},
            {"audio_warm_enhance", ac.audio_warm_enhance},
            {"gun_proxy_anti_drop", ac.gun_proxy_anti_drop},
            {"master_volume", ac.master_volume},
            {"mt32_volume", ac.mt32_volume},
            {"engine_bus_volume", ac.engine_bus_volume},
            {"wind_volume", ac.wind_volume},
            {"weapons_bus_volume", ac.weapons_bus_volume},
            {"avionics_bus_volume", ac.avionics_bus_volume},
            {"sb16_sfx_volume", ac.sb16_sfx_volume},
        };
        const auto& vr = renderer.vr_config();
        j["render"] = {
            {"mode", static_cast<int>(renderer.mode())},
            {"hybrid_fill_widescreen", vr.hybrid_fill_widescreen},
            {"widescreen_3d_cockpit", vr.widescreen_3d_cockpit},
            {"enhanced_3d_padlock", vr.enhanced_3d_padlock},
            {"keep_padlock_top_strip", vr.keep_padlock_top_strip},
            {"virtual_cockpit_freelook", vr.virtual_cockpit_freelook},
            {"show_clouds", vr.show_clouds},
            {"show_aircraft", vr.show_aircraft},
            {"show_ground_objects", vr.show_ground_objects},
            {"show_ground_units", vr.show_ground_units},
            {"show_missiles_trails", vr.show_missiles_trails},
            {"show_explosions", vr.show_explosions},
            {"show_contrails", vr.show_contrails},
            {"show_roads_rivers", vr.show_roads_rivers},
            {"retro_desert_facets", vr.retro_desert_facets},
            {"low_alt_sand_dots", vr.low_alt_sand_dots},
            {"horizon_fog", vr.horizon_fog},
            {"ground_shadows", vr.ground_shadows},
            {"wireframe_terrain", vr.wireframe_terrain},
            {"smart_scaling_mode", static_cast<int>(vr.smart_scaling_mode)},
            {"smart_scale_ground", vr.smart_scale_ground},
            {"fov_deg", vr.fov_deg},
            {"widescreen_fov_deg", vr.widescreen_fov_deg},
            {"aircraft_scale", vr.aircraft_scale},
            {"terrain_scale", vr.terrain_scale},
            {"hud_cy_trim_px", vr.hud_cy_trim_px},
            {"lighting_mode", static_cast<int>(vr.lighting_mode)},
        };
        const auto& htc = head_tracker.config();
        j["head_tracker"] = {
            {"enabled", htc.enabled},
            {"udp_port", htc.udp_port},
            {"yaw_scale", htc.yaw_scale},
            {"pitch_scale", htc.pitch_scale},
            {"invert_yaw", htc.invert_yaw},
            {"invert_pitch", htc.invert_pitch},
        };
        const auto& fc = flight_model.config();
        j["flight_model"] = {
            {"enabled", fc.enabled},
            {"cat3_limiter", fc.cat3_limiter},
            {"aoa_limiter", fc.aoa_limiter},
            {"engine_f110_ge100", fc.engine_f110_ge100},
            {"max_roll_rate_dps", fc.max_roll_rate_dps},
            {"stick_pitch_sens", fc.stick_pitch_sens},
            {"stick_roll_sens", fc.stick_roll_sens},
        };
        j["host"] = {
            {"skip_intro", machine.skip_intro()},
            {"auto_turbo_in_flight", machine.auto_turbo()},
            {"flight_cpu_speed_ips", machine.flight_cpu_speed_ips()},
            {"flight_input_mode", static_cast<int>(machine.flight_input_mode())},
            {"sam_mod_preset", static_cast<int>(machine.sam_mod_preset())},
            {"show_launcher_on_startup", show_launcher_on_startup},
            {"last_exe", last_exe},
            {"window_w", win_w},
            {"window_h", win_h},
        };
        std::ofstream out(path);
        if (!out) return false;
        out << j.dump(2) << "\n";
        return out.good();
    } catch (...) {
        return false;
    }
}

bool has_exe_in_dir(const std::string& dir, const char* exe_name) {
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec)) return false;
    std::string target = exe_name;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string nm = entry.path().filename().string();
        std::transform(nm.begin(), nm.end(), nm.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (nm == target) return true;
    }
    return false;
}

const char* window_title_for_exe(const std::string& exe) {
    std::string u = exe;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (u.find("HORNET") != std::string::npos) return "Hornet: Naval Strike — Falcon Gold Native Host (F/A-18C)";
    if (u.find("MIG") != std::string::npos)    return "MiG-29: Deadly Adversary — Falcon Gold Native Host";
    return "Falcon 3.0 Gold — Native Host (F-16C)";
}

enum class LauncherOutcome { Launch, Cancel, Quit };

LauncherOutcome run_trilogy_launcher(SDL_Window* window,
                                     bool in_game_return,
                                     std::string& game_dir,
                                     std::string& rom_dir,
                                     std::string& selected_exe,
                                     bool& show_launcher_on_startup,
                                     falcon::AudioEngine& audio,
                                     falcon::ModernRenderer& renderer,
                                     falcon::DosMachine& machine,
                                     falcon::HeadTracker& head_tracker) {
    struct TitleEntry {
        const char* exe;
        const char* title;
        const char* aircraft;
        const char* badge;
        const char* tagline;
        const char* features;
        ImVec4 accent;
    };
    static constexpr TitleEntry kTitles[3] = {
        {
            "FALCONCD.EXE",
            "FALCON 3.0",
            "F-16C Fighting Falcon",
            "USAF",
            "Desert Storm, Israel, Panama, Red Flag & Fighting Tiger",
            "- Full 3D HD Terrain & 8 Theaters\n- Live 2D HUD/MFD/RWR + 360 Virtual Pit\n- Optional 240Hz NASA F-16C Flight Model",
            ImVec4(0.24f, 0.88f, 0.48f, 1.0f)
        },
        {
            "HORNETCD.EXE",
            "HORNET: NAVAL STRIKE",
            "F/A-18C Hornet",
            "US NAVY",
            "Carrier Operations, Maritime Strike & Air Superiority",
            "- Full 3D / Hybrid Cockpit & 360 Virtual Pit\n- Verified F/A-18C 6-DOF Telemetry & Weapons\n- All 8 Theaters + 64-Voice CM-32L Audio",
            ImVec4(0.32f, 0.68f, 1.00f, 1.0f)
        },
        {
            "MIGCD.EXE",
            "MIG-29: DEADLY ADVERSARY",
            "MiG-29A Fulcrum",
            "VVS / PVO",
            "Frontline Interceptor, IRST & Helmet-Mounted Sight",
            "- Full 3D / Hybrid Cockpit & 360 Virtual Pit\n- Verified MiG-29A 6-DOF Telemetry & Weapons\n- All 8 Theaters + 64-Voice CM-32L Audio",
            ImVec4(0.95f, 0.42f, 0.34f, 1.0f)
        }
    };

    int sel_idx = 0;
    {
        std::string u = selected_exe;
        std::transform(u.begin(), u.end(), u.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (u.find("HORNET") != std::string::npos) sel_idx = 1;
        else if (u.find("MIG") != std::string::npos) sel_idx = 2;
    }

    bool avail[3] = {false, false, false};
    bool roms_ok = false;
    auto refresh_detection = [&]() {
        for (int i = 0; i < 3; ++i) {
            avail[i] = has_exe_in_dir(game_dir, kTitles[i].exe);
        }
        roms_ok = has_mt32_roms_in(rom_dir);
    };
    refresh_detection();

    SDL_SetWindowTitle(window, "Falcon 3.0 Gold — Trilogy Launcher");

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) {
                return LauncherOutcome::Quit;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE && in_game_return) {
                    return LauncherOutcome::Cancel;
                }
                if (ev.key.scancode == SDL_SCANCODE_LEFT) {
                    sel_idx = (sel_idx + 2) % 3;
                } else if (ev.key.scancode == SDL_SCANCODE_RIGHT) {
                    sel_idx = (sel_idx + 1) % 3;
                } else if ((ev.key.scancode == SDL_SCANCODE_RETURN || ev.key.scancode == SDL_SCANCODE_KP_ENTER) &&
                           avail[sel_idx]) {
                    selected_exe = kTitles[sel_idx].exe;
                    return LauncherOutcome::Launch;
                }
            }
        }

        int win_w = 1280, win_h = 800;
        SDL_GetWindowSize(window, &win_w, &win_h);
        glViewport(0, 0, win_w, win_h);
        glClearColor(0.045f, 0.065f, 0.098f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(win_w), static_cast<float>(win_h)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 22.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.115f, 1.0f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        LauncherOutcome outcome = LauncherOutcome::Cancel;
        bool decided = false;

        if (ImGui::Begin("##FalconGoldLauncher", nullptr, flags)) {
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const float content_w = std::min(avail_w, 1080.0f);
            const float left_pad = std::max(0.0f, (avail_w - content_w) * 0.5f);
            if (left_pad > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left_pad);

            ImGui::BeginGroup();

            // Header Banner
            ImGui::SetWindowFontScale(1.65f);
            ImGui::TextColored(ImVec4(0.28f, 0.94f, 0.50f, 1.0f), "FALCON 3.0 GOLD");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.88f, 0.92f, 0.98f, 1.0f), "NATIVE HOST");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextColored(ImVec4(0.62f, 0.70f, 0.80f, 1.0f),
                               "Select your aircraft & campaign package below  |  F11 Cycles 3D Mode  |  F12 In-Game Overlay  |  F9 Recenter Head");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Warning banner if no game EXEs found yet
            if (!avail[0] && !avail[1] && !avail[2]) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.24f, 0.14f, 0.05f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.65f, 0.18f, 1.0f));
                ImGui::BeginChild("##NoGameWarn", ImVec2(content_w, 54.0f), ImGuiChildFlags_Borders);
                ImGui::SetCursorPos(ImVec2(14.0f, 10.0f));
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f),
                                   "Falcon Gold game files not found in the current folder.");
                ImGui::SetCursorPosX(14.0f);
                ImGui::TextColored(ImVec4(0.92f, 0.88f, 0.80f, 1.0f),
                                   "Click 'Browse...' below and select your GOG Falcon Gold folder (or FALCON3 subfolder).");
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            }

            // 3 Side-by-Side Aircraft Title Cards
            const float card_gap = 16.0f;
            const float card_w = (content_w - 2.0f * card_gap) / 3.0f;
            const float card_h = 228.0f;

            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine(0.0f, card_gap);
                const auto& t = kTitles[i];
                bool is_sel = (sel_idx == i);
                bool ready = avail[i];

                ImVec4 bg = is_sel ? ImVec4(0.10f, 0.16f, 0.24f, 1.0f) : ImVec4(0.075f, 0.105f, 0.155f, 1.0f);
                ImVec4 border = is_sel ? t.accent : ImVec4(0.22f, 0.28f, 0.36f, 0.85f);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
                ImGui::PushStyleColor(ImGuiCol_Border, border);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, is_sel ? 2.5f : 1.0f);

                char cid[32];
                std::snprintf(cid, sizeof(cid), "##TitleCard_%d", i);
                ImGui::BeginChild(cid, ImVec2(card_w, card_h), ImGuiChildFlags_Borders);

                ImGui::SetCursorPos(ImVec2(14.0f, 12.0f));
                ImGui::TextColored(t.accent, "[%s]", t.badge);
                ImGui::SameLine();
                const char* status_txt = ready ? "READY" : "NOT FOUND";
                float st_w = ImGui::CalcTextSize(status_txt).x;
                ImGui::SetCursorPosX(card_w - st_w - 14.0f);
                ImGui::TextColored(ready ? ImVec4(0.30f, 0.95f, 0.45f, 1.0f) : ImVec4(0.85f, 0.35f, 0.32f, 1.0f),
                                   "%s", status_txt);

                ImGui::SetCursorPosX(14.0f);
                ImGui::SetWindowFontScale(1.22f);
                ImGui::TextColored(ready ? ImVec4(0.96f, 0.98f, 1.0f, 1.0f) : ImVec4(0.55f, 0.58f, 0.62f, 1.0f),
                                   "%s", t.title);
                ImGui::SetWindowFontScale(1.0f);

                ImGui::SetCursorPosX(14.0f);
                ImGui::TextColored(ImVec4(0.88f, 0.78f, 0.40f, 1.0f), "%s  (%s)", t.aircraft, t.exe);

                ImGui::Spacing();
                ImGui::SetCursorPosX(14.0f);
                ImGui::PushTextWrapPos(card_w - 14.0f);
                ImGui::TextColored(ImVec4(0.76f, 0.82f, 0.90f, 1.0f), "%s", t.tagline);
                ImGui::Spacing();
                ImGui::SetCursorPosX(14.0f);
                ImGui::TextColored(ImVec4(0.60f, 0.68f, 0.78f, 1.0f), "%s", t.features);
                ImGui::PopTextWrapPos();

                // Clickable card hit area (single click selects, double click launches!)
                ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
                char bid[32];
                std::snprintf(bid, sizeof(bid), "##CardHit_%d", i);
                if (ImGui::InvisibleButton(bid, ImVec2(card_w, card_h))) {
                    sel_idx = i;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ready) {
                    sel_idx = i;
                    selected_exe = t.exe;
                    outcome = LauncherOutcome::Launch;
                    decided = true;
                }

                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // Folders & Roland ROMs Panel
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.102f, 0.148f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.26f, 0.35f, 0.90f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::BeginChild("##PathsPanel", ImVec2(content_w, 108.0f), ImGuiChildFlags_Borders);
            ImGui::SetCursorPos(ImVec2(14.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.55f, 0.82f, 1.0f, 1.0f), "INSTALL & AUDIO FOLDERS");

            // Row 1: Game folder
            ImGui::SetCursorPos(ImVec2(14.0f, 36.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Falcon Gold Folder:");
            ImGui::SameLine(178.0f);
            ImGui::SetNextItemWidth(content_w - 305.0f);
            char gd_buf[512]{};
            std::snprintf(gd_buf, sizeof(gd_buf), "%s", game_dir.c_str());
            if (ImGui::InputText("##GameDirInput", gd_buf, sizeof(gd_buf))) {
                game_dir = gd_buf;
                std::string resolved = resolve_falcon_dir(game_dir);
                if (!resolved.empty()) game_dir = resolved;
                refresh_detection();
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...##GameDir", ImVec2(106.0f, 0.0f))) {
                std::string picked = pick_folder_native("Select your Falcon 3.0 Gold folder (containing FALCONCD.EXE or FALCON3\\)");
                if (!picked.empty()) {
                    std::string resolved = resolve_falcon_dir(picked);
                    game_dir = !resolved.empty() ? resolved : picked;
                    refresh_detection();
                }
            }

            // Row 2: Roland ROM folder
            ImGui::SetCursorPos(ImVec2(14.0f, 70.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Roland CM-32L ROMs:");
            ImGui::SameLine(178.0f);
            ImGui::SetNextItemWidth(content_w - 465.0f);
            char rd_buf[512]{};
            std::snprintf(rd_buf, sizeof(rd_buf), "%s", rom_dir.c_str());
            if (ImGui::InputText("##RomDirInput", rd_buf, sizeof(rd_buf))) {
                rom_dir = rd_buf;
                audio.config().cm32l_rom_dir = rom_dir;
                refresh_detection();
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...##RomDir", ImVec2(106.0f, 0.0f))) {
                std::string picked = pick_folder_native("Select your Roland CM-32L / MT-32 ROM folder");
                if (!picked.empty()) {
                    rom_dir = picked;
                    audio.config().cm32l_rom_dir = rom_dir;
                    refresh_detection();
                }
            }
            ImGui::SameLine();
            if (roms_ok) {
                ImGui::TextColored(ImVec4(0.30f, 0.95f, 0.45f, 1.0f), "CM-32L/MT-32 Ready");
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.28f, 1.0f), "No ROMs (Fallback)");
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            ImGui::Spacing();

            // Quick Flight Settings Panel
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.102f, 0.148f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.26f, 0.35f, 0.90f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::BeginChild("##QuickSettingsPanel", ImVec2(content_w, 120.0f), ImGuiChildFlags_Borders);
            ImGui::SetCursorPos(ImVec2(14.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.55f, 0.82f, 1.0f, 1.0f), "QUICK LAUNCH SETTINGS");

            ImGui::SetCursorPos(ImVec2(14.0f, 38.0f));
            ImGui::BeginGroup();
            int rmode = static_cast<int>(renderer.mode());
            const char* rmodes[] = {
                "Classic 1991 VGA (320x200)",
                "Modern 3D (Virtual Cockpit)",
                "Hybrid Cockpit (HD 3D + 2D HUD)",
                "VR Stereo Preview (Side-by-Side)"
            };
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::Combo("Render Mode", &rmode, rmodes, 4)) {
                renderer.mode() = static_cast<falcon::RenderMode>(rmode);
            }

            int sam = static_cast<int>(machine.sam_mod_preset());
            const char* sams[] = {
                "Vanilla (Stock 1991)",
                "Historical F3MSL (Recommended)",
                "Early Strela (SA-7A)",
                "Low Threat / Training"
            };
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::Combo("SAM Realism", &sam, sams, 4)) {
                machine.set_sam_mod_preset(static_cast<falcon::SamModPreset>(sam));
            }
            ImGui::EndGroup();

            ImGui::SameLine(465.0f);
            ImGui::BeginGroup();
            ImGui::Checkbox("Fill Widescreen 3D World", &renderer.vr_config().hybrid_fill_widescreen);
            bool skip_intro = machine.skip_intro();
            if (ImGui::Checkbox("Fast-Boot (Skip Intro Video)", &skip_intro)) {
                machine.set_skip_intro(skip_intro);
            }
            ImGui::Checkbox("Show Launcher on Startup", &show_launcher_on_startup);
            ImGui::EndGroup();

            ImGui::SameLine(745.0f);
            ImGui::BeginGroup();
            ImGui::Checkbox("OpenTrack Head Tracking (UDP 4242)", &head_tracker.config().enabled);
            ImGui::Checkbox("3D Virtual Cockpit Free-Look", &renderer.vr_config().virtual_cockpit_freelook);
            ImGui::Checkbox("Unclamped 3D Padlock ('8')", &renderer.vr_config().enhanced_3d_padlock);
            ImGui::EndGroup();

            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            ImGui::Spacing();
            ImGui::Spacing();

            // Bottom Action Bar
            bool can_launch = avail[sel_idx];
            if (!can_launch) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.56f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.68f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.48f, 0.24f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

            char launch_lbl[128];
            std::snprintf(launch_lbl, sizeof(launch_lbl), "LAUNCH  %s  (%s)", kTitles[sel_idx].title, kTitles[sel_idx].exe);
            if (ImGui::Button(launch_lbl, ImVec2(380.0f, 46.0f))) {
                selected_exe = kTitles[sel_idx].exe;
                outcome = LauncherOutcome::Launch;
                decided = true;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (!can_launch) ImGui::EndDisabled();

            if (in_game_return) {
                ImGui::SameLine();
                if (ImGui::Button("Resume Current Session (Esc)", ImVec2(230.0f, 46.0f))) {
                    outcome = LauncherOutcome::Cancel;
                    decided = true;
                }
            }

            ImGui::SameLine(content_w - 120.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.32f, 0.14f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.46f, 0.18f, 0.18f, 1.0f));
            if (ImGui::Button("Quit", ImVec2(120.0f, 46.0f))) {
                outcome = LauncherOutcome::Quit;
                decided = true;
            }
            ImGui::PopStyleColor(2);

            ImGui::EndGroup();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        if (decided) {
            return outcome;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    bool headless_test = false;
    bool test_fastboot = false;
    bool trace_audio = false;
    bool explicit_exe = false;
    bool force_launcher = false;
    bool skip_launcher_cli = false;
    uint64_t headless_steps = 0;
    std::string dump_ram_path;
    std::string dump_vga_path;
    std::string explicit_game_dir;
    std::string explicit_rom_dir;
    std::string exe_name = "FALCONCD.EXE";
    struct ScriptEvent {
        uint64_t at_step = 0;
        std::string kind; // "key" or "click"
        int a = 0, b = 0;
        bool fired = false;
    };
    std::vector<ScriptEvent> script_events;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless" && i + 1 < argc) {
            headless_test = true;
            headless_steps = std::stoull(argv[++i]);
        } else if (arg == "--test-fastboot") {
            headless_test = true;
            test_fastboot = true;
        } else if (arg == "--trace-audio") {
            trace_audio = true;
        } else if (arg == "--launcher") {
            force_launcher = true;
        } else if (arg == "--no-launcher") {
            skip_launcher_cli = true;
        } else if (arg == "--game-dir" && i + 1 < argc) {
            explicit_game_dir = argv[++i];
        } else if (arg == "--rom-dir" && i + 1 < argc) {
            explicit_rom_dir = argv[++i];
        } else if (arg == "--exe" && i + 1 < argc) {
            exe_name = argv[++i];
            explicit_exe = true;
        } else if (arg == "--dump-ram" && i + 1 < argc) {
            dump_ram_path = argv[++i];
        } else if (arg == "--dump-vga" && i + 1 < argc) {
            dump_vga_path = argv[++i];
        } else if (arg == "--key" && i + 3 < argc) {
            script_events.push_back({std::stoull(argv[i + 1]), "key", std::stoi(argv[i + 2], nullptr, 0), std::stoi(argv[i + 3], nullptr, 0), false});
            i += 3;
        } else if (arg == "--click" && i + 3 < argc) {
            script_events.push_back({std::stoull(argv[i + 1]), "click", std::stoi(argv[i + 2]), std::stoi(argv[i + 3]), false});
            i += 3;
        }
    }

    // Peek saved game_dir / rom_dir from falcon3_config.json (CLI flags always take priority!)
    std::string cfg_dir_hint = (fs::current_path() / "falcon3_config.json").string();
    std::string saved_game_dir, saved_rom_dir;
    peek_saved_paths(cfg_dir_hint, saved_game_dir, saved_rom_dir);

    std::string game_dir = find_game_dir(!explicit_game_dir.empty() ? explicit_game_dir : saved_game_dir, argc, argv);
    fs::path cfg_dir = resolve_config_dir(game_dir);
    std::string config_path = (cfg_dir / "falcon3_config.json").string();
    std::string hotas_path  = (cfg_dir / "falcon3_hotas.json").string();
    if (saved_rom_dir.empty() && config_path != cfg_dir_hint) {
        std::string dummy_gd;
        peek_saved_paths(config_path, dummy_gd, saved_rom_dir);
    }
    std::string rom_dir = find_rom_dir(!explicit_rom_dir.empty() ? explicit_rom_dir : saved_rom_dir);

    if (headless_test) {
        falcon::AudioEngine audio;
        audio.config().cm32l_rom_dir = rom_dir;
        audio.set_trace_audio(trace_audio);
        if (trace_audio) audio.load_mt32_roms(rom_dir);
        falcon::HotasMapper hotas;
        falcon::NetUdp net;
        falcon::DosMachine machine(audio, hotas, net);
        machine.set_skip_intro(test_fastboot);
        if (!machine.load_exe(game_dir, exe_name)) {
            std::printf("Failed to load %s from %s\n", exe_name.c_str(), game_dir.c_str());
            return 1;
        }
        if (test_fastboot) {
            auto t0 = std::chrono::steady_clock::now();
            int frames = 0;
            while (machine.is_fast_booting() && frames < 600) {
                machine.step_realtime(1.0 / 60.0);
                frames++;
            }
            for (int f = 0; f < 5; ++f) machine.step_realtime(1.0 / 60.0);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("Fast-boot finished in %.1f ms (%d host frames, fast_boot_active=%d)\n",
                        ms, frames, machine.is_fast_booting() ? 1 : 0);
        }
        if (headless_steps > 0) {
            uint64_t remaining = headless_steps;
            while (remaining > 0) {
                uint64_t chunk = remaining > 50000 ? 50000 : remaining;
                uint64_t ran = machine.step(chunk);
                machine.on_frame_tick();
                if (trace_audio) {
                    audio.set_in_flight(machine.read_flight_telemetry().in_flight);
                    audio.pump_silent_mt32(256);
                }
                uint64_t cur = machine.snapshot().total_insns;
                for (auto& se : script_events) {
                    if (!se.fired && cur >= se.at_step) {
                        se.fired = true;
                        if (se.kind == "key") {
                            machine.push_key(static_cast<uint8_t>(se.a), static_cast<uint8_t>(se.b), true);
                            machine.step(80000);
                            machine.push_key(static_cast<uint8_t>(se.a), static_cast<uint8_t>(se.b), false);
                        } else if (se.kind == "click") {
                            machine.set_mouse_state(se.a, se.b, 1);
                            machine.step(50000);
                            machine.set_mouse_state(se.a, se.b, 0);
                        }
                    }
                }
                if (ran == 0) break;
                remaining -= ran;
            }
        }
        auto snap = machine.snapshot();
        std::printf("Headless done: insns=%llu CS:IP=%04X:%04X VGA=0x%02X EMS=%d/%d exited=%d MIDI_rx=%llu SysEx=%llu SB_DAC=%llu\n",
                    static_cast<unsigned long long>(snap.total_insns),
                    snap.cs, snap.ip, machine.vga().mode(),
                    snap.ems_pages_used, snap.ems_pages_total, snap.exited ? 1 : 0,
                    static_cast<unsigned long long>(audio.midi_bytes_received()),
                    static_cast<unsigned long long>(audio.sysex_messages_received()),
                    static_cast<unsigned long long>(audio.sb_dac_samples_received()));
        uint32_t m6882 = 0x68820;
        std::printf("6882:0426=%04X 6882:0418=%04X\n",
                    machine.raw_memory()[m6882 + 0x0426] | (machine.raw_memory()[m6882 + 0x0427] << 8),
                    machine.raw_memory()[m6882 + 0x0418] | (machine.raw_memory()[m6882 + 0x0419] << 8));
        uint32_t pc = (static_cast<uint32_t>(snap.cs) << 4) + snap.ip;
        std::printf("Code @ %04X:%04X: ", snap.cs, snap.ip);
        for (int i = -16; i < 24; ++i) {
            if (i == 0) std::printf("[");
            std::printf("%02X", machine.raw_memory()[(pc + i) & 0xFFFFF]);
            if (i == 0) std::printf("]");
            std::printf(" ");
        }
        std::printf("\n");
        for (const auto& line : machine.log_lines()) {
            std::printf("  %s\n", line.c_str());
        }
        std::printf("--- Text Screen (0xB8000) ---\n");
        const uint8_t* tm = machine.raw_memory() + 0xB8000;
        for (int row = 0; row < 25; ++row) {
            std::string line;
            for (int col = 0; col < 80; ++col) {
                char c = static_cast<char>(tm[(row * 80 + col) * 2]);
                line.push_back((c >= 32 && c < 127) ? c : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (!line.empty()) std::printf("  [%02d] %s\n", row, line.c_str());
        }
        auto tel = machine.read_flight_telemetry();
        std::printf("Telemetry: in_flight=%d ALT=%.0f KIAS=%.0f HDG=%.1f PITCH=%.1f ROLL=%.1f AIR=%zu GND=%zu MSL=%zu VIEW=%u PAD=%d(%s,%.2fNM)\n",
                    tel.in_flight ? 1 : 0, tel.altitude_ft, tel.airspeed_kts, tel.yaw_deg, tel.pitch_deg, tel.roll_deg,
                    tel.entities.size(), tel.ground_units.size(), tel.missiles.size(),
                    tel.view_mode, tel.padlock_has_target ? 1 : 0,
                    tel.padlock_target_name[0] ? tel.padlock_target_name : "-", tel.padlock_range_nm);
        if (tel.in_flight && tel.view_mode == 0) {
            falcon::Cockpit3D test_pit;
            bool ok = machine.harvest_cockpit_views(test_pit);
            std::printf("Cockpit3D harvest test: ok=%d all_4_views=%d end_view_mode=%u\n",
                        ok ? 1 : 0, test_pit.has_all_views() ? 1 : 0,
                        machine.read_flight_telemetry().view_mode);
        }
        std::printf("Weapon: stores_id=%u cannon_selected=%d\n",
                    machine.selected_stores_id(), machine.is_cannon_selected() ? 1 : 0);
        std::printf("VGA CRTC: start=0x%04X stride=%d reg07=0x%02X reg09=0x%02X reg12=0x%02X reg18=0x%02X unchained=%d\n",
                    machine.vga().crtc_start_address(),
                    machine.vga().crtc_reg(0x13) * 2,
                    machine.vga().crtc_reg(0x07),
                    machine.vga().crtc_reg(0x09),
                    machine.vga().crtc_reg(0x12),
                    machine.vga().crtc_reg(0x18),
                    machine.vga().is_unchained() ? 1 : 0);
        std::printf("DAC[0..15]: ");
        for (int i = 0; i < 16; ++i) {
            uint8_t r, g, b;
            machine.vga().get_dac_color(static_cast<uint8_t>(i), r, g, b);
            std::printf("%d:(%d,%d,%d) ", i, r, g, b);
        }
        std::printf("\n");
        std::vector<uint32_t> rgba(falcon::VgaDevice::WIDTH * falcon::VgaDevice::MAX_HEIGHT);
        machine.render_vga_rgba_400(rgba.data());
        std::FILE* ppm = std::fopen("headless_frame.ppm", "wb");
        if (ppm) {
            std::fprintf(ppm, "P6\n%d %d\n255\n", falcon::VgaDevice::WIDTH, falcon::VgaDevice::MAX_HEIGHT);
            for (uint32_t px : rgba) {
                uint8_t rgb[3] = {
                    static_cast<uint8_t>(px & 0xFF),
                    static_cast<uint8_t>((px >> 8) & 0xFF),
                    static_cast<uint8_t>((px >> 16) & 0xFF)
                };
                std::fwrite(rgb, 1, 3, ppm);
            }
            std::fclose(ppm);
        }
        if (!dump_ram_path.empty()) {
            std::FILE* rf = std::fopen(dump_ram_path.c_str(), "wb");
            if (rf) {
                std::fwrite(machine.raw_memory(), 1, 0xA0000, rf);
                std::fclose(rf);
                std::printf("Dumped 640KB RAM to %s\n", dump_ram_path.c_str());
            }
        }
        if (!dump_vga_path.empty()) {
            std::vector<uint8_t> idx(falcon::VgaDevice::WIDTH * 200);
            machine.vga().render_indices_200(idx.data());
            std::FILE* vf = std::fopen(dump_vga_path.c_str(), "wb");
            if (vf) {
                std::fwrite(machine.vga().dac_palette(), 1, 768, vf);
                std::fwrite(idx.data(), 1, idx.size(), vf);
                std::fclose(vf);
                std::printf("Dumped VGA palette+indices to %s\n", dump_vga_path.c_str());
            }
        }
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK)) {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("Falcon 3.0 Native Host", 1280, 800,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    falcon::AudioEngine audio;
    audio.config().cm32l_rom_dir = rom_dir;

    falcon::HotasMapper hotas;
    hotas.init();
    if (!hotas.load_profile(hotas_path)) {
        hotas.load_profile("falcon3_hotas.json");
    }

    falcon::NetUdp net;
    net.init();

    falcon::HeadTracker head_tracker;
    falcon::ModernRenderer renderer;
    falcon::DosMachine machine(audio, hotas, net);
    falcon::F16FlightModel flight_model;

    int init_win_w = 1280, init_win_h = 800;
    bool show_launcher_on_startup = true;
    std::string last_exe = "FALCONCD.EXE";
    load_host_settings(config_path, explicit_rom_dir, audio, renderer, flight_model, machine, head_tracker,
                       init_win_w, init_win_h, show_launcher_on_startup, last_exe);
    if (!explicit_exe && !last_exe.empty()) {
        exe_name = last_exe;
    }
    if (init_win_w != 1280 || init_win_h != 800) {
        SDL_SetWindowSize(window, init_win_w, init_win_h);
    }
    if (audio.config().cm32l_rom_dir.empty() || !has_mt32_roms_in(audio.config().cm32l_rom_dir)) {
        audio.config().cm32l_rom_dir = rom_dir;
    } else {
        rom_dir = audio.config().cm32l_rom_dir;
    }

    auto save_settings_now = [&]() -> bool {
        int cw = 1280, ch = 800;
        SDL_GetWindowSize(window, &cw, &ch);
        cfg_dir = resolve_config_dir(game_dir);
        config_path = (cfg_dir / "falcon3_config.json").string();
        return save_host_settings(config_path, game_dir, audio, renderer, flight_model, machine, head_tracker,
                                  cw, ch, show_launcher_on_startup, exe_name);
    };

    bool missing_game = resolve_falcon_dir(game_dir).empty() || !has_exe_in_dir(game_dir, exe_name.c_str());
    bool open_startup_launcher = force_launcher || missing_game ||
                                 (show_launcher_on_startup && !explicit_exe && !skip_launcher_cli);
    if (open_startup_launcher) {
        LauncherOutcome lo = run_trilogy_launcher(window, false, game_dir, rom_dir, exe_name,
                                                  show_launcher_on_startup, audio, renderer, machine, head_tracker);
        if (lo == LauncherOutcome::Quit) {
            save_settings_now();
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            SDL_GL_DestroyContext(gl_ctx);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        save_settings_now();
    }

    audio.config().cm32l_rom_dir = rom_dir;
    audio.init();
    if (head_tracker.config().enabled) {
        head_tracker.init(head_tracker.config().udp_port);
    }

    auto load_base_assets = [&](const std::string& gdir) {
        renderer.load_theater_map((fs::path(gdir) / "KUWAIT.MAP").string());
        renderer.load_alt_terrain((fs::path(gdir) / "ALT.IRQ").string());
        renderer.load_isldat((fs::path(gdir) / "ISLDAT.IRQ").string());
        renderer.load_objects((fs::path(gdir) / "objects").string());
        renderer.load_mapobj_models((fs::path(gdir) / "mapobj").string());
        renderer.load_mapobj_world((fs::path(gdir) / "MAPOBJ.IRQ").string());
        renderer.load_ils_runways((fs::path(gdir) / "ILS.IRQ").string());
    };
    load_base_assets(game_dir);

    SDL_SetWindowTitle(window, window_title_for_exe(exe_name));
    if (!machine.load_exe(game_dir, exe_name)) {
        std::string msg = "Could not find " + exe_name + " in:\n" + game_dir +
                          "\n\nPass your GOG Falcon Gold folder with:\n"
                          "  falcon30_native.exe --game-dir \"C:\\GOG Games\\Falcon Gold\"\n"
                          "or set the FALCON3_GAME_DIR environment variable.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Falcon 3.0 Native Host — Game Files Not Found",
                                 msg.c_str(), window);
    }

    bool return_to_launcher_requested = false;
    falcon::OverlayUI ui(machine, audio, hotas, net, renderer, flight_model, head_tracker,
                         save_settings_now,
                         [&]() { return_to_launcher_requested = true; },
                         &show_launcher_on_startup);

    // OpenGL texture for 320x400 VGA framebuffer (supports both 320x200 and 320x400 Mode X!)
    GLuint vga_tex = 0, vga_cockpit_tex = 0, vga_hud_tex = 0;
    glGenTextures(1, &vga_tex);
    glBindTexture(GL_TEXTURE_2D, vga_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    std::vector<uint32_t> rgba(falcon::VgaDevice::WIDTH * falcon::VgaDevice::MAX_HEIGHT, 0xFF000000u);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, falcon::VgaDevice::WIDTH, falcon::VgaDevice::MAX_HEIGHT,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // Palette-separated 320x200 Cockpit+HUD and HUD-only RGBA textures for 1:1 HD 3D compositing!
    std::vector<uint32_t> cockpit_rgba(falcon::VgaDevice::WIDTH * 200, 0x00000000u);
    std::vector<uint32_t> hud_rgba(falcon::VgaDevice::WIDTH * 200, 0x00000000u);
    auto init_layer_tex = [&](GLuint& tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, falcon::VgaDevice::WIDTH, 200,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, cockpit_rgba.data());
    };
    init_layer_tex(vga_cockpit_tex);
    init_layer_tex(vga_hud_tex);

    uint64_t prev_perf = SDL_GetPerformanceCounter();
    const double perf_freq = static_cast<double>(SDL_GetPerformanceFrequency());

    bool running = true;
    bool prev_in_flight = false;
    bool prev_fm_active = false;
    float kbd_roll_raw = 0.0f, kbd_pitch_raw = 0.0f, kbd_rud_raw = 0.0f;
    // Seamless dual HOTAS + Keyboard live handoff state (overrides Falcon 3.0's legacy 1991 CONFIG!)
    bool seamless_stick_hotas = true;
    bool seamless_thr_hotas = true;
    float thr_handoff_ref = 0.5f;
    int last_applied_dos_joy_mode = -1;
    // Direct Chaff/Flare & Tracer + 3D Doppler flyby tracking
    bool pending_chaff_fx = false;
    bool pending_flare_fx = false;
    double last_chaff_time = -10.0;
    double last_flare_time = -10.0;
    double last_tracer_time = -10.0;
    double sim_clock_sec = 0.0;
    float prev_nearby_dist_km = -1.0f;
    uint16_t prev_nearby_id = 0xFFFFu;
    // 360-deg 3D Virtual Cockpit sprite harvest & smooth Right-Mouse free-look state
    bool cockpit_harvested = false;
    int in_flight_frames = 0;
    bool rmb_freelook = false;
    float rmb_last_x = 0.0f, rmb_last_y = 0.0f;
    // Host-driven Native 3D Padlock ('8'): keeps DOS in View '3' (Forward Cockpit) so Falcon's
    // 100% REAL live 2D HUD + MFD radar + RWR render right onto the 3D Virtual Cockpit glass!
    bool native_padlock_active = false;
    uint8_t native_padlock_kind = 0;   // 1=Missile/SAM, 2=Aircraft, 3=Ground
    uint32_t native_padlock_id = 0;    // Sticky target identifier
    int native_padlock_step = 0;       // +1 next threat, -1 previous threat
    int native_padlock_grace = 0;      // Grace frames while DOS settles into view_mode == 0
    bool suppress_key8_up = false;
    int space_missile_release_frames = 0; // Single-shot clean tap for Spacebar when missile/bomb selected!
    bool suppress_space_up = false;
    std::array<bool, 256> prev_missile_slots{};
    std::string auto_loaded_theater_ext = "IRQ";

    while (running) {
        if (return_to_launcher_requested) {
            return_to_launcher_requested = false;
            std::string prev_game_dir = game_dir;
            std::string prev_rom_dir = rom_dir;
            std::string chosen_exe = machine.current_exe();
            LauncherOutcome lo = run_trilogy_launcher(window, true, game_dir, rom_dir, chosen_exe,
                                                      show_launcher_on_startup, audio, renderer, machine, head_tracker);
            if (lo == LauncherOutcome::Quit) {
                running = false;
                break;
            }
            if (lo == LauncherOutcome::Launch) {
                exe_name = chosen_exe;
                if (game_dir != prev_game_dir) {
                    load_base_assets(game_dir);
                    auto_loaded_theater_ext = "IRQ";
                }
                if (rom_dir != prev_rom_dir) {
                    audio.config().cm32l_rom_dir = rom_dir;
                    audio.load_mt32_roms(rom_dir);
                }
                if (head_tracker.config().enabled && !head_tracker.is_listening()) {
                    head_tracker.init(head_tracker.config().udp_port);
                } else if (!head_tracker.config().enabled && head_tracker.is_listening()) {
                    head_tracker.shutdown();
                }
                SDL_SetWindowTitle(window, window_title_for_exe(exe_name));
                machine.load_exe(game_dir, exe_name);
                cockpit_harvested = false;
                in_flight_frames = 0;
                prev_in_flight = false;
                native_padlock_active = false;
                save_settings_now();
            } else {
                SDL_SetWindowTitle(window, window_title_for_exe(machine.current_exe()));
            }
            prev_perf = SDL_GetPerformanceCounter();
            continue;
        }

        uint64_t now_perf = SDL_GetPerformanceCounter();
        double dt_sec = static_cast<double>(now_perf - prev_perf) / perf_freq;
        prev_perf = now_perf;

        bool fm_capture_stick = (flight_model.config().enabled && prev_in_flight);
        if (fm_capture_stick && !prev_fm_active) {
            // Release any held DOS flight-stick keys so Falcon's internal 1991 physics stops steering!
            const uint8_t stick_scs[] = {0x48, 0x50, 0x4B, 0x4D, 0x1A, 0x1B};
            for (uint8_t sc : stick_scs) machine.push_key(sc, 0, false);
            kbd_roll_raw = kbd_pitch_raw = kbd_rud_raw = 0.0f;
        }
        prev_fm_active = fm_capture_stick;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            hotas.handle_sdl_event(ev);

            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F12) {
                ui.toggle();
            } else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F11) {
                int next = (static_cast<int>(renderer.mode()) + 1) % 4;
                renderer.mode() = static_cast<falcon::RenderMode>(next);
            } else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F9) {
                head_tracker.recenter();
                renderer.vr_config().head_yaw_deg = 0.0f;
                renderer.vr_config().head_pitch_deg = 0.0f;
            } else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
                if (!ImGui::GetIO().WantCaptureKeyboard) {
                    // When Native F-16C Flight Model is active in flight, intercept flight-stick & rudder keys
                    // (Arrows + [/]) so Falcon's 1991 physics doesn't fight our 240 Hz C++ FLCS!
                    // Leave , / . / / un-intercepted so Falcon's real Afterburner stages (AB1..AB5) still work!
                    bool is_stick_key = (ev.key.scancode == SDL_SCANCODE_UP ||
                                         ev.key.scancode == SDL_SCANCODE_DOWN ||
                                         ev.key.scancode == SDL_SCANCODE_LEFT ||
                                         ev.key.scancode == SDL_SCANCODE_RIGHT ||
                                         ev.key.scancode == SDL_SCANCODE_LEFTBRACKET ||
                                         ev.key.scancode == SDL_SCANCODE_RIGHTBRACKET);
                    bool is_thr_key = (ev.key.scancode == SDL_SCANCODE_EQUALS ||
                                       ev.key.scancode == SDL_SCANCODE_MINUS ||
                                       ev.key.scancode == SDL_SCANCODE_KP_PLUS ||
                                       ev.key.scancode == SDL_SCANCODE_KP_MINUS ||
                                       ev.key.scancode == SDL_SCANCODE_SLASH ||
                                       ev.key.scancode == SDL_SCANCODE_COMMA ||
                                       ev.key.scancode == SDL_SCANCODE_PERIOD);
                    // In AutoSeamless mode, real keyboard flight/throttle keys seamlessly hand control to Keyboard;
                    // moving the physical HOTAS stick or throttle lever immediately hands it back to HOTAS!
                    if (ev.type == SDL_EVENT_KEY_DOWN && prev_in_flight &&
                        machine.flight_input_mode() == falcon::FlightInputMode::AutoSeamless) {
                        if (is_stick_key) {
                            seamless_stick_hotas = false;
                        }
                        if (is_thr_key) {
                            seamless_thr_hotas = false;
                            thr_handoff_ref = hotas.axis_value(falcon::FlightAxis::Throttle);
                        }
                    }
                    bool suppress_dos_key = false;
                    bool native_padlock_eligible = (prev_in_flight && !machine.paused() &&
                                                    renderer.mode() != falcon::RenderMode::ClassicVga &&
                                                    renderer.vr_config().enhanced_3d_padlock);
                    bool no_ctrl_alt = ((ev.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) == 0);
                    if (ev.key.scancode == SDL_SCANCODE_8 && no_ctrl_alt) {
                        if (ev.type == SDL_EVENT_KEY_DOWN && native_padlock_eligible) {
                            if (!ev.key.repeat) {
                                bool shift_held = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                                if (!native_padlock_active) {
                                    native_padlock_active = true;
                                    native_padlock_kind = 0;
                                    native_padlock_step = 0;
                                } else {
                                    native_padlock_step += shift_held ? -1 : +1;
                                }
                                renderer.vr_config().head_yaw_deg = 0.0f;
                                renderer.vr_config().head_pitch_deg = 0.0f;
                                native_padlock_grace = 15;
                            }
                            suppress_key8_up = true;
                            suppress_dos_key = true;
                        } else if (ev.type == SDL_EVENT_KEY_UP && suppress_key8_up) {
                            suppress_key8_up = false;
                            suppress_dos_key = true;
                        }
                    }
                    if (ev.type == SDL_EVENT_KEY_DOWN && prev_in_flight && !machine.paused()) {
                        if (ev.key.scancode == SDL_SCANCODE_INSERT) pending_chaff_fx = true;
                        if (ev.key.scancode == SDL_SCANCODE_DELETE) pending_flare_fx = true;
                        bool is_other_view_key = no_ctrl_alt &&
                            (ev.key.scancode == SDL_SCANCODE_1 || ev.key.scancode == SDL_SCANCODE_2 ||
                             ev.key.scancode == SDL_SCANCODE_3 || ev.key.scancode == SDL_SCANCODE_4 ||
                             ev.key.scancode == SDL_SCANCODE_5 || ev.key.scancode == SDL_SCANCODE_6 ||
                             ev.key.scancode == SDL_SCANCODE_7 || ev.key.scancode == SDL_SCANCODE_9 ||
                             ev.key.scancode == SDL_SCANCODE_0);
                        if (is_other_view_key) {
                            native_padlock_active = false;
                            native_padlock_kind = 0;
                            native_padlock_step = 0;
                            native_padlock_grace = 0;
                            renderer.vr_config().head_yaw_deg = 0.0f;
                            renderer.vr_config().head_pitch_deg = 0.0f;
                        }
                    }
                    if (ev.key.scancode == SDL_SCANCODE_SPACE && prev_in_flight && !machine.paused()) {
                        if (ev.type == SDL_EVENT_KEY_DOWN) {
                            if (ev.key.repeat) {
                                // Never flood DOS with OS key-repeat on the trigger!
                                suppress_dos_key = true;
                            } else if (!machine.is_cannon_selected()) {
                                // Single crisp press -> auto-release after 2 frames so 20-MIPS turbo
                                // fires EXACTLY ONE missile/bomb per Spacebar press!
                                space_missile_release_frames = 2;
                                suppress_space_up = true;
                            }
                        } else if (ev.type == SDL_EVENT_KEY_UP && suppress_space_up) {
                            suppress_space_up = false;
                            if (space_missile_release_frames == 0) {
                                suppress_dos_key = true; // Already auto-released cleanly!
                            } else {
                                space_missile_release_frames = 0;
                            }
                        }
                    }
                    if (!suppress_dos_key && !(fm_capture_stick && is_stick_key)) {
                        uint8_t sc = sdl_scancode_to_dos(ev.key.scancode);
                        uint8_t ascii = (ev.key.key < 128) ? static_cast<uint8_t>(ev.key.key) : 0;
                        if (sc != 0) {
                            machine.push_key(sc, ascii, ev.type == SDL_EVENT_KEY_DOWN);
                        }
                    }
                }
            }
        }

        // Update mouse for DOS INT 33h mapped accurately to the 4:3 letterboxed VGA viewport
        int win_w = 1, win_h = 1;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (!ImGui::GetIO().WantCaptureMouse) {
            float win_aspect = static_cast<float>(win_w) / static_cast<float>(std::max(1, win_h));
            float target_aspect = 4.0f / 3.0f;
            float vx = 0.0f, vy = 0.0f, vw = static_cast<float>(win_w), vh = static_cast<float>(win_h);
            if (win_aspect > target_aspect) {
                vw = static_cast<float>(win_h) * target_aspect;
                vx = (static_cast<float>(win_w) - vw) * 0.5f;
            } else {
                vh = static_cast<float>(win_w) / target_aspect;
                vy = (static_cast<float>(win_h) - vh) * 0.5f;
            }
            float mx = 0, my = 0;
            SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
            // Smooth 360-deg 3D Virtual Cockpit Free-Look via Right-Mouse Drag (Middle-Click recenters!)
            if (prev_in_flight && renderer.mode() != falcon::RenderMode::ClassicVga &&
                renderer.vr_config().virtual_cockpit_freelook) {
                if (mb & SDL_BUTTON_MMASK) {
                    renderer.vr_config().head_yaw_deg = 0.0f;
                    renderer.vr_config().head_pitch_deg = 0.0f;
                }
                if (mb & SDL_BUTTON_RMASK) {
                    if (rmb_freelook) {
                        float dx_m = mx - rmb_last_x;
                        float dy_m = my - rmb_last_y;
                        auto& vr = renderer.vr_config();
                        vr.head_yaw_deg = std::clamp(vr.head_yaw_deg + dx_m * 0.28f, -175.0f, 175.0f);
                        vr.head_pitch_deg = std::clamp(vr.head_pitch_deg - dy_m * 0.24f, -68.0f, 82.0f);
                    }
                    rmb_freelook = true;
                    rmb_last_x = mx;
                    rmb_last_y = my;
                } else {
                    rmb_freelook = false;
                }
            } else {
                rmb_freelook = false;
            }
            int dx = static_cast<int>(((mx - vx) / std::max(1.0f, vw)) * 320.0f);
            int dy = static_cast<int>(((my - vy) / std::max(1.0f, vh)) * 200.0f);
            uint8_t btns = ((mb & SDL_BUTTON_LMASK) ? 1 : 0) | ((mb & SDL_BUTTON_RMASK) ? 2 : 0);
            machine.set_mouse_state(dx, dy, btns);
        }

        // Update multi-HOTAS mapper -> inject keys into DOS machine
        hotas.update([&](uint8_t sc, uint8_t ascii, bool pressed) {
            bool native_padlock_eligible = (prev_in_flight && !machine.paused() &&
                                            renderer.mode() != falcon::RenderMode::ClassicVga &&
                                            renderer.vr_config().enhanced_3d_padlock);
            if (sc == 0x09 && native_padlock_eligible) { // HOTAS Padlock ('8')
                if (pressed) {
                    if (!native_padlock_active) {
                        native_padlock_active = true;
                        native_padlock_kind = 0;
                        native_padlock_step = 0;
                    } else {
                        native_padlock_step += 1;
                    }
                    renderer.vr_config().head_yaw_deg = 0.0f;
                    renderer.vr_config().head_pitch_deg = 0.0f;
                    native_padlock_grace = 15;
                }
                return;
            }
            if (pressed && prev_in_flight && !machine.paused()) {
                if (sc == 0x52) pending_chaff_fx = true; // Insert = Chaff
                if (sc == 0x53) pending_flare_fx = true; // Delete = Flare
                if ((sc >= 0x02 && sc <= 0x08) || sc == 0x0A || sc == 0x0B) {
                    native_padlock_active = false;
                    native_padlock_kind = 0;
                    native_padlock_step = 0;
                    native_padlock_grace = 0;
                    renderer.vr_config().head_yaw_deg = 0.0f;
                    renderer.vr_config().head_pitch_deg = 0.0f;
                }
            }
            machine.push_key(sc, ascii, pressed);
        });
        if (hotas.consume_axis_bindings_changed() && machine.auto_bypass_joy_cal()) {
            seamless_stick_hotas = true;
            seamless_thr_hotas = true;
            machine.apply_auto_joystick_calibration(true);
            last_applied_dos_joy_mode = -1;
        }

        // Authoritative Flight Input arbitration (overrides Falcon's 1991 CONFIG seamlessly!)
        bool has_hotas_stick = hotas.is_axis_active(falcon::FlightAxis::Roll) ||
                               hotas.is_axis_active(falcon::FlightAxis::Pitch);
        bool has_hotas_yaw   = hotas.is_axis_active(falcon::FlightAxis::Yaw);
        bool has_hotas_thr   = hotas.is_axis_active(falcon::FlightAxis::Throttle);
        bool use_hotas_stick = false;
        bool use_hotas_thr   = false;

        if (machine.flight_input_mode() == falcon::FlightInputMode::ForceHotas) {
            use_hotas_stick = has_hotas_stick || has_hotas_yaw;
            use_hotas_thr   = has_hotas_thr;
        } else if (machine.flight_input_mode() == falcon::FlightInputMode::AutoSeamless) {
            if (has_hotas_stick || has_hotas_yaw) {
                float hr = hotas.axis_value(falcon::FlightAxis::Roll);
                float hp = hotas.axis_value(falcon::FlightAxis::Pitch);
                float hy = hotas.axis_value(falcon::FlightAxis::Yaw);
                if (std::abs(hr) > 0.12f || std::abs(hp) > 0.12f || std::abs(hy) > 0.15f) {
                    seamless_stick_hotas = true;
                }
            } else {
                seamless_stick_hotas = false;
            }
            if (has_hotas_thr) {
                float ht = hotas.axis_value(falcon::FlightAxis::Throttle);
                if (!seamless_thr_hotas && std::abs(ht - thr_handoff_ref) > 0.025f) {
                    seamless_thr_hotas = true;
                }
            } else {
                seamless_thr_hotas = false;
            }
            use_hotas_stick = seamless_stick_hotas && (has_hotas_stick || has_hotas_yaw);
            use_hotas_thr   = seamless_thr_hotas && has_hotas_thr;
        }

        // Compute desired DOS gameport mode (0=Keyboard, 1=XY, 2=XY+Yaw, 3=XY+Thr, 4=XY+Yaw+Thr)
        // and sync live to Falcon's in-RAM joystick driver instantaneously with zero menu diving!
        int desired_dos_mode = 0;
        if (use_hotas_stick && use_hotas_thr) {
            desired_dos_mode = has_hotas_yaw ? 4 : 3;
        } else if (use_hotas_stick) {
            desired_dos_mode = has_hotas_yaw ? 2 : 1;
        } else if (use_hotas_thr) {
            desired_dos_mode = 3;
        }

        // Step Optional Native F-16C Block 30/40 Flight Model BEFORE DOS step_realtime so Falcon's
        // live HUD, velocity vector, pitch ladder & radar draw our exact up-to-date 6-DOF state!
        falcon::FlightTelemetry pre_tel = machine.read_flight_telemetry();
        if (pre_tel.in_flight && desired_dos_mode != last_applied_dos_joy_mode) {
            machine.apply_auto_joystick_calibration(false, desired_dos_mode);
            last_applied_dos_joy_mode = desired_dos_mode;
        }
        bool fm_stepped = false;
        bool fm_sync_fwd_cam = false;
        bool fm_write_thr = false;

        if (!pre_tel.in_flight) {
            flight_model.reset();
            kbd_roll_raw = kbd_pitch_raw = kbd_rud_raw = 0.0f;
            last_applied_dos_joy_mode = -1;
        } else if (flight_model.config().enabled && !machine.paused()) {
            float weight_lbs = 21500.0f, falc_thr = 0.80f;
            machine.read_player_fm_extras(weight_lbs, falc_thr);
            if (!flight_model.is_initialized()) {
                flight_model.sync_from_falcon(pre_tel.world_x, pre_tel.world_y, pre_tel.altitude_ft, pre_tel.agl_ft,
                                              pre_tel.yaw_deg, pre_tel.pitch_deg, pre_tel.roll_deg,
                                              pre_tel.airspeed_kts, weight_lbs, falc_thr);
            }

            falcon::F16ControlInput ctrl{};
            const bool* keys = SDL_GetKeyboardState(nullptr);
            bool kbd_ok = !ImGui::GetIO().WantCaptureKeyboard;
            float dt_f = static_cast<float>(std::clamp(dt_sec, 0.001, 0.05));

            // Smooth progressive virtual-stick ramp for keyboard (precise on small taps, full 9G/240 dps on hold!)
            auto ramp_axis = [&](float& val, int dir, float attack_rate, float return_rate) -> float {
                if (dir != 0) {
                    // Instant crisp reversal when pressing the opposite key
                    if ((dir > 0 && val < 0.0f) || (dir < 0 && val > 0.0f)) val = 0.0f;
                    val = std::clamp(val + static_cast<float>(dir) * attack_rate * dt_f, -1.0f, 1.0f);
                } else {
                    float step = return_rate * dt_f;
                    if (std::abs(val) <= step) val = 0.0f;
                    else val -= (val > 0.0f ? step : -step);
                }
                float a = std::abs(val);
                float shaped = 0.18f * a + 0.82f * a * a;
                return (val >= 0.0f) ? shaped : -shaped;
            };

            int dir_roll = 0, dir_pitch = 0, dir_rud = 0;
            if (kbd_ok) {
                if (keys[SDL_SCANCODE_LEFT])         dir_roll -= 1;
                if (keys[SDL_SCANCODE_RIGHT])        dir_roll += 1;
                if (keys[SDL_SCANCODE_DOWN])         dir_pitch += 1; // Pull stick back -> nose up / pull G
                if (keys[SDL_SCANCODE_UP])           dir_pitch -= 1; // Push stick forward -> nose down
                if (keys[SDL_SCANCODE_LEFTBRACKET])  dir_rud -= 1;   // Official Falcon 3.0 [ left rudder
                if (keys[SDL_SCANCODE_RIGHTBRACKET]) dir_rud += 1;   // Official Falcon 3.0 ] right rudder
                ctrl.speedbrake = keys[SDL_SCANCODE_B];
            }
            float k_roll  = ramp_axis(kbd_roll_raw,  dir_roll,  2.4f, 7.0f);
            float k_pitch = ramp_axis(kbd_pitch_raw, dir_pitch, 2.2f, 6.5f);
            float k_rud   = ramp_axis(kbd_rud_raw,   dir_rud,   2.8f, 7.0f);

            ctrl.stick_roll  = (use_hotas_stick && hotas.is_axis_active(falcon::FlightAxis::Roll))
                               ? hotas.axis_value(falcon::FlightAxis::Roll) : k_roll;
            ctrl.stick_pitch = (use_hotas_stick && hotas.is_axis_active(falcon::FlightAxis::Pitch))
                               ? hotas.axis_value(falcon::FlightAxis::Pitch) : k_pitch;
            ctrl.rudder      = (use_hotas_stick && hotas.is_axis_active(falcon::FlightAxis::Yaw))
                               ? hotas.axis_value(falcon::FlightAxis::Yaw) : k_rud;

            if (use_hotas_thr && hotas.is_axis_active(falcon::FlightAxis::Throttle)) {
                ctrl.has_analog_throttle = true;
                ctrl.throttle = hotas.axis_value(falcon::FlightAxis::Throttle);
                fm_write_thr = true;
            } else {
                // Follow Falcon's own live throttle (0x4737C) so =/-/A/1..0, RPM dial & AB audio stay 100% synced!
                ctrl.has_analog_throttle = true;
                ctrl.throttle = falc_thr;
                fm_write_thr = false;
            }

            flight_model.step(dt_sec, ctrl);
            float dyaw = std::abs(pre_tel.cam_yaw_deg - pre_tel.yaw_deg);
            if (dyaw > 180.0f) dyaw = 360.0f - dyaw;
            fm_sync_fwd_cam = (!pre_tel.is_external_cam && dyaw < 15.0f &&
                               std::abs(pre_tel.cam_pitch_deg - pre_tel.pitch_deg) < 15.0f);
            machine.write_player_flight_state(flight_model.state(), fm_sync_fwd_cam, fm_write_thr);
            fm_stepped = true;
        }

        // Auto-release single-shot Spacebar missile tap so holding Spacebar never ripples a whole salvo!
        if (space_missile_release_frames > 0) {
            if (--space_missile_release_frames == 0) {
                machine.push_key(0x39, ' ', false);
            }
        }

        // Step DOS CPU decoupled from monitor refresh rate (true wall-clock real time!)
        machine.step_realtime(dt_sec);

        // Auto-sync 3D theater terrain, islands, roads, buildings & ILS runways whenever Falcon loads a theater!
        if (!machine.detected_theater_ext().empty() &&
            machine.detected_theater_ext() != auto_loaded_theater_ext) {
            auto_loaded_theater_ext = machine.detected_theater_ext();
            const std::string& ext = auto_loaded_theater_ext;
            std::string map_name = "KUWAIT.MAP";
            if (ext == "BOS") map_name = "BOSNIA.MAP";
            else if (ext == "ISR") map_name = "ISRAEL.MAP";
            else if (ext == "KOR") map_name = "KOREA.MAP";
            else if (ext == "PAN") map_name = "PANAMA.MAP";
            else if (ext == "NEL") map_name = "NELLIS.MAP";
            else if (ext == "HOK") map_name = "JAPAN.MAP";
            else if (ext == "PAK") map_name = "INDIA.MAP";
            fs::path gdir = machine.game_dir();
            renderer.load_theater_map((gdir / map_name).string());
            renderer.load_alt_terrain((gdir / ("ALT." + ext)).string());
            renderer.load_isldat((gdir / ("ISLDAT." + ext)).string());
            renderer.load_mapobj_world((gdir / ("MAPOBJ." + ext)).string());
            renderer.load_ils_runways((gdir / ("ILS." + ext)).string());
        }

        // Keep exe_name & window title synced if user switched title via F12 -> Title menu
        if (!machine.current_exe().empty() && machine.current_exe() != exe_name) {
            exe_name = machine.current_exe();
            SDL_SetWindowTitle(window, window_title_for_exe(exe_name));
            cockpit_harvested = false;
            renderer.cockpit_3d().clear_views();
        }

        // Re-assert native 6-DOF state after DOS step so Falcon's 1991 physics never drifts our state
        if (fm_stepped) {
            machine.write_player_flight_state(flight_model.state(), fm_sync_fwd_cam, fm_write_thr);
        }
        falcon::FlightTelemetry tel = machine.read_flight_telemetry();
        sim_clock_sec += std::clamp(dt_sec, 0.0005, 0.05);
        if (tel.in_flight && !prev_in_flight) {
            // Ensure SAM Realism Mod & authoritative flight input state are active in RAM on entering 3D combat
            machine.apply_sam_mod_in_ram();
            last_applied_dos_joy_mode = -1;
            prev_nearby_dist_km = -1.0f;
            cockpit_harvested = false;
            in_flight_frames = 0;
        }
        if (tel.in_flight) {
            in_flight_frames++;
            // Wait until Falcon has finished its black screen fade-in and rendered the live Forward Cockpit
            // (harvest_cockpit_views() validates >=8k world + >=10k non-black cockpit pixels before accepting!)
            if (!cockpit_harvested && in_flight_frames >= 12 && tel.view_mode == 0) {
                if (machine.harvest_cockpit_views(renderer.cockpit_3d())) {
                    cockpit_harvested = true;
                    tel = machine.read_flight_telemetry();
                }
            }
        } else {
            in_flight_frames = 0;
            cockpit_harvested = false;
            native_padlock_active = false;
            native_padlock_kind = 0;
            native_padlock_step = 0;
            native_padlock_grace = 0;
            prev_missile_slots.fill(false);
            renderer.cockpit_3d().clear_views();
        }
        prev_in_flight = tel.in_flight;

        // Host-driven Native 3D Padlock ('8'): keeps DOS in View '3' (Forward Cockpit) so Falcon's
        // 100% REAL live 2D HUD + MFD radar + RWR draw straight onto the 3D Virtual Cockpit glass!
        if (!tel.in_flight || renderer.mode() == falcon::RenderMode::ClassicVga ||
            !renderer.vr_config().enhanced_3d_padlock) {
            native_padlock_active = false;
            native_padlock_kind = 0;
            native_padlock_step = 0;
            native_padlock_grace = 0;
        } else if (native_padlock_active) {
            if (tel.view_mode != 0) {
                if (native_padlock_grace > 0) {
                    if (native_padlock_grace == 15) {
                        // Return DOS to Forward Cockpit ('3') so live 2D HUD + MFD + RWR keep updating!
                        machine.push_key(0x04, '3', true);
                        machine.push_key(0x04, '3', false);
                    }
                    native_padlock_grace--;
                } else {
                    native_padlock_active = false;
                    native_padlock_kind = 0;
                    native_padlock_step = 0;
                }
            } else {
                native_padlock_grace = 0;
            }
        }
        if (native_padlock_active && tel.in_flight) {
            struct NativeCandidate {
                uint8_t kind = 0;   // 1=Missile/SAM, 2=Aircraft, 3=Ground
                uint32_t id = 0;
                double wx = 0.0, wy = 0.0, wz = 0.0;
                float range_nm = 0.0f;
                bool is_missile = false;
                std::string name;
            };
            constexpr double kHalfFtPerNm = 12152.23;
            double player_z_hf = static_cast<double>(tel.altitude_ft) * 2.0;
            auto calc_nm = [&](double wx, double wy, double wz) -> float {
                double dn = wx - tel.world_x;
                double de = wy - tel.world_y;
                double dz = wz - player_z_hf;
                return static_cast<float>(std::sqrt(dn * dn + de * de + dz * dz) / kHalfFtPerNm);
            };

            std::vector<NativeCandidate> cands;
            cands.reserve(tel.missiles.size() + tel.entities.size() + 16);
            for (const auto& m : tel.missiles) {
                float nm = calc_nm(m.world_x, m.world_y, m.world_z);
                if (nm <= 18.0f) {
                    cands.push_back({1, static_cast<uint32_t>(m.slot_id), m.world_x, m.world_y, m.world_z,
                                     nm, true, m.is_sam ? "SAM (R)" : "MISSILE"});
                }
            }
            for (size_t i = 0; i < tel.entities.size(); ++i) {
                const auto& ent = tel.entities[i];
                if (ent.is_player) continue;
                float nm = calc_nm(ent.world_x, ent.world_y, ent.world_z);
                if (nm <= 35.0f) {
                    uint32_t aid = (ent.flight_id != 0)
                                   ? ((static_cast<uint32_t>(ent.flight_id) << 16) | static_cast<uint32_t>(ent.type_id))
                                   : static_cast<uint32_t>(0x80000000u | i);
                    cands.push_back({2, aid, ent.world_x, ent.world_y, ent.world_z,
                                     nm, false, renderer.entity_type_name(ent.type_id, false)});
                }
            }
            for (size_t i = 0; i < tel.ground_units.size(); ++i) {
                const auto& g = tel.ground_units[i];
                float nm = calc_nm(g.world_x, g.world_y, g.world_z);
                if (nm <= 12.0f) {
                    uint32_t gid = (static_cast<uint32_t>(g.flight_id) << 16) | static_cast<uint32_t>(i & 0xFFFFu);
                    cands.push_back({3, gid, g.world_x, g.world_y, g.world_z,
                                     nm, false, renderer.entity_type_name(g.type_id, true)});
                }
            }
            std::sort(cands.begin(), cands.end(), [](const NativeCandidate& a, const NativeCandidate& b) {
                if (a.kind != b.kind) return a.kind < b.kind;
                return a.range_nm < b.range_nm;
            });

            tel.padlock_active = true;
            if (!cands.empty()) {
                int sel = -1;
                if (native_padlock_kind != 0) {
                    for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
                        if (cands[i].kind == native_padlock_kind && cands[i].id == native_padlock_id) {
                            sel = i;
                            break;
                        }
                    }
                }
                if (sel < 0) {
                    sel = 0;
                    native_padlock_step = 0;
                } else if (native_padlock_step != 0) {
                    int n = static_cast<int>(cands.size());
                    sel = ((sel + native_padlock_step) % n + n) % n;
                    native_padlock_step = 0;
                }
                const auto& pick = cands[sel];
                native_padlock_kind = pick.kind;
                native_padlock_id = pick.id;

                tel.padlock_has_target = true;
                tel.padlock_target_is_missile = pick.is_missile;
                tel.padlock_target_x = pick.wx;
                tel.padlock_target_y = pick.wy;
                tel.padlock_target_z = pick.wz;
                tel.padlock_range_nm = pick.range_nm;
                std::memset(tel.padlock_target_name, 0, sizeof(tel.padlock_target_name));
                std::strncpy(tel.padlock_target_name, pick.name.c_str(), sizeof(tel.padlock_target_name) - 1);
            } else {
                native_padlock_kind = 0;
                native_padlock_step = 0;
                tel.padlock_has_target = false;
                tel.padlock_target_is_missile = false;
                tel.padlock_range_nm = 0.0f;
                std::strncpy(tel.padlock_target_name, "NONE", sizeof(tel.padlock_target_name) - 1);
            }
        }

        // Fire 100%-reliable CM-32L Chaff/Flare/Missile-Launch audio + 3D visual countermeasures (immune to DOS 3-slot drops!)
        if (tel.in_flight && !machine.paused()) {
            // Detect newly launched player missiles from RAM (0x42170) within 800 ft of our F-16 -> play CM-32L missileFir (42)!
            std::array<bool, 256> cur_msl{};
            double pz_hf = static_cast<double>(tel.altitude_ft) * 2.0;
            for (const auto& m : tel.missiles) {
                if (m.slot_id < cur_msl.size()) {
                    cur_msl[m.slot_id] = true;
                    if (!prev_missile_slots[m.slot_id] && in_flight_frames > 15) {
                        double dn = m.world_x - tel.world_x;
                        double de = m.world_y - tel.world_y;
                        double dz = m.world_z - pz_hf;
                        if (dn * dn + de * de + dz * dz < 1600.0 * 1600.0) {
                            audio.trigger_missile_launch_sfx();
                        }
                    }
                }
            }
            prev_missile_slots = cur_msl;

            if (pending_chaff_fx && (sim_clock_sec - last_chaff_time) >= 0.25) {
                last_chaff_time = sim_clock_sec;
                audio.trigger_chaff_sfx();
                renderer.spawn_chaff(tel);
            }
            if (pending_flare_fx && (sim_clock_sec - last_flare_time) >= 0.25) {
                last_flare_time = sim_clock_sec;
                audio.trigger_flare_sfx();
                renderer.spawn_flare(tel);
            }
        }
        pending_chaff_fx = false;
        pending_flare_fx = false;

        // Compute live 3D Spatial Audio & Telemetry (Engine/AB/Wind/Doppler Flybys/Combat Pan!)
        {
            falcon::FlightAudioTelemetry fat{};
            fat.in_flight = tel.in_flight;
            fat.paused = machine.paused();
            fat.is_external_cam = tel.is_external_cam;
            fat.airspeed_kts = tel.airspeed_kts;
            if (tel.in_flight) {
                if (fm_stepped) {
                    const auto& st = flight_model.state();
                    fat.rpm_pct = static_cast<float>(st.rpm_pct);
                    fat.throttle_01 = static_cast<float>(st.throttle_cmd);
                    fat.ab_stage = (st.throttle_cmd > 0.78)
                                   ? std::clamp(static_cast<int>(std::lround(1.0 + ((st.throttle_cmd - 0.78) / 0.22) * 4.0)), 1, 5)
                                   : 0;
                    fat.g_load = static_cast<float>(st.g_load);
                } else {
                    machine.read_flight_audio_extras(fat.rpm_pct, fat.throttle_01, fat.ab_stage, fat.g_load);
                }

                // Camera right vector in OpenGL world space (X=East, Y=Up, Z=-North)
                constexpr float kD2R = 0.0174532925f;
                float cy = tel.cam_yaw_deg * kD2R;
                float cp = tel.cam_pitch_deg * kD2R;
                float cr = tel.cam_roll_deg * kD2R;
                // Camera horizontal right (before roll) = (cos(yaw), 0, sin(yaw))
                float rx0 = std::cos(cy), ry0 = 0.0f, rz0 = std::sin(cy);
                // Camera up (before roll) = (-sin(pitch)*sin(yaw), cos(pitch), sin(pitch)*cos(yaw))
                float ux0 = -std::sin(cp) * std::sin(cy), uy0 = std::cos(cp), uz0 = std::sin(cp) * std::cos(cy);
                // Apply camera roll
                float cam_rx = rx0 * std::cos(cr) + ux0 * std::sin(cr);
                float cam_ry = ry0 * std::cos(cr) + uy0 * std::sin(cr);
                float cam_rz = rz0 * std::cos(cr) + uz0 * std::sin(cr);

                constexpr double kHf2Km = 0.0001524;
                float cam_y_km = std::max(0.020f, tel.cam_alt_ft * 0.0003048f);
                float best_jet_km = 999.0f;
                float best_jet_pan = 0.0f;
                uint16_t best_jet_id = 0xFFFFu;
                float nearest_combat_km = 999.0f;

                for (const auto& ent : tel.entities) {
                    float ex = static_cast<float>((ent.world_y - tel.cam_y) * kHf2Km);
                    float ey = static_cast<float>(ent.world_z * kHf2Km) - cam_y_km;
                    float ez = static_cast<float>(-(ent.world_x - tel.cam_x) * kHf2Km);
                    float d = std::sqrt(ex * ex + ey * ey + ez * ez);
                    float inv_d = (d > 1e-4f) ? (1.0f / d) : 0.0f;
                    float pan = std::clamp((ex * cam_rx + ey * cam_ry + ez * cam_rz) * inv_d, -1.0f, 1.0f);
                    if (ent.is_player) {
                        fat.player_ext_pan = pan;
                        fat.player_ext_dist_km = d;
                    } else {
                        if (d < nearest_combat_km) {
                            nearest_combat_km = d;
                            fat.combat_pan = pan;
                        }
                        if (d < best_jet_km) {
                            best_jet_km = d;
                            best_jet_pan = pan;
                            best_jet_id = ent.flight_id;
                        }
                    }
                }
                if (best_jet_km < 2.2f) {
                    fat.has_nearby_jet = true;
                    fat.nearby_jet_dist_km = best_jet_km;
                    fat.nearby_jet_pan = best_jet_pan;
                    if (prev_nearby_dist_km > 0.0f && prev_nearby_id == best_jet_id && dt_sec > 0.001) {
                        float closing_mps = (prev_nearby_dist_km - best_jet_km) * 1000.0f / static_cast<float>(dt_sec);
                        fat.nearby_jet_doppler = std::clamp(1.0f + closing_mps / 340.0f, 0.68f, 1.42f);
                    }
                    prev_nearby_dist_km = best_jet_km;
                    prev_nearby_id = best_jet_id;
                } else {
                    prev_nearby_dist_km = -1.0f;
                }
            }
            audio.update_flight_telemetry(fat);
        }

        // Drive non-blocking polyphonic M61A1 Vulcan cannon audio + 3D tracers ONLY when M61 cannon is selected!
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            bool gun_trigger = tel.in_flight && !machine.paused() && !ImGui::GetIO().WantCaptureKeyboard &&
                               machine.is_cannon_selected() &&
                               (keys[SDL_SCANCODE_SPACE] || ((hotas.gameport_buttons() & 1u) != 0));
            audio.set_gun_firing(gun_trigger);
            if (gun_trigger && (sim_clock_sec - last_tracer_time) >= 0.055) {
                last_tracer_time = sim_clock_sec;
                renderer.spawn_gun_tracer(tel);
            }
        }

        // Pump audio
        audio.update();

        // Update VGA texture (320x400)
        machine.render_vga_rgba_400(rgba.data());
        glBindTexture(GL_TEXTURE_2D, vga_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, falcon::VgaDevice::WIDTH, falcon::VgaDevice::MAX_HEIGHT,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

        // Upload palette-separated Cockpit/HUD overlays & render via selected mode
        if (tel.in_flight && renderer.mode() != falcon::RenderMode::ClassicVga) {
            renderer.set_vga_palette(machine.vga().dac_palette());
            machine.vga().render_overlay_layers_200(cockpit_rgba.data(), hud_rgba.data(),
                                                    &tel.world_y_min, &tel.world_y_max);
            glBindTexture(GL_TEXTURE_2D, vga_cockpit_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, falcon::VgaDevice::WIDTH, 200,
                            GL_RGBA, GL_UNSIGNED_BYTE, cockpit_rgba.data());
            glBindTexture(GL_TEXTURE_2D, vga_hud_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, falcon::VgaDevice::WIDTH, 200,
                            GL_RGBA, GL_UNSIGNED_BYTE, hud_rgba.data());
        }
        renderer.render_frame(win_w, win_h, vga_tex, vga_cockpit_tex, vga_hud_tex, tel, static_cast<float>(dt_sec));

        // Poll OpenTrack / TrackIR UDP 4242 head tracker (feeds 360-deg 3D Virtual Cockpit Free-Look when connected!)
        head_tracker.poll(sim_clock_sec);
        if (head_tracker.config().enabled && head_tracker.is_connected(sim_clock_sec) &&
            prev_in_flight && !native_padlock_active && !rmb_freelook &&
            renderer.mode() != falcon::RenderMode::ClassicVga &&
            renderer.vr_config().virtual_cockpit_freelook) {
            renderer.vr_config().head_yaw_deg   = head_tracker.yaw_deg();
            renderer.vr_config().head_pitch_deg = head_tracker.pitch_deg();
        }

        // Render Dear ImGui overlay
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ui.draw(tel, sim_clock_sec);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    // Persist host, renderer, audio, OpenTrack & Flight Model settings cleanly on exit
    save_settings_now();
    head_tracker.shutdown();

    glDeleteTextures(1, &vga_tex);
    glDeleteTextures(1, &vga_cockpit_tex);
    glDeleteTextures(1, &vga_hud_tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
