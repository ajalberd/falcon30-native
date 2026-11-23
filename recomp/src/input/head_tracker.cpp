#include "input/head_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace falcon {

namespace {
float apply_deadzone(float v, float dz) {
    if (std::abs(v) <= dz) return 0.0f;
    return (v > 0.0f) ? (v - dz) : (v + dz);
}
} // namespace

HeadTracker::HeadTracker() = default;

HeadTracker::~HeadTracker() {
    shutdown();
}

bool HeadTracker::init(int port) {
    shutdown();
    cfg_.udp_port = std::clamp(port, 1024, 65535);

#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.udp_port));
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return false;
    }
    sock_ = static_cast<uintptr_t>(s);
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    fcntl(s, F_SETFL, O_NONBLOCK);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.udp_port));
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(s);
        return false;
    }
    sock_ = static_cast<uintptr_t>(s);
#endif
    sock_valid_ = true;
    bound_port_ = cfg_.udp_port;
    return true;
}

void HeadTracker::shutdown() {
    if (sock_valid_) {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(sock_));
#else
        close(static_cast<int>(sock_));
#endif
        sock_valid_ = false;
        sock_ = static_cast<uintptr_t>(~0ULL);
        bound_port_ = 0;
    }
}

void HeadTracker::recenter() {
    center_yaw_ = raw_yaw_;
    center_pitch_ = raw_pitch_;
    center_roll_ = raw_roll_;
    out_yaw_ = 0.0f;
    out_pitch_ = 0.0f;
    out_roll_ = 0.0f;
}

void HeadTracker::poll(double now_sec) {
    if (!cfg_.enabled) {
        out_yaw_ = 0.0f;
        out_pitch_ = 0.0f;
        out_roll_ = 0.0f;
        return;
    }
    if (!sock_valid_ || bound_port_ != cfg_.udp_port) {
        if (!init(cfg_.udp_port)) return;
    }

    // OpenTrack standard UDP-over-network packet: 6 IEEE-754 little-endian doubles (48 bytes):
    //   [0]=X (cm), [1]=Y (cm), [2]=Z (cm), [3]=Yaw (deg), [4]=Pitch (deg), [5]=Roll (deg)
    double latest[6]{};
    bool got_any = false;
    for (int iter = 0; iter < 32; ++iter) {
        uint8_t buf[64]{};
#ifdef _WIN32
        int n = recvfrom(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
#else
        ssize_t n = recvfrom(static_cast<int>(sock_), buf, sizeof(buf), 0, nullptr, nullptr);
#endif
        if (n < 48) break;
        std::memcpy(latest, buf, 48);
        got_any = true;
    }

    if (got_any) {
        if (std::isfinite(latest[3]) && std::isfinite(latest[4]) && std::isfinite(latest[5])) {
            raw_yaw_   = static_cast<float>(latest[3]);
            raw_pitch_ = static_cast<float>(latest[4]);
            raw_roll_  = static_cast<float>(latest[5]);
            has_packet_ = true;
            last_packet_sec_ = now_sec;

            float dy = apply_deadzone(raw_yaw_ - center_yaw_, cfg_.deadzone_deg) * cfg_.yaw_scale;
            float dp = apply_deadzone(raw_pitch_ - center_pitch_, cfg_.deadzone_deg) * cfg_.pitch_scale;
            if (cfg_.invert_yaw) dy = -dy;
            if (cfg_.invert_pitch) dp = -dp;

            out_yaw_   = std::clamp(dy, -175.0f, 175.0f);
            out_pitch_ = std::clamp(dp, -68.0f, 82.0f);
            out_roll_  = std::clamp(raw_roll_ - center_roll_, -45.0f, 45.0f);
        }
    }
}

} // namespace falcon
