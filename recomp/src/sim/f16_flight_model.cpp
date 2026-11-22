#include "sim/f16_flight_model.h"

#include <algorithm>
#include <cmath>

namespace falcon {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kRad2Deg = 180.0 / kPi;
constexpr double kGravityFps2 = 32.174;
constexpr double kFpsPerKnot = 1.68781;
constexpr double kWingAreaSqFt = 300.0;  // F-16C wing area = 300 sq ft

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0; // North, East, Up
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3& a) {
    double l = length(a);
    return (l > 1e-12) ? (a * (1.0 / l)) : Vec3{1.0, 0.0, 0.0};
}

// Rodrigues rotation of vector v around unit axis k by angle rad
inline Vec3 rotate_axis(const Vec3& v, const Vec3& k, double rad) {
    double c = std::cos(rad);
    double s = std::sin(rad);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}

// ISA 1976 Standard Atmosphere (altitude in feet -> air density slug/ft^3, density ratio sigma, speed of sound ft/s)
void isa_atmosphere(double alt_ft, double& rho, double& sigma, double& a_fps) {
    double h = std::clamp(alt_ft, 0.0, 65000.0);
    constexpr double kRho0 = 0.0023769; // slug/ft^3 at sea level
    constexpr double kT0 = 518.67;      // deg Rankine
    if (h <= 36089.0) {
        double T = kT0 - 0.00356616 * h;
        double theta = T / kT0;
        sigma = std::pow(theta, 4.25588);
        a_fps = 1116.45 * std::sqrt(theta);
    } else {
        double theta_trop = 389.97 / kT0;
        double sigma_trop = std::pow(theta_trop, 4.25588);
        sigma = sigma_trop * std::exp(-(h - 36089.0) / 20806.0);
        a_fps = 968.08;
    }
    rho = kRho0 * sigma;
}

// Build body basis (F=forward, R=right, U=up) in NEU from Euler (yaw, pitch, roll) in degrees
void basis_from_euler(double yaw_deg, double pitch_deg, double roll_deg, Vec3& F, Vec3& R, Vec3& U) {
    double y = yaw_deg * kDeg2Rad;
    double p = pitch_deg * kDeg2Rad;
    double r = roll_deg * kDeg2Rad;
    double cy = std::cos(y), sy = std::sin(y);
    double cp = std::cos(p), sp = std::sin(p);
    double cr = std::cos(r), sr = std::sin(r);

    F = {cp * cy, cp * sy, sp};
    Vec3 R0 = {-sy, cy, 0.0};
    Vec3 U0 = {-sp * cy, -sp * sy, cp};
    R = R0 * cr - U0 * sr;
    U = R0 * sr + U0 * cr;
}

// Extract Euler (yaw 0..360, pitch -89.5..+89.5, roll -180..+180) from orthonormal NEU basis
void euler_from_basis(const Vec3& F, const Vec3& R, const Vec3& U,
                      double& yaw_deg, double& pitch_deg, double& roll_deg) {
    double pitch_rad = std::asin(std::clamp(F.z, -0.9999, 0.9999));
    pitch_deg = pitch_rad * kRad2Deg;

    double horiz = std::sqrt(F.x * F.x + F.y * F.y);
    if (horiz > 1e-4) {
        double yaw_rad = std::atan2(F.y, F.x);
        yaw_deg = yaw_rad * kRad2Deg;
        while (yaw_deg < 0.0) yaw_deg += 360.0;
        while (yaw_deg >= 360.0) yaw_deg -= 360.0;

        Vec3 R0 = {-std::sin(yaw_rad), std::cos(yaw_rad), 0.0};
        Vec3 U0 = {-std::sin(pitch_rad) * std::cos(yaw_rad),
                   -std::sin(pitch_rad) * std::sin(yaw_rad),
                    std::cos(pitch_rad)};
        // R = cr*R0 - sr*U0, U = sr*R0 + cr*U0
        double cr = dot(R, R0);
        double sr = -dot(R, U0);
        roll_deg = std::atan2(sr, cr) * kRad2Deg;
    }
}

// NASA TP-1538 inspired F-16C lift coefficient C_L(alpha_deg, mach) with leading-edge flaps & strake vortex lift
double f16_cl(double alpha_deg, double mach) {
    double a = std::clamp(alpha_deg, -20.0, 45.0);
    // Subsonic/transonic lift slope (per deg): ~0.072/deg (4.13/rad), enhanced slightly transonically, tapers supersonically
    double mach_scale = 1.0;
    if (mach > 0.7 && mach <= 1.05) {
        mach_scale = 1.0 + 0.14 * ((mach - 0.7) / 0.35);
    } else if (mach > 1.05) {
        mach_scale = std::max(0.58, 1.14 - 0.42 * (mach - 1.05));
    }
    double cl_lin = (0.045 + 0.072 * a) * mach_scale;
    // Smooth strake-vortex peak around 28-32 deg AOA (C_L_max ~1.68), gentle stall roll-off beyond 32 deg
    if (a > 24.0) {
        double excess = a - 24.0;
        double peak = (0.045 + 0.072 * 24.0) * mach_scale;
        if (a <= 33.0) {
            return peak + 0.022 * excess - 0.0014 * excess * excess;
        }
        double at33 = peak + 0.022 * 9.0 - 0.0014 * 81.0;
        return std::max(0.55, at33 - 0.030 * (a - 33.0));
    }
    if (a < -15.0) {
        return std::max(-1.05, cl_lin);
    }
    return cl_lin;
}

// NASA TP-1538 inspired F-16C drag coefficient C_D(alpha_deg, cl, mach, speedbrake)
double f16_cd(double alpha_deg, double cl, double mach, bool speedbrake) {
    // Clean zero-lift drag C_D0
    double cd0 = 0.0175;
    // Transonic wave drag rise (peaks around M 1.05-1.15)
    if (mach > 0.82) {
        double dm = mach - 0.82;
        if (mach <= 1.15) {
            double t = dm / 0.33;
            cd0 += 0.024 * (t * t * (3.0 - 2.0 * t));
        } else {
            double decay = std::exp(-0.65 * (mach - 1.15));
            cd0 += 0.013 + 0.011 * decay;
        }
    }
    // Induced drag K * C_L^2 (with LEF optimization subsonically, rises at high AOA & supersonic)
    double k = 0.145;
    if (mach > 1.0) k += 0.08 * std::min(1.0, mach - 1.0);
    double abs_a = std::abs(alpha_deg);
    if (abs_a > 12.0) {
        double da = (abs_a - 12.0) / 15.0;
        k += 0.095 * da * da;
    }
    double cd = cd0 + k * cl * cl;
    if (speedbrake) cd += 0.055;
    return cd;
}

// Real F110-GE-100 / F100-PW-220 thrust curve (lbf) vs throttle (0..1), altitude (ft), Mach
double f16_thrust_lbf(double throttle_01, double alt_ft, double mach, bool f110_ge100) {
    double rho = 0.0, sigma = 1.0, a_fps = 1116.0;
    isa_atmosphere(alt_ft, rho, sigma, a_fps);

    double sl_mil = f110_ge100 ? 17155.0 : 14590.0;
    double sl_ab  = f110_ge100 ? 28600.0 : 23770.0;
    double idle   = 750.0;

    double thr = std::clamp(throttle_01, 0.0, 1.0);
    double base_sl = idle;
    if (thr <= 0.80) {
        double t = thr / 0.80;
        base_sl = idle + (sl_mil - idle) * std::pow(t, 1.35);
    } else {
        double t = (thr - 0.80) / 0.20;
        base_sl = sl_mil + (sl_ab - sl_mil) * t;
    }

    // Altitude density lapse & ram-air Mach recovery (F-16 reaches ~M 1.2 on the deck, ~M 2.0 at 40k ft)
    double alt_lapse = std::pow(std::max(0.05, sigma), (thr > 0.80) ? 0.76 : 0.82);
    double ram = 1.0 + ((thr > 0.80) ? 0.34 : 0.16) * std::min(mach, 1.85) * std::min(mach, 1.85);
    if (mach > 1.85) {
        ram *= std::max(0.45, 1.0 - 0.75 * (mach - 1.85));
    }
    return base_sl * alt_lapse * ram;
}
} // namespace

void F16FlightModel::sync_from_falcon(double wx, double wy, float alt_ft, float agl_ft,
                                      float yaw_deg, float pitch_deg, float roll_deg,
                                      float kias, float weight_lbs, float throttle_01) {
    state_.world_x = wx;
    state_.world_y = wy;
    state_.alt_ft = std::max(10.0, static_cast<double>(alt_ft));
    state_.ground_alt_ft = std::max(0.0, state_.alt_ft - static_cast<double>(agl_ft));
    state_.yaw_deg = yaw_deg;
    state_.pitch_deg = pitch_deg;
    state_.roll_deg = roll_deg;
    state_.weight_lbs = std::clamp(static_cast<double>(weight_lbs), 16500.0, 37500.0);
    state_.throttle_cmd = std::clamp(static_cast<double>(throttle_01), 0.0, 1.0);
    state_.rpm_pct = 62.0 + state_.throttle_cmd * 48.0;

    double rho = 0.0, sigma = 1.0, a_fps = 1116.0;
    isa_atmosphere(state_.alt_ft, rho, sigma, a_fps);
    double tas = std::max(180.0, (static_cast<double>(kias) * kFpsPerKnot) / std::sqrt(std::max(0.08, sigma)));
    state_.tas_fps = tas;
    state_.kias = kias;
    state_.mach = tas / a_fps;

    Vec3 F, R, U;
    basis_from_euler(state_.yaw_deg, state_.pitch_deg, state_.roll_deg, F, R, U);
    state_.fx = F.x; state_.fy = F.y; state_.fz = F.z;
    state_.rx = R.x; state_.ry = R.y; state_.rz = R.z;
    state_.ux = U.x; state_.uy = U.y; state_.uz = U.z;

    // Initialize velocity with slight trim AOA below nose F so 1G starts dead steady
    double cl_trim = state_.weight_lbs / std::max(1.0, 0.5 * rho * tas * tas * kWingAreaSqFt);
    double a_trim = std::clamp((cl_trim - 0.045) / 0.072, 0.0, 12.0) * kDeg2Rad;
    Vec3 Vdir = normalize(F * std::cos(a_trim) - U * std::sin(a_trim));
    state_.vn_fps = Vdir.x * tas;
    state_.ve_fps = Vdir.y * tas;
    state_.vu_fps = Vdir.z * tas;
    state_.aoa_deg = a_trim * kRad2Deg;
    state_.beta_deg = 0.0;
    state_.g_load = 1.0;
    state_.max_g = 1.0;
    state_.roll_rate_dps = 0.0;
    state_.pitch_rate_dps = 0.0;
    state_.turn_rate_dps = 0.0;
    state_.fp_yaw_deg = state_.yaw_deg;
    state_.fp_pitch_deg = state_.pitch_deg;
    state_.initialized = true;
}

void F16FlightModel::step(double dt_sec, const F16ControlInput& in) {
    if (!state_.initialized) return;
    double dt_total = std::clamp(dt_sec, 0.0005, 0.05);
    constexpr double kSubStep = 1.0 / 240.0;
    int steps = std::max(1, static_cast<int>(std::ceil(dt_total / kSubStep)));
    double dt = dt_total / static_cast<double>(steps);
    for (int i = 0; i < steps; ++i) {
        substep(dt, in);
    }
}

void F16FlightModel::substep(double dt, const F16ControlInput& in) {
    double rho = 0.0, sigma = 1.0, a_fps = 1116.0;
    isa_atmosphere(state_.alt_ft, rho, sigma, a_fps);

    // Use persisted gimbal-lock-free body basis (F=forward, R=right, U=up in NEU)
    Vec3 F{state_.fx, state_.fy, state_.fz};
    Vec3 R{state_.rx, state_.ry, state_.rz};
    Vec3 U{state_.ux, state_.uy, state_.uz};

    Vec3 V{state_.vn_fps, state_.ve_fps, state_.vu_fps};
    double tas = std::max(60.0, length(V));
    Vec3 V_hat = V * (1.0 / tas);
    double mach = tas / a_fps;
    double qbar = 0.5 * rho * tas * tas; // dynamic pressure (lbf/ft^2)
    double mass = state_.weight_lbs / kGravityFps2; // slugs

    // Current aerodynamic angles
    double v_fwd = dot(V, F);
    double v_right = dot(V, R);
    double v_up = dot(V, U);
    double alpha_deg = std::atan2(-v_up, std::max(10.0, v_fwd)) * kRad2Deg;
    double beta_deg = std::asin(std::clamp(v_right / tas, -0.95, 0.95)) * kRad2Deg;

    // 1. Engine spool dynamics (authentic F110/F100 lag)
    double target_thr = in.has_analog_throttle ? static_cast<double>(in.throttle) : state_.throttle_cmd;
    target_thr = std::clamp(target_thr, 0.0, 1.0);
    double tau_eng = (target_thr > 0.80) ? 0.85 : 1.35;
    state_.throttle_cmd += (target_thr - state_.throttle_cmd) * std::min(1.0, dt / tau_eng);
    state_.rpm_pct = 62.0 + state_.throttle_cmd * 48.0;
    double thrust = f16_thrust_lbf(state_.throttle_cmd, state_.alt_ft, mach, cfg_.engine_f110_ge100);

    // 2. Aerodynamic forces (NASA TP-1538 Lift, Drag, Thrust, Sideforce, Gravity)
    double cl = f16_cl(alpha_deg, mach);
    double cd = f16_cd(alpha_deg, cl, mach, in.speedbrake);
    double lift_lbf = qbar * kWingAreaSqFt * cl;
    double drag_lbf = qbar * kWingAreaSqFt * cd;
    double side_lbf = -qbar * kWingAreaSqFt * (0.024 * beta_deg);

    // Lift direction = body Up orthogonalized to V_hat
    Vec3 L_dir = U - V_hat * dot(U, V_hat);
    double l_len = length(L_dir);
    if (l_len > 1e-6) L_dir = L_dir * (1.0 / l_len);
    else L_dir = U;

    Vec3 force = F * thrust + L_dir * lift_lbf - V_hat * drag_lbf + R * side_lbf
                 + Vec3{0.0, 0.0, -state_.weight_lbs};
    Vec3 accel = force * (1.0 / mass);

    // Compute true normal G-load (felt by pilot along body Up axis)
    double aero_up = dot(F * thrust + L_dir * lift_lbf - V_hat * drag_lbf, U);
    state_.g_load = aero_up / state_.weight_lbs;
    if (state_.g_load > state_.max_g) state_.max_g = state_.g_load;

    // 3. Digital FLCS Control Laws (G-command pitch + 25.5 deg AOA limiter + AOA-scheduled roll rate!)
    double g_max = cfg_.cat3_limiter ? 5.5 : 9.0;
    double g_min = cfg_.cat3_limiter ? -2.0 : -3.0;
    double aoa_limit = cfg_.aoa_limiter ? (cfg_.cat3_limiter ? 18.0 : 25.5) : 42.0;

    double sp = std::clamp(static_cast<double>(in.stick_pitch) * cfg_.stick_pitch_sens, -1.0, 1.0);
    double sr = std::clamp(static_cast<double>(in.stick_roll)  * cfg_.stick_roll_sens,  -1.0, 1.0);
    double rud = std::clamp(static_cast<double>(in.rudder), -1.0, 1.0);

    // Hands-off FLCS auto-trim = 1.0G along current flight path (classic real F-16 hands-off feel!)
    double nz_cmd = 1.0;
    if (sp >= 0.0) {
        nz_cmd = 1.0 + (g_max - 1.0) * sp;
    } else {
        nz_cmd = 1.0 + (1.0 - g_min) * sp;
    }

    // Solve target AOA for commanded G-load, smoothly clamped by FLCS AOA limiter!
    double mach_scale = 1.0;
    if (mach > 0.7 && mach <= 1.05) {
        mach_scale = 1.0 + 0.14 * ((mach - 0.7) / 0.35);
    } else if (mach > 1.05) {
        mach_scale = std::max(0.58, 1.14 - 0.42 * (mach - 1.05));
    }
    double cl_req = (nz_cmd * state_.weight_lbs) / std::max(1.0, qbar * kWingAreaSqFt);
    double aoa_target = (cl_req / mach_scale - 0.045) / 0.072;
    aoa_target = std::clamp(aoa_target, -10.0, aoa_limit);

    // Exact flight-path curvature feed-forward (zero steady-state G droop in hard turns!)
    double q_ff_dps = (dot(accel, U) / tas) * kRad2Deg;
    double r_ff_dps = (dot(accel, R) / tas) * kRad2Deg;

    double aoa_err = aoa_target - alpha_deg;
    double max_q_dps = (tas > 220.0) ? 55.0 : 24.0;
    double q_cmd_dps = std::clamp(q_ff_dps + aoa_err * 7.5, -max_q_dps, max_q_dps);
    state_.pitch_rate_dps += (q_cmd_dps - state_.pitch_rate_dps) * std::min(1.0, dt * 16.0);

    // FLCS Roll-rate command (240 deg/s unloaded, smoothly scheduled down at high AOA for departure resistance)
    double aoa_roll_sched = std::clamp(1.0 - std::max(0.0, alpha_deg - 10.0) / 26.0, 0.32, 1.0);
    double q_roll_auth = std::clamp(qbar / 80.0, 0.25, 1.0);
    double p_cmd_dps = sr * static_cast<double>(cfg_.max_roll_rate_dps) * aoa_roll_sched * q_roll_auth;
    state_.roll_rate_dps += (p_cmd_dps - state_.roll_rate_dps) * std::min(1.0, dt * 18.0);

    // Yaw rate: flight-path coordination + rudder + POSITIVE weathercock damping (+beta turns nose F toward +R into V!)
    double r_cmd_dps = std::clamp(r_ff_dps + rud * 28.0 * q_roll_auth + beta_deg * 5.0, -45.0, 45.0);

    // Track horizontal turn rate (deg/s) for dogfight telemetry
    double prev_fp_yaw = std::atan2(V.y, V.x);

    // Integrate velocity & position (semi-implicit Euler at 240 Hz!)
    V = V + accel * dt;
    double new_tas = std::clamp(length(V), 90.0, 2250.0);
    if (length(V) > 1e-6) V = normalize(V) * new_tas;

    double new_fp_yaw = std::atan2(V.y, V.x);
    double dyaw_fp = (new_fp_yaw - prev_fp_yaw) * kRad2Deg;
    while (dyaw_fp > 180.0) dyaw_fp -= 360.0;
    while (dyaw_fp < -180.0) dyaw_fp += 360.0;
    state_.turn_rate_dps = std::abs(dyaw_fp) / dt;

    // 4. Rotate aircraft body basis (F, R, U):
    //   - Roll around stability/velocity axis at high AOA (real F-16 FLCS!) blended with nose F
    Vec3 roll_axis = normalize(F * 0.4 + V_hat * 0.6);
    double d_roll = state_.roll_rate_dps * kDeg2Rad * dt;
    double d_pitch = state_.pitch_rate_dps * kDeg2Rad * dt;
    double d_yaw = r_cmd_dps * kDeg2Rad * dt;

    // Pitch rotates around body Right (R): positive pitch_rate pitches nose UP (+F toward +U -> right-hand around -R)
    Vec3 neg_R = R * -1.0;
    F = rotate_axis(F, neg_R, d_pitch);
    U = rotate_axis(U, neg_R, d_pitch);

    // Roll rotates around -roll_axis: positive d_roll dips right wing R toward -U (banking right!)
    Vec3 neg_roll_axis = roll_axis * -1.0;
    F = rotate_axis(F, neg_roll_axis, d_roll);
    R = rotate_axis(R, neg_roll_axis, d_roll);
    U = rotate_axis(U, neg_roll_axis, d_roll);

    // Yaw right rotates around +U: cross(U, F) = +R, so positive d_yaw moves F toward +R
    F = rotate_axis(F, U, d_yaw);
    R = rotate_axis(R, U, d_yaw);

    // Re-orthonormalize right-handed (F, R, U) cleanly (F x R = +U, U x F = +R)
    F = normalize(F);
    R = normalize(cross(U, F));
    U = normalize(cross(F, R));

    // Integrate world position (Falcon 3.0 stores world_x=North, world_y=East in 0.5-ft units -> * 2.0)
    state_.world_x += V.x * dt * 2.0;
    state_.world_y += V.y * dt * 2.0;
    state_.alt_ft  += V.z * dt;

    // Ground safety floor (prevent tunneling below desert/runway)
    double min_alt = state_.ground_alt_ft + 8.0;
    if (state_.alt_ft < min_alt) {
        state_.alt_ft = min_alt;
        if (V.z < 0.0) V.z = 0.0;
        if (F.z < 0.0) {
            F.z = 0.02;
            F = normalize(F);
            R = normalize(cross(Vec3{0.0, 0.0, 1.0}, F));
            U = normalize(cross(F, R));
        }
    }

    state_.vn_fps = V.x;
    state_.ve_fps = V.y;
    state_.vu_fps = V.z;
    state_.fx = F.x; state_.fy = F.y; state_.fz = F.z;
    state_.rx = R.x; state_.ry = R.y; state_.rz = R.z;
    state_.ux = U.x; state_.uy = U.y; state_.uz = U.z;
    euler_from_basis(F, R, U, state_.yaw_deg, state_.pitch_deg, state_.roll_deg);

    // Update live aero telemetry
    state_.tas_fps = new_tas;
    state_.mach = new_tas / a_fps;
    state_.kias = (new_tas * std::sqrt(std::max(0.05, sigma))) / kFpsPerKnot;
    state_.aoa_deg = alpha_deg;
    state_.beta_deg = beta_deg;
    state_.thrust_lbf = thrust;
    state_.drag_lbf = drag_lbf;
    state_.ps_fps = ((thrust - drag_lbf) * new_tas) / state_.weight_lbs;

    double horiz_v = std::sqrt(V.x * V.x + V.y * V.y);
    state_.fp_pitch_deg = std::atan2(V.z, std::max(1.0, horiz_v)) * kRad2Deg;
    double fyaw = std::atan2(V.y, V.x) * kRad2Deg;
    while (fyaw < 0.0) fyaw += 360.0;
    while (fyaw >= 360.0) fyaw -= 360.0;
    state_.fp_yaw_deg = fyaw;
}

} // namespace falcon
