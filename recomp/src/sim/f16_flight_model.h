#pragma once

#include <cstdint>

namespace falcon {

struct F16ControlInput {
    float stick_roll = 0.0f;    // -1.0 (left) .. +1.0 (right)
    float stick_pitch = 0.0f;   // -1.0 (nose down) .. +1.0 (nose up / pull back)
    float rudder = 0.0f;        // -1.0 (left) .. +1.0 (right)
    float throttle = 0.75f;     // 0.0 (idle) .. 0.80 (MIL) .. 1.0 (MAX AB)
    bool has_analog_throttle = false;
    bool speedbrake = false;
};

struct F16FlightModelConfig {
    bool enabled = false;           // Toggleable live in F12 UI (A/B compare vs original Falcon 3.0!)
    bool cat3_limiter = false;      // false = CAT I (9.0G / 25.5 deg AOA), true = CAT III (5.5G / 18 deg AOA)
    bool aoa_limiter = true;        // Authentic F-16 FLCS 25.5 deg AOA limiter (disable for MPO deep-stall test)
    bool engine_f110_ge100 = true;  // true = Block 30/40 F110-GE-100 (28,600 lbf AB), false = F100-PW-220 (23,770 lbf)
    float max_roll_rate_dps = 240.0f;
    float stick_pitch_sens = 1.0f;
    float stick_roll_sens = 1.0f;
};

struct F16SimState {
    bool initialized = false;
    // World position in Falcon 3.0 native 0.5-ft units (North, East, Up)
    double world_x = 0.0;
    double world_y = 0.0;
    double alt_ft = 7800.0;
    double ground_alt_ft = 0.0;

    // Velocity in North-East-Up ft/s
    double vn_fps = 0.0;
    double ve_fps = -823.0;
    double vu_fps = 0.0;

    // Euler attitude (degrees): yaw 0..360 (0=N, 90=E, 270=W), pitch -89.5..+89.5, roll -180..+180
    double yaw_deg = 270.0;
    double pitch_deg = 0.0;
    double roll_deg = 0.0;

    // Persisted orthonormal body basis in NEU (gimbal-lock-free through 90 deg vertical loops!)
    double fx = 0.0, fy = -1.0, fz = 0.0;
    double rx = 1.0, ry = 0.0,  rz = 0.0;
    double ux = 0.0, uy = 0.0,  uz = 1.0;

    // Live aerodynamic outputs (for HUD/UI telemetry & RAM sync)
    double tas_fps = 823.0;
    double kias = 488.0;
    double mach = 0.74;
    double aoa_deg = 2.0;
    double beta_deg = 0.0;
    double g_load = 1.0;
    double max_g = 1.0;
    double roll_rate_dps = 0.0;
    double pitch_rate_dps = 0.0;
    double turn_rate_dps = 0.0;
    double ps_fps = 0.0;            // Specific excess power (ft/s)
    double thrust_lbf = 8000.0;
    double drag_lbf = 8000.0;
    double throttle_cmd = 0.75;     // 0..1
    double rpm_pct = 85.0;          // 60..100+ (AB > 100)
    double weight_lbs = 21500.0;
    double fp_yaw_deg = 270.0;      // Flight path heading (velocity vector)
    double fp_pitch_deg = 0.0;      // Flight path climb angle (gamma)
};

class F16FlightModel {
public:
    F16FlightModel() = default;

    // Sync initial state seamlessly from Falcon 3.0's current RAM state
    void sync_from_falcon(double wx, double wy, float alt_ft, float agl_ft,
                          float yaw_deg, float pitch_deg, float roll_deg,
                          float kias, float weight_lbs, float throttle_01);
    void reset() { state_.initialized = false; }

    // Step true-to-life 6-DOF F-16C Block 30/40 + FLCS physics by dt_sec (sub-stepped at 240 Hz)
    void step(double dt_sec, const F16ControlInput& in);

    F16FlightModelConfig& config() { return cfg_; }
    const F16FlightModelConfig& config() const { return cfg_; }
    const F16SimState& state() const { return state_; }
    bool is_initialized() const { return state_.initialized; }

private:
    F16FlightModelConfig cfg_;
    F16SimState state_;

    void substep(double dt, const F16ControlInput& in);
};

} // namespace falcon
