#pragma once

#include <cstdint>

namespace falcon {

struct HeadTrackerConfig {
    bool enabled = true;        // Listen passively on UDP 4242 for OpenTrack / TrackIR (zero overhead when idle)
    int udp_port = 4242;        // Standard OpenTrack UDP-over-network port
    float yaw_scale = 1.35f;    // Multiplier for cockpit look-around comfort (allows 180 deg check-six)
    float pitch_scale = 1.15f;
    bool invert_yaw = false;
    bool invert_pitch = false;
    float deadzone_deg = 0.25f;
};

class HeadTracker {
public:
    HeadTracker();
    ~HeadTracker();

    bool init(int port = 4242);
    void shutdown();

    // Poll non-blocking UDP socket, draining to the latest OpenTrack 48-byte double[6] packet
    void poll(double now_sec);

    void recenter();

    bool is_listening() const { return sock_valid_; }
    bool is_connected(double now_sec) const { return has_packet_ && (now_sec - last_packet_sec_) < 1.0; }

    float yaw_deg() const { return out_yaw_; }
    float pitch_deg() const { return out_pitch_; }
    float roll_deg() const { return out_roll_; }

    float raw_yaw_deg() const { return raw_yaw_; }
    float raw_pitch_deg() const { return raw_pitch_; }
    float raw_roll_deg() const { return raw_roll_; }

    HeadTrackerConfig& config() { return cfg_; }
    const HeadTrackerConfig& config() const { return cfg_; }

private:
    HeadTrackerConfig cfg_{};
    uintptr_t sock_ = static_cast<uintptr_t>(~0ULL);
    bool sock_valid_ = false;
    int bound_port_ = 0;

    bool has_packet_ = false;
    double last_packet_sec_ = -10.0;

    float raw_yaw_ = 0.0f;
    float raw_pitch_ = 0.0f;
    float raw_roll_ = 0.0f;

    float center_yaw_ = 0.0f;
    float center_pitch_ = 0.0f;
    float center_roll_ = 0.0f;

    float out_yaw_ = 0.0f;
    float out_pitch_ = 0.0f;
    float out_roll_ = 0.0f;
};

} // namespace falcon
