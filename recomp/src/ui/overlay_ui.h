#pragma once

#include "audio/audio_engine.h"
#include "dos/dos_machine.h"
#include "input/hotas_mapper.h"
#include "net/net_udp.h"
#include "render/modern_renderer.h"
#include "sim/f16_flight_model.h"

#include <string>

namespace falcon {

class OverlayUI {
public:
    OverlayUI(DosMachine& machine, AudioEngine& audio, HotasMapper& hotas,
              NetUdp& net, ModernRenderer& renderer, F16FlightModel& flight_model);

    void draw(const FlightTelemetry& tel);

    bool visible() const { return visible_; }
    void toggle() { visible_ = !visible_; }
    void set_visible(bool v) { visible_ = v; }

private:
    DosMachine& machine_;
    AudioEngine& audio_;
    HotasMapper& hotas_;
    NetUdp& net_;
    ModernRenderer& renderer_;
    F16FlightModel& flight_model_;

    bool visible_ = true;
    bool show_hotas_ = false;
    bool show_audio_ = false;
    bool show_render_ = true;
    bool show_flight_model_ = false;
    bool show_multiplayer_ = false;
    bool show_debugger_ = false;

    char rom_dir_buf_[512]{};
    char join_ip_buf_[128] = "127.0.0.1";
    int net_port_ = 19920;
    std::string profile_status_;

    void draw_hotas_window();
    void draw_audio_window();
    void draw_render_vr_window(const FlightTelemetry& tel);
    void draw_flight_model_window(const FlightTelemetry& tel);
    void draw_multiplayer_window();
    void draw_debugger_window();
};

} // namespace falcon
