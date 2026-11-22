#include "render/modern_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace falcon {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr double kHalfFtToKm = 0.0001524;          // 0.5 ft = 0.1524 m = 0.0001524 km
constexpr double kTheaterHalfSpan = 3355648.0;     // 1024 * 3277 (verified 0.0-ft error vs ILS.* across all 8 theaters!)
constexpr double kAltTileUnits = 209728.0;         // 32 tiles across 6,711,296 units (64 * 3277 per tile)
constexpr double kIslVertScale = 209728.0 / 5502.0;// Verified ISLDAT.* vertex -> 0.5 ft units
constexpr float kObjUnitToKm = 0.0000412f;         // Verified F16 365 units = 49.4 ft = 0.01505 km
constexpr float kFalconFyVga = 285.75f;            // Verified from HUD: 5 deg = 25.0 VGA px

inline uint16_t rd16(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<uint16_t>(d[off] | (static_cast<uint16_t>(d[off + 1]) << 8));
}
inline int16_t rds16(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<int16_t>(rd16(d, off));
}
inline uint32_t rd32(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<uint32_t>(d[off]) |
           (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) |
           (static_cast<uint32_t>(d[off + 3]) << 24);
}
inline int32_t rds32(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<int32_t>(rd32(d, off));
}

// Fast deterministic integer hash -> [0, 1) for world-locked retro desert facets & sand dots
inline float hash2f(int64_t x, int64_t z, uint32_t seed = 0) {
    uint64_t h = static_cast<uint64_t>(x) * 374761393ULL +
                 static_cast<uint64_t>(z) * 668265263ULL +
                 static_cast<uint64_t>(seed) * 1442695041ULL;
    h = (h ^ (h >> 13)) * 1274126177ULL;
    h ^= (h >> 16);
    return static_cast<float>(h & 0xFFFFu) / 65536.0f;
}

void set_perspective(float fov_y_deg, float aspect, float near_z, float far_z) {
    float f = 1.0f / std::tan(fov_y_deg * 0.5f * kDeg2Rad);
    float m[16]{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far_z + near_z) / (near_z - far_z);
    m[11] = -1.0f;
    m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    glLoadMatrixf(m);
}

void set_falcon_vga_frustum(float cy_vga, float near_z, float far_z, float aspect = 4.0f / 3.0f) {
    // Exact off-center frustum matching Falcon 3.0's 320x200 projection for ANY view:
    // Forward Cockpit cy=53, Chase External cy=85, Padlock cy=117, Full-Screen cy=100.
    // Expanding aspect > 4/3 fills widescreen left/right margins with raw 3D world while keeping
    // the center 4:3 region 100% 1:1 pixel-locked to Falcon 3.0's 2D cockpit & HUD!
    float cy = std::clamp(cy_vga, 20.0f, 180.0f);
    float top = near_z * (cy / kFalconFyVga);
    float bottom = -near_z * ((200.0f - cy) / kFalconFyVga);
    float half_w = near_z * (100.0f / kFalconFyVga) * std::max(4.0f / 3.0f, aspect);
    glLoadIdentity();
    glFrustum(-half_w, half_w, bottom, top, near_z, far_z);
}

void set_widescreen_cockpit_frustum(float aspect, float vfov_deg, float cy_frac, float near_z, float far_z) {
    // Pulled-back 16:9 widescreen off-center frustum keeping Falcon 3.0's live 2D HUD 1:1 optically collimated!
    float vfov = std::clamp(vfov_deg, 36.0f, 68.0f);
    float h_span = 2.0f * std::tan(vfov * 0.5f * kDeg2Rad);
    float top = near_z * (cy_frac * h_span);
    float bottom = -near_z * ((1.0f - cy_frac) * h_span);
    float half_w = near_z * (0.5f * h_span * std::max(1.0f, aspect));
    glLoadIdentity();
    glFrustum(-half_w, half_w, bottom, top, near_z, far_z);
}

// 7-segment style vector strokes for crisp OpenGL fallback HUD digits/letters
void stroke_char(float x, float y, float w, float h, char c) {
    auto seg = [&](float x0, float y0, float x1, float y1) {
        glVertex2f(x + x0 * w, y + y0 * h);
        glVertex2f(x + x1 * w, y + y1 * h);
    };
    switch (c) {
        case '0': seg(0,0,1,0); seg(1,0,1,1); seg(1,1,0,1); seg(0,1,0,0); break;
        case '1': seg(0.6f,0,0.6f,1); break;
        case '2': seg(0,1,1,1); seg(1,1,1,0.5f); seg(1,0.5f,0,0.5f); seg(0,0.5f,0,0); seg(0,0,1,0); break;
        case '3': seg(0,1,1,1); seg(1,1,1,0); seg(0,0.5f,1,0.5f); seg(0,0,1,0); break;
        case '4': seg(0,1,0,0.5f); seg(0,0.5f,1,0.5f); seg(1,1,1,0); break;
        case '5': seg(1,1,0,1); seg(0,1,0,0.5f); seg(0,0.5f,1,0.5f); seg(1,0.5f,1,0); seg(1,0,0,0); break;
        case '6': seg(1,1,0,1); seg(0,1,0,0); seg(0,0,1,0); seg(1,0,1,0.5f); seg(1,0.5f,0,0.5f); break;
        case '7': seg(0,1,1,1); seg(1,1,0.4f,0); break;
        case '8': seg(0,0,1,0); seg(1,0,1,1); seg(1,1,0,1); seg(0,1,0,0); seg(0,0.5f,1,0.5f); break;
        case '9': seg(0,0.5f,1,0.5f); seg(0,0.5f,0,1); seg(0,1,1,1); seg(1,1,1,0); seg(0,0,1,0); break;
        case '-': seg(0.1f,0.5f,0.9f,0.5f); break;
        case '.': seg(0.4f,0.05f,0.6f,0.05f); break;
        case 'K': seg(0,0,0,1); seg(1,1,0,0.5f); seg(0,0.5f,1,0); break;
        case 'T': seg(0,1,1,1); seg(0.5f,1,0.5f,0); break;
        case 'S': seg(1,1,0,1); seg(0,1,0,0.5f); seg(0,0.5f,1,0.5f); seg(1,0.5f,1,0); seg(1,0,0,0); break;
        case 'F': seg(0,0,0,1); seg(0,1,1,1); seg(0,0.5f,0.75f,0.5f); break;
        case 'H': seg(0,0,0,1); seg(1,0,1,1); seg(0,0.5f,1,0.5f); break;
        case 'D': seg(0,0,0,1); seg(0,1,0.75f,1); seg(0.75f,1,1,0.75f); seg(1,0.75f,1,0.25f); seg(1,0.25f,0.75f,0); seg(0.75f,0,0,0); break;
        case 'G': seg(1,1,0,1); seg(0,1,0,0); seg(0,0,1,0); seg(1,0,1,0.5f); seg(0.5f,0.5f,1,0.5f); break;
        case 'M': seg(0,0,0,1); seg(0,1,0.5f,0.4f); seg(0.5f,0.4f,1,1); seg(1,1,1,0); break;
        case 'A': seg(0,0,0.5f,1); seg(0.5f,1,1,0); seg(0.22f,0.45f,0.78f,0.45f); break;
        case 'B': seg(0,0,0,1); seg(0,1,0.8f,1); seg(0.8f,1,1,0.78f); seg(1,0.78f,0.8f,0.55f); seg(0,0.55f,0.85f,0.55f); seg(0.85f,0.55f,1,0.28f); seg(1,0.28f,0.8f,0); seg(0.8f,0,0,0); break;
        case 'C': seg(1,1,0,1); seg(0,1,0,0); seg(0,0,1,0); break;
        case 'E': seg(1,1,0,1); seg(0,1,0,0); seg(0,0.5f,0.8f,0.5f); seg(0,0,1,0); break;
        case 'I': seg(0.5f,0,0.5f,1); seg(0.2f,1,0.8f,1); seg(0.2f,0,0.8f,0); break;
        case 'L': seg(0,1,0,0); seg(0,0,1,0); break;
        case 'N': seg(0,0,0,1); seg(0,1,1,0); seg(1,0,1,1); break;
        case 'O': seg(0,0,1,0); seg(1,0,1,1); seg(1,1,0,1); seg(0,1,0,0); break;
        case 'P': seg(0,0,0,1); seg(0,1,1,1); seg(1,1,1,0.5f); seg(1,0.5f,0,0.5f); break;
        case 'R': seg(0,0,0,1); seg(0,1,1,1); seg(1,1,1,0.5f); seg(1,0.5f,0,0.5f); seg(0.35f,0.5f,1,0); break;
        case 'U': seg(0,1,0,0); seg(0,0,1,0); seg(1,0,1,1); break;
        case 'V': seg(0,1,0.5f,0); seg(0.5f,0,1,1); break;
        case 'W': seg(0,1,0.2f,0); seg(0.2f,0,0.5f,0.5f); seg(0.5f,0.5f,0.8f,0); seg(0.8f,0,1,1); break;
        case 'X': seg(0,1,1,0); seg(0,0,1,1); break;
        case 'Y': seg(0,1,0.5f,0.5f); seg(1,1,0.5f,0.5f); seg(0.5f,0.5f,0.5f,0); break;
        default: break;
    }
}
} // namespace

ModernRenderer::ModernRenderer() {
    for (int i = 0; i < 256; ++i) {
        float g = static_cast<float>(i) / 255.0f;
        vga_rgb_[i * 3 + 0] = g;
        vga_rgb_[i * 3 + 1] = g;
        vga_rgb_[i * 3 + 2] = g;
    }
}

ModernRenderer::~ModernRenderer() {
    if (padlock_strip_tex_ != 0) {
        glDeleteTextures(1, &padlock_strip_tex_);
        padlock_strip_tex_ = 0;
    }
}

void ModernRenderer::set_vga_palette(const uint8_t* dac_768) {
    if (!dac_768) return;
    std::memcpy(vga_dac_raw_.data(), dac_768, 768);
    for (int i = 0; i < 256 * 3; ++i) {
        vga_rgb_[i] = static_cast<float>(dac_768[i]) / 63.0f;
    }
    has_vga_pal_ = true;
    if (cockpit_3d_.has_any_views()) {
        cockpit_3d_.sync_palette(vga_dac_raw_.data());
    }
}

bool ModernRenderer::load_theater_map(const std::string& map_path) {
    std::ifstream f(map_path, std::ios::binary);
    if (!f) return false;
    uint16_t hdr[2]{};
    f.read(reinterpret_cast<char*>(hdr), 4);
    if (!f || hdr[0] == 0 || hdr[1] == 0 || hdr[0] > 1024 || hdr[1] > 1024) return false;
    map_w_ = hdr[0];
    map_h_ = hdr[1];
    map_heights_.resize(static_cast<size_t>(map_w_) * static_cast<size_t>(map_h_));
    f.read(reinterpret_cast<char*>(map_heights_.data()), static_cast<std::streamsize>(map_heights_.size()));
    loaded_theater_ = fs::path(map_path).filename().string();
    return true;
}

bool ModernRenderer::load_isldat(const std::string& isldat_path) {
    std::ifstream f(isldat_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 0x600) return false;

    isldat_tris_.clear();
    isldat_land_tris_.clear();
    isldat_urban_tris_.clear();
    uint16_t off_tbl = rd16(d, 4); // 1280 (0x500)
    size_t scan_start = 9 + static_cast<size_t>(off_tbl);
    size_t scan_end = std::min(d.size() - 2, scan_start + 0x700);

    std::unordered_set<uint16_t> poly_offs;
    for (size_t p = scan_start; p + 2 <= scan_end; p += 2) {
        uint16_t v = rd16(d, p);
        if (v >= off_tbl && 9 + static_cast<size_t>(v) + 48 < d.size()) {
            if (rd16(d, 9 + v + 34) == static_cast<uint16_t>(v + 0x26)) {
                poly_offs.insert(v);
            }
        }
    }

    for (uint16_t po : poly_offs) {
        size_t p = 9 + static_cast<size_t>(po);
        int32_t ix = rds32(d, p + 0);
        int32_t iy = rds32(d, p + 4);
        size_t mp = p + 0x26;
        if (mp + 16 > d.size()) continue;
        uint16_t nverts = rd16(d, mp + 8);
        uint16_t vptr = rd16(d, mp + 10);
        uint16_t fptr = rd16(d, mp + 12);
        if (nverts == 0 || nverts > 512 || 9 + static_cast<size_t>(vptr) + nverts * 6 > d.size()) continue;

        std::vector<IslTriVert> verts(nverts);
        for (uint16_t k = 0; k < nverts; ++k) {
            size_t vp = 9 + static_cast<size_t>(vptr) + static_cast<size_t>(k) * 6;
            int16_t vx = rds16(d, vp + 0);
            int16_t vz = rds16(d, vp + 4);
            verts[k].wx = static_cast<double>(ix) + static_cast<double>(vz) * kIslVertScale;
            verts[k].wy = static_cast<double>(iy) + static_cast<double>(vx) * kIslVertScale;
        }

        size_t fp = 9 + static_cast<size_t>(fptr);
        std::vector<size_t> groups;
        while (fp + 2 <= d.size()) {
            uint16_t w = rd16(d, fp);
            fp += 2;
            if (w == 0xFFFF) break;
            if (9 + static_cast<size_t>(w) < d.size()) {
                groups.push_back(9 + static_cast<size_t>(w));
            } else {
                groups.clear();
                groups.push_back(fp - 2);
                break;
            }
        }
        if (groups.empty() && fp < d.size()) groups.push_back(fp);

        for (size_t gp : groups) {
            if (gp + 4 > d.size()) continue;
            // Face group header: w1 encodes Falcon 3.0's exact ISLDAT surface layer:
            //   w1 == 1: Water (bays, straits, channels, inland lakes)
            //   w1 == 0: Land / Islands drawn ON TOP of water (e.g. Bubiyan & Failaka Islands!)
            //   w1 == 2: Grey urban / city / concrete aprons drawn ON TOP of land (Kuwait City, airbases)
            uint16_t surf_kind = rd16(d, gp + 2);
            gp += 4;
            if (surf_kind > 2) continue;
            auto& dst_tris = (surf_kind == 0) ? isldat_land_tris_
                           : (surf_kind == 2) ? isldat_urban_tris_
                                              : isldat_tris_;
            while (gp + 2 <= d.size()) {
                uint16_t nv = rd16(d, gp);
                gp += 2;
                if (nv == 0xFFFF || nv < 3 || nv > 32) break;
                if (gp + (static_cast<size_t>(nv) + 1) * 2 > d.size()) break;
                std::vector<IslTriVert> poly;
                poly.reserve(nv);
                for (uint16_t k = 0; k < nv; ++k) {
                    uint16_t vi = rd16(d, gp + static_cast<size_t>(k) * 2) / 16;
                    if (vi < verts.size()) poly.push_back(verts[vi]);
                }
                gp += (static_cast<size_t>(nv) + 1) * 2;
                for (size_t k = 1; k + 1 < poly.size(); ++k) {
                    dst_tris.push_back(poly[0]);
                    dst_tris.push_back(poly[k]);
                    dst_tris.push_back(poly[k + 1]);
                }
            }
        }
    }
    return !isldat_tris_.empty() || !isldat_land_tris_.empty() || !isldat_urban_tris_.empty();
}

bool ModernRenderer::load_alt_terrain(const std::string& alt_path) {
    std::ifstream f(alt_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 0x1800) return false;

    cloud_grid_.resize(128 * 128);
    for (int i = 0; i < 4096; ++i) {
        uint8_t b = d[i];
        cloud_grid_[i * 4 + 0] = (b >> 6) & 3;
        cloud_grid_[i * 4 + 1] = (b >> 4) & 3;
        cloud_grid_[i * 4 + 2] = (b >> 2) & 3;
        cloud_grid_[i * 4 + 3] = (b >> 0) & 3;
    }

    for (int i = 0; i < 32 * 32; ++i) {
        alt_tile_grid_[i] = rd16(d, 0x1000 + static_cast<size_t>(i) * 2);
    }

    size_t tile_bytes = d.size() - 0x1800;
    alt_tile_count_ = static_cast<int>(tile_bytes / 256);
    alt_tile_data_.assign(d.begin() + 0x1800, d.begin() + 0x1800 + static_cast<ptrdiff_t>(alt_tile_count_ * 256));
    has_alt_terrain_ = (alt_tile_count_ > 6);
    return true;
}

bool ModernRenderer::is_alt_water_at(double wx, double wy) const {
    if (!has_alt_terrain_) return false;
    int col = static_cast<int>((wy + kTheaterHalfSpan) / kAltTileUnits);
    int row = static_cast<int>((kTheaterHalfSpan - wx) / kAltTileUnits);
    if (row < 0 || row >= 32 || col < 0 || col >= 32) return false;
    uint16_t tid = alt_tile_grid_[row * 32 + col] & 0x01FFu;
    return (tid == 0 || tid == 1);
}

bool ModernRenderer::load_objects(const std::string& objects_path) {
    std::ifstream f(objects_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 256) return false;

    uint32_t first_off = rd32(d, 0);
    if (first_off < 4 || first_off > 4096 || (first_off % 4) != 0) return false;
    size_t nmodels = first_off / 4;
    models_.clear();
    models_.resize(nmodels);

    for (size_t m = 0; m < nmodels; ++m) {
        uint32_t o0 = rd32(d, m * 4);
        uint32_t o1 = (m + 1 < nmodels) ? rd32(d, (m + 1) * 4) : static_cast<uint32_t>(d.size());
        if (o0 >= d.size() || o1 <= o0 + 64 || o1 > d.size()) continue;
        // Every model record begins with a 2-byte length prefix (u16 == mlen - 2);
        // ALL internal pointers (lod_off, vptr, fptr, octants, faces) are relative to base = o0 + 2!
        size_t base = static_cast<size_t>(o0) + 2;
        size_t blen = static_cast<size_t>(o1) - base;

        ObjModel& mod = models_[m];
        if (blen >= 10) {
            for (int c = 0; c < 6; ++c) {
                char ch = static_cast<char>(d[base + 2 + c]);
                if (ch >= 32 && ch < 127) mod.name.push_back(ch);
            }
        }

        size_t lod_off = 0;
        size_t max_scan = std::min<size_t>(blen - 12, 200);
        for (size_t s = 10; s + 10 <= max_scan; s += 2) {
            if (rd16(d, base + s) == 0xFFFF && rd16(d, base + s + 2) == 0x4000) {
                lod_off = s + 4;
                break;
            }
        }

        uint16_t nverts = 0, vptr = 0, fptr = 0;
        if (lod_off > 0 && lod_off + 6 <= blen) {
            uint16_t w0 = rd16(d, base + lod_off + 0);
            uint16_t w1 = rd16(d, base + lod_off + 2);
            uint16_t w2 = rd16(d, base + lod_off + 4);
            // Articulated / swing-wing / helicopter / AWACS models store an animation table
            // after 0xFFFF 0x4000 with w1 == 0 and w0 = real LOD0 descriptor offset (relative to base)!
            if (w1 == 0 && static_cast<size_t>(w0) + 6 <= blen) {
                lod_off = static_cast<size_t>(w0);
                nverts = rd16(d, base + lod_off + 0);
                vptr   = rd16(d, base + lod_off + 2);
                fptr   = rd16(d, base + lod_off + 4);
            } else {
                nverts = w0;
                vptr   = w1;
                fptr   = w2;
            }
        }

        // Fallback scan for models with inline wing-sweep blocks before LOD0 (e.g., M01 Tornado)
        if (!(nverts > 0 && nverts <= 512 && vptr >= 0x20 &&
              static_cast<size_t>(vptr) + static_cast<size_t>(nverts) * 6 <= blen && fptr < blen)) {
            nverts = vptr = fptr = 0;
            size_t scan_end = std::min<size_t>(blen - 10, 0x260);
            for (size_t s = 0x20; s + 10 <= scan_end; s += 2) {
                uint16_t c_nv = rd16(d, base + s + 0);
                uint16_t c_vp = rd16(d, base + s + 2);
                uint16_t c_fp = rd16(d, base + s + 4);
                uint16_t z1   = rd16(d, base + s + 6);
                uint16_t z2   = rd16(d, base + s + 8);
                if (c_nv >= 6 && c_nv <= 200 && c_vp >= 0x20 &&
                    static_cast<size_t>(c_vp) + static_cast<size_t>(c_nv) * 6 <= blen &&
                    c_fp > c_vp && c_fp < blen && z1 == 0 && z2 == 0) {
                    nverts = c_nv;
                    vptr = c_vp;
                    fptr = c_fp;
                    break;
                }
            }
        }
        if (nverts == 0 || nverts > 512 || vptr < 0x20 ||
            static_cast<size_t>(vptr) + static_cast<size_t>(nverts) * 6 > blen || fptr >= blen) continue;

        // Verified Falcon 3.0 local model axes: +X = Right, -Y = Up, +Z = Forward (Nose)
        // OpenGL model axes (facing -Z): X = +vx, Y = -vy, Z = -vz
        mod.verts.resize(nverts);
        for (uint16_t k = 0; k < nverts; ++k) {
            size_t vp = base + static_cast<size_t>(vptr) + static_cast<size_t>(k) * 6;
            int16_t vx = rds16(d, vp + 0);
            int16_t vy = rds16(d, vp + 2);
            int16_t vz = rds16(d, vp + 4);
            mod.verts[k] = {
                static_cast<float>(vx) * kObjUnitToKm,
                -static_cast<float>(vy) * kObjUnitToKm,
                -static_cast<float>(vz) * kObjUnitToKm
            };
        }

        // Walk ALL 8 octant painter's-algorithm draw lists (deduplicated by face offset) so no wings/tails are ever missing!
        std::queue<uint16_t> q;
        std::unordered_set<uint16_t> visited;

        if (static_cast<size_t>(fptr) + 2 <= blen) {
            uint16_t first_w = rd16(d, base + fptr);
            if (first_w > fptr && first_w < blen) {
                for (int oi = 0; oi < 8; ++oi) {
                    if (static_cast<size_t>(fptr) + static_cast<size_t>(oi) * 2 + 2 > blen) break;
                    uint16_t oct_ptr = rd16(d, base + fptr + static_cast<size_t>(oi) * 2);
                    if (oct_ptr == 0xFFFF || oct_ptr >= blen) break;
                    size_t p = base + oct_ptr;
                    while (p + 2 <= o1) {
                        uint16_t w = rd16(d, p);
                        p += 2;
                        if (w == 0xFFFF) break;
                        if (w < blen) q.push(w);
                    }
                }
            } else {
                size_t p = base + fptr;
                while (p + 2 <= o1) {
                    uint16_t w = rd16(d, p);
                    p += 2;
                    if (w == 0xFFFF) break;
                    if (w < blen) q.push(w);
                }
            }
        }

        while (!q.empty()) {
            uint16_t gp = q.front();
            q.pop();
            if (visited.count(gp) || static_cast<size_t>(gp) + 6 > blen) continue;
            visited.insert(gp);

            uint16_t w0 = rd16(d, base + gp);
            if (w0 == 0xFFFF) {
                size_t p = base + gp + 2;
                while (p + 2 <= o1) {
                    uint16_t sub = rd16(d, p);
                    p += 2;
                    if (sub == 0xFFFF) break;
                    if (sub < blen && !visited.count(sub)) q.push(sub);
                }
                continue;
            }

            // Face record @ base+gp: [flags, color_w, nv, v0*16, v1*16, ..., v0*16]
            uint16_t color_w = rd16(d, base + gp + 2);
            uint16_t nv = rd16(d, base + gp + 4);
            if (nv < 3 || nv > 16 || static_cast<size_t>(gp) + 6 + static_cast<size_t>(nv) * 2 > blen) continue;

            ObjFace face;
            face.color_idx = static_cast<uint8_t>(color_w & 0xFF);
            face.verts.reserve(nv);
            bool valid = true;
            for (uint16_t k = 0; k < nv; ++k) {
                uint16_t vi = rd16(d, base + gp + 6 + static_cast<size_t>(k) * 2) / 16;
                if (vi >= nverts) { valid = false; break; }
                face.verts.push_back(vi);
            }
            if (!valid) continue;

            const ObjVec3& a = mod.verts[face.verts[0]];
            const ObjVec3& b = mod.verts[face.verts[1]];
            const ObjVec3& c = mod.verts[face.verts[2]];
            float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            float nx = uy * vz - uz * vy;
            float ny = uz * vx - ux * vz;
            float nz = ux * vy - uy * vx;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-7f) { nx /= len; ny /= len; nz /= len; }
            else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
            face.normal = {nx, ny, nz};
            mod.faces.push_back(std::move(face));
        }
    }
    return !models_.empty();
}

bool ModernRenderer::load_mapobj_models(const std::string& mapobj_path) {
    std::ifstream f(mapobj_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 512) return false;

    uint32_t first_off = rd32(d, 0);
    if (first_off < 4 || first_off > 4096 || (first_off % 4) != 0) return false;
    size_t nmodels = first_off / 4;
    mapobj_models_.clear();
    mapobj_models_.resize(nmodels);
    constexpr float kMapObjUnitToKm = 0.0000381f; // 0.125 ft/unit (buildings ~55 ft, runways/bridges ~1600 ft)

    for (size_t m = 0; m < nmodels; ++m) {
        uint32_t o0 = rd32(d, m * 4);
        uint32_t o1 = (m + 1 < nmodels) ? rd32(d, (m + 1) * 4) : static_cast<uint32_t>(d.size());
        if (o0 >= d.size() || o1 <= o0 + 32 || o1 > d.size()) continue;
        // Same 2-byte length prefix (u16 == mlen - 2) as objects -> all pointers relative to base = o0 + 2!
        size_t base = static_cast<size_t>(o0) + 2;
        size_t blen = static_cast<size_t>(o1) - base;

        // Mapobj LOD table: type-0x09 header stores highest-detail LOD0 pointer at base + 13
        // (unlocks ALL 120 mapobj models including all 4 directions of 3D runways 26..49, bridges, SAMs & hangars!).
        // Fallback to far-LOD 0xFFFF/0x4000 scan for any non-0x09 model.
        size_t lod_off = 0;
        if (blen > 20 && d[base] == 0x09) {
            uint16_t ptr0 = rd16(d, base + 13);
            if (static_cast<size_t>(ptr0) + 6 <= blen) {
                lod_off = ptr0;
            }
        }
        if (lod_off == 0) {
            size_t max_scan = std::min<size_t>(blen - 10, 128);
            for (size_t s = 2; s + 10 <= max_scan; ++s) {
                if (rd16(d, base + s) == 0xFFFF && rd16(d, base + s + 2) == 0x4000) {
                    lod_off = s + 4;
                    break;
                }
            }
        }
        if (lod_off == 0) continue;

        uint16_t nverts = rd16(d, base + lod_off + 0);
        uint16_t vptr   = rd16(d, base + lod_off + 2);
        uint16_t fptr   = rd16(d, base + lod_off + 4);
        if (nverts == 0 || nverts > 512 || static_cast<size_t>(vptr) + nverts * 6 > blen || fptr >= blen) continue;

        ObjModel& mod = mapobj_models_[m];
        mod.verts.resize(nverts);
        for (uint16_t k = 0; k < nverts; ++k) {
            size_t vp = base + static_cast<size_t>(vptr) + static_cast<size_t>(k) * 6;
            int16_t vx = rds16(d, vp + 0);
            int16_t vy = rds16(d, vp + 2);
            int16_t vz = rds16(d, vp + 4);
            mod.verts[k] = {
                static_cast<float>(vx) * kMapObjUnitToKm,
                -static_cast<float>(vy) * kMapObjUnitToKm,
                -static_cast<float>(vz) * kMapObjUnitToKm
            };
        }

        // Parse face group pointer table at base + fptr (terminated by 0xFFFF)
        std::vector<size_t> groups;
        size_t p = base + fptr;
        while (p + 2 <= o1) {
            uint16_t w = rd16(d, p);
            p += 2;
            if (w == 0xFFFF) break;
            if (w < blen) groups.push_back(base + w);
            else { groups.clear(); groups.push_back(p - 2); break; }
        }
        if (groups.empty() && p < o1) groups.push_back(p);

        float max_used_abs_y = 0.0f;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            size_t gp = groups[gi];
            if (gp + 6 > o1) continue;
            uint16_t flags = rd16(d, gp);
            uint16_t color_w = rd16(d, gp + 2);
            size_t fp_cur = gp + 4;
            bool multi = ((flags & 0x1000) != 0);
            do {
                if (fp_cur + 2 > o1) break;
                uint16_t nv = rd16(d, fp_cur);
                fp_cur += 2;
                if (nv == 0xFFFF || nv < 3 || nv > 32) break;
                if (fp_cur + (static_cast<size_t>(nv) + 1) * 2 > o1) break;

                ObjFace face;
                face.color_idx = static_cast<uint8_t>(color_w & 0xFF);
                face.layer_idx = static_cast<uint8_t>(std::min<size_t>(gi, 255));
                face.verts.reserve(nv);
                bool valid = true;
                for (uint16_t k = 0; k < nv; ++k) {
                    uint16_t vi = rd16(d, fp_cur + static_cast<size_t>(k) * 2) / 16;
                    if (vi >= nverts) { valid = false; break; }
                    face.verts.push_back(vi);
                }
                fp_cur += (static_cast<size_t>(nv) + 1) * 2;
                if (!valid) continue;

                for (uint16_t vi : face.verts) {
                    max_used_abs_y = std::max(max_used_abs_y, std::abs(mod.verts[vi].y));
                }

                const ObjVec3& a = mod.verts[face.verts[0]];
                const ObjVec3& b = mod.verts[face.verts[1]];
                const ObjVec3& c = mod.verts[face.verts[2]];
                float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
                float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
                float nx = uy * vz - uz * vy;
                float ny = uz * vx - ux * vz;
                float nz = ux * vy - uy * vx;
                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 1e-7f) { nx /= len; ny /= len; nz /= len; }
                else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
                face.normal = {nx, ny, nz};
                mod.faces.push_back(std::move(face));
            } while (multi);
        }
        mod.is_ground_decal = !mod.faces.empty() && (max_used_abs_y < 0.0025f);
    }
    return !mapobj_models_.empty();
}

bool ModernRenderer::load_mapobj_world(const std::string& mapobj_irq_path) {
    std::ifstream f(mapobj_irq_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> m((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (m.size() < 256) return false;

    map_features_.clear();
    road_segments_.clear();
    map_structures_.clear();

    constexpr double kSecSz = (2.0 * kTheaterHalfSpan) / 8.0; // 838,400 units
    constexpr double kSubSz = kSecSz / 4.0;                   // 209,600 units
    constexpr double kRowSz = kSubSz / 16.0;                  // 13,100 units
    constexpr double kCellSz = kRowSz / 4.0;                  // 3,275 units (~0.50 km)

    struct GridFeat {
        int gr = 0, gc = 0;
        double wx = 0.0, wy = 0.0;
        uint8_t lo_mask = 0;
        uint8_t hi_mask = 0;
        uint8_t kind = 0; // 0 = road, 1 = river (hi & 0x40), 2 = railroad / range boundary (hi & 0x80)
    };
    std::vector<GridFeat> gfeats;
    std::unordered_map<uint32_t, size_t> feat_grid;
    auto cell_key = [](int gr, int gc) -> uint32_t {
        return (static_cast<uint32_t>(gr & 0xFFF) << 12) | static_cast<uint32_t>(gc & 0xFFF);
    };

    uint32_t secs[64]{};
    for (int i = 0; i < 64; ++i) secs[i] = rd32(m, static_cast<size_t>(i) * 4);

    for (int si = 0; si < 64; ++si) {
        uint32_t so = secs[si];
        if (so == 0xFFFFFFFFu || so + 32 > m.size()) continue;
        uint32_t s_next = static_cast<uint32_t>(m.size());
        for (int j = si + 1; j < 64; ++j) {
            if (secs[j] != 0xFFFFFFFFu && secs[j] > so) { s_next = secs[j]; break; }
        }
        int sr = si / 8, sc = si % 8;

        for (int subi = 0; subi < 16; ++subi) {
            uint32_t bo = so + rd16(m, so + static_cast<size_t>(subi) * 2);
            uint32_t bnext = (subi < 15) ? (so + rd16(m, so + static_cast<size_t>(subi + 1) * 2)) : s_next;
            if (bnext <= bo || bnext > m.size() || bnext - bo < 20) continue;
            int subr = subi / 4, subc = subi % 4;
            uint32_t rel = bo + 0x12;
            if (rel >= bnext) continue;
            uint8_t nr = m[rel];
            if (nr == 0 || nr > 16) continue;

            uint32_t p = rel + 1;
            for (int ri = 0; ri < nr && p + 3 <= bnext; ++ri) {
                uint8_t r_id = m[p];
                uint16_t r_off = rd16(m, p + 1);
                p += 3;
                uint32_t cp = rel + r_off;
                if (cp >= bnext) continue;
                uint8_t nc = m[cp++];
                for (int ci = 0; ci < nc && cp + 3 <= bnext; ++ci) {
                    uint8_t c_id = m[cp];
                    uint16_t c_off = rd16(m, cp + 1);
                    cp += 3;
                    uint32_t lp = rel + c_off;
                    if (lp >= bnext) continue;
                    uint8_t nl = m[lp++];
                    for (int li = 0; li < nl && lp + 3 <= bnext; ++li) {
                        uint8_t sub_cell = m[lp];
                        uint16_t l_off = rd16(m, lp + 1);
                        lp += 3;
                        uint32_t op = rel + l_off;
                        if (op + 3 > bnext) continue;

                        double cell_wx_n = kTheaterHalfSpan - (sr * kSecSz + subr * kSubSz + (r_id + (sub_cell >> 2) * 0.25) * kRowSz);
                        double cell_wy_w = -kTheaterHalfSpan + (sc * kSecSz + subc * kSubSz + (c_id + (sub_cell & 3) * 0.25) * kRowSz);

                        uint8_t hdr_b = m[op];
                        bool buildings_only = (hdr_b & 0x80u) != 0;
                        uint8_t nobj = hdr_b & 0x7Fu;
                        uint32_t obj_base = buildings_only ? (op + 1) : (op + 3);

                        if (!buildings_only) {
                            uint8_t lo_mask = m[op + 1];
                            uint8_t hi_mask = m[op + 2];
                            if (lo_mask != 0 || hi_mask != 0) {
                                double cwx = cell_wx_n - kCellSz * 0.5;
                                double cwy = cell_wy_w + kCellSz * 0.5;
                                map_features_.push_back({cwx, cwy, lo_mask, hi_mask});
                                int gr = sr * 256 + subr * 64 + static_cast<int>(r_id) * 4 + (sub_cell >> 2);
                                int gc = sc * 256 + subc * 64 + static_cast<int>(c_id) * 4 + (sub_cell & 3);
                                // Verified Falcon 3.0 MAPOBJ.* feature type bits in hi_mask:
                                //   hi & 0x40 = Real river (Colorado River/Lake Mead, Tigris & Euphrates!)
                                //   hi & 0x80 = Railroad / Restricted Range boundary (Nellis Range box, Iraqi rail line)
                                //   (hi & 0xC0) == 0 = Roads & highways (hi & 0x01 = full E-W straight, hi & 0x02 = full N-S straight!)
                                uint8_t kind = (hi_mask & 0x40u) ? 1u : ((hi_mask & 0x80u) ? 2u : 0u);
                                size_t idx = gfeats.size();
                                gfeats.push_back({gr, gc, cwx, cwy, lo_mask, hi_mask, kind});
                                feat_grid[cell_key(gr, gc)] = idx;
                            }
                        }
                        if (nobj > 0 && nobj < 64 && obj_base + static_cast<uint32_t>(nobj) * 10 <= bnext) {
                            for (int k = 0; k < nobj; ++k) {
                                uint32_t rp = obj_base + static_cast<uint32_t>(k) * 10;
                                uint16_t dy_u = rd16(m, rp + 0);
                                uint16_t dx_u = rd16(m, rp + 2);
                                uint8_t mid = m[rp + 6];
                                double ox = cell_wx_n - std::clamp(static_cast<double>(dx_u), 0.0, kCellSz);
                                double oy = cell_wy_w + std::clamp(static_cast<double>(dy_u), 0.0, kCellSz);
                                // Note: byte rp+8 is a per-facility instance ID, NOT a rotation (orientations are baked into model_id!)
                                map_structures_.push_back({ox, oy, mid, 0.0f});
                            }
                        }
                    }
                }
            }
        }
    }

    // Build crisp, razor-sharp connected road, river & rail/boundary segments directly from Falcon 3.0's 10-bit geometry mask:
    //   lo_mask low nibble:  0x01=S, 0x02=E, 0x04=W, 0x08=N (half-arms from center to cell edge)
    //   lo_mask high nibble: 0x10 (+0x80 paired neighbor) = exact 45-deg SW<->NE diagonal!
    //                        0x20 (+0x40 paired neighbor) = exact 45-deg NW<->SE diagonal!
    //   hi_mask low 2 bits:  0x01 = full-cell straight E<->W, 0x02 = full-cell straight N<->S!
    road_segments_.reserve(gfeats.size() * 2);
    const double h = kCellSz * 0.5;
    for (const auto& gf : gfeats) {
        uint8_t lm = gf.lo_mask;
        uint8_t hm = gf.hi_mask;
        uint8_t k = gf.kind;

        // 1. Full-cell straight segments (hi_mask 0x01 = W<->E, 0x02 = N<->S)
        if (hm & 0x01u) road_segments_.push_back({gf.wx, gf.wy - h, gf.wx, gf.wy + h, k});
        if (hm & 0x02u) road_segments_.push_back({gf.wx - h, gf.wy, gf.wx + h, gf.wy, k});

        // 2. Exact cardinal half-arms from cell center to edge (0x08=N, 0x01=S, 0x02=E, 0x04=W)
        if (lm & 0x08u) road_segments_.push_back({gf.wx, gf.wy, gf.wx + h, gf.wy, k});
        if (lm & 0x01u) road_segments_.push_back({gf.wx, gf.wy, gf.wx - h, gf.wy, k});
        if (lm & 0x02u) road_segments_.push_back({gf.wx, gf.wy, gf.wx, gf.wy + h, k});
        if (lm & 0x04u) road_segments_.push_back({gf.wx, gf.wy, gf.wx, gf.wy - h, k});

        // 3. Exact 45-degree straight diagonals (zero stair-stepping!):
        //    0x10 = SW->NE diagonal across cell (0x80 is its adjacent SE half-cell clip -> skip if 0x10 neighbor exists)
        if (lm & 0x10u) {
            road_segments_.push_back({gf.wx - h, gf.wy - h, gf.wx + h, gf.wy + h, k});
        } else if (lm & 0x80u) {
            bool has_pair = false;
            if (auto it = feat_grid.find(cell_key(gf.gr, gf.gc - 1)); it != feat_grid.end() && gfeats[it->second].kind == k && (gfeats[it->second].lo_mask & 0x10u)) has_pair = true;
            if (auto it = feat_grid.find(cell_key(gf.gr - 1, gf.gc)); it != feat_grid.end() && gfeats[it->second].kind == k && (gfeats[it->second].lo_mask & 0x10u)) has_pair = true;
            if (!has_pair) road_segments_.push_back({gf.wx - h, gf.wy - h, gf.wx + h, gf.wy + h, k});
        }
        //    0x20 = NW->SE diagonal across cell (0x40 is its adjacent SW/NE clip -> skip if 0x20 neighbor exists)
        if (lm & 0x20u) {
            road_segments_.push_back({gf.wx + h, gf.wy - h, gf.wx - h, gf.wy + h, k});
        } else if (lm & 0x40u) {
            bool has_pair = false;
            if (auto it = feat_grid.find(cell_key(gf.gr, gf.gc - 1)); it != feat_grid.end() && gfeats[it->second].kind == k && (gfeats[it->second].lo_mask & 0x20u)) has_pair = true;
            if (auto it = feat_grid.find(cell_key(gf.gr, gf.gc + 1)); it != feat_grid.end() && gfeats[it->second].kind == k && (gfeats[it->second].lo_mask & 0x20u)) has_pair = true;
            if (auto it = feat_grid.find(cell_key(gf.gr - 1, gf.gc)); it != feat_grid.end() && gfeats[it->second].kind == k && (gfeats[it->second].lo_mask & 0x20u)) has_pair = true;
            if (auto it = feat_grid.find(cell_key(gf.gr + 1, gf.gc)); it != feat_grid.end() && gfeats[it->second].kind == k && (gfeats[it->second].lo_mask & 0x20u)) has_pair = true;
            if (!has_pair) road_segments_.push_back({gf.wx + h, gf.wy - h, gf.wx - h, gf.wy + h, k});
        }
    }
    sync_ils_mapobj_coverage();
    return !map_features_.empty() || !map_structures_.empty();
}

void ModernRenderer::sync_ils_mapobj_coverage() {
    for (auto& rw : ils_runways_) {
        rw.has_mapobj_runway = false;
        for (const auto& st : map_structures_) {
            if (st.model_id >= 26 && st.model_id <= 49) {
                if (std::abs(st.wx - rw.wx) < 8000.0 && std::abs(st.wy - rw.wy) < 8000.0) {
                    rw.has_mapobj_runway = true;
                    break;
                }
            }
        }
    }
}

bool ModernRenderer::load_ils_runways(const std::string& ils_path) {
    std::ifstream f(ils_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 17) return false;

    ils_runways_.clear();
    size_t count = (d.size() - 2) / 15;
    for (size_t i = 0; i < count; ++i) {
        size_t p = 2 + i * 15;
        int32_t wx = rds32(d, p + 0);
        int32_t wy = rds32(d, p + 4);
        int32_t z = rds32(d, p + 8);
        // Verified 15-byte ILS.* layout across all 8 theaters:
        //   +0..+3: i32 wx (North, 0.5 ft), +4..+7: i32 wy (East, 0.5 ft),
        //   +8..+11: i32 z (elev, 0.5 ft), +12..+13: u16 14-bit BAM heading (0x0000=N, 0x1000=E, 0x2000=S, 0x3000=W), +14: flags
        uint16_t hdg = rd16(d, p + 12);
        float hdg_deg = static_cast<float>(hdg & 0x3FFFu) * (360.0f / 16384.0f);

        // Deduplicate padded duplicate slots (e.g. ILS.NEL repeats slot 3 in slots 4..9)
        bool dup = false;
        for (const auto& ex : ils_runways_) {
            if (std::abs(ex.wx - static_cast<double>(wx)) < 10.0 &&
                std::abs(ex.wy - static_cast<double>(wy)) < 10.0 &&
                std::abs(ex.heading_deg - hdg_deg) < 0.5f) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        IlsRunway rw;
        rw.wx = static_cast<double>(wx);
        rw.wy = static_cast<double>(wy);
        rw.alt_ft = static_cast<float>(z) * 0.5f;
        rw.heading_deg = hdg_deg;
        ils_runways_.push_back(rw);
    }
    sync_ils_mapobj_coverage();
    return !ils_runways_.empty();
}

int ModernRenderer::resolve_model_index(uint16_t type_id) const {
    if (models_.empty()) return -1;
    if (type_id < 0x22 && type_id < models_.size() && !models_[type_id].faces.empty()) {
        return static_cast<int>(type_id);
    }
    switch (type_id) {
        case 0x2A: return 0x15; // MiG-19
        case 0x2D: return 0x15; // MiG-19
        case 0x2B: return 0x1B; // Su-25 Frogfoot
        case 0x24: return 0x14; // MiG-21
        case 0x29: return 0x19; // Su-24
        case 0x2E: return 0x1E; // Tu-22
        case 0x2F: return 0x1F; // An-12
        default: break;
    }
    if (type_id >= 0x10 && (type_id - 0x10) < models_.size() && !models_[type_id - 0x10].faces.empty()) {
        return static_cast<int>(type_id - 0x10);
    }
    return 0x11 < models_.size() ? 0x11 : 3;
}

int ModernRenderer::resolve_ground_model_index(uint16_t type_id) const {
    if (models_.empty()) return -1;
    // Map Falcon 3.0 RAM ground-unit type_id (0x0121..0x0130, low byte 0x21..0x30) to game_data/objects 3D models:
    //   34=M60 tank, 35=AMX13 tank, 36=M113 APC, 37=SHAHIN SAM, 38=WALID, 41=BDRM, 42=BTR70, 43=ZSU57 AAA, 44=SCUD
    uint8_t lo = static_cast<uint8_t>(type_id & 0xFFu);
    int idx = -1;
    switch (lo) {
        case 0x21: idx = 34; break; // Armored MBT (T-72 / M60)
        case 0x22: idx = 34; break; // M60
        case 0x23: idx = 35; break; // AMX13 / Light tank
        case 0x24: idx = 36; break; // M113 APC
        case 0x25: idx = 37; break; // Shahine / SAM launcher
        case 0x26: idx = 38; break; // Walid / Truck
        case 0x27: idx = 42; break; // BTR-70 / Armored vehicle column (most common ground unit in Kuwait!)
        case 0x28: idx = 41; break; // BRDM recon
        case 0x29: idx = 41; break; // BRDM
        case 0x2A: idx = 42; break; // BTR-70
        case 0x2B: idx = 43; break; // ZSU-57-2 / AAA
        case 0x2C: idx = 44; break; // SCUD launcher
        case 0x2D: idx = 37; break; // SAM battery launcher
        case 0x2E: idx = 37; break; // SAM radar / launcher
        case 0x2F: idx = 37; break; // SAM battery
        case 0x30: idx = 37; break; // SAM site / radar launcher
        default:
            if (lo >= 34 && lo <= 44) idx = lo;
            else idx = 34;
            break;
    }
    if (idx >= 0 && idx < static_cast<int>(models_.size()) && !models_[idx].faces.empty()) {
        return idx;
    }
    return -1;
}

std::string ModernRenderer::entity_type_name(uint16_t type_id, bool is_ground) const {
    int idx = is_ground ? resolve_ground_model_index(type_id) : resolve_model_index(type_id);
    if (idx >= 0 && idx < static_cast<int>(models_.size()) && !models_[idx].name.empty()) {
        std::string nm = models_[idx].name;
        while (!nm.empty() && (nm.back() == ' ' || nm.back() == '\0')) nm.pop_back();
        for (char& ch : nm) {
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        }
        if (!nm.empty()) return nm;
    }
    return is_ground ? "GROUND" : "BANDIT";
}

void ModernRenderer::update_telemetry(const FlightTelemetry& raw, float dt_sec) {
    if (!raw.in_flight) {
        smooth_tel_ = raw;
        has_smooth_tel_ = false;
        return;
    }

    auto angle_diff = [](float target, float cur) -> float {
        float d = target - cur;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return d;
    };

    // Snap immediately on view switches (e.g., Cockpit -> Chase / Side / Padlock) so camera never slides through the seat!
    bool view_jumped = false;
    if (has_smooth_tel_) {
        double dcx = raw.cam_x - smooth_tel_.cam_x;
        double dcy = raw.cam_y - smooth_tel_.cam_y;
        float dyaw = std::abs(angle_diff(raw.cam_yaw_deg, smooth_tel_.cam_yaw_deg));
        if (raw.is_external_cam != smooth_tel_.is_external_cam ||
            dyaw > 22.0f || (dcx * dcx + dcy * dcy) > 400.0 * 400.0) {
            view_jumped = true;
        }
    }

    bool hud_overlay_active = (mode_ == RenderMode::HybridCockpit) ||
                              ((mode_ == RenderMode::Modern3D || mode_ == RenderMode::VrStereoPreview) && vr_cfg_.use_original_hud);
    if (!has_smooth_tel_ || view_jumped || vr_cfg_.smooth_rate <= 0.01f || dt_sec <= 0.0f ||
        (vr_cfg_.sync_hud_no_lag && hud_overlay_active)) {
        smooth_tel_ = raw;
        has_smooth_tel_ = true;
        return;
    }

    float dt = std::clamp(dt_sec, 0.001f, 0.05f);
    float alpha = 1.0f - std::exp(-vr_cfg_.smooth_rate * dt);

    auto lerp_angle = [&](float cur, float target) -> float {
        return cur + angle_diff(target, cur) * alpha;
    };

    smooth_tel_.in_flight = true;
    smooth_tel_.world_x += (raw.world_x - smooth_tel_.world_x) * alpha;
    smooth_tel_.world_y += (raw.world_y - smooth_tel_.world_y) * alpha;
    smooth_tel_.altitude_ft += (raw.altitude_ft - smooth_tel_.altitude_ft) * alpha;
    smooth_tel_.agl_ft += (raw.agl_ft - smooth_tel_.agl_ft) * alpha;
    smooth_tel_.yaw_deg = lerp_angle(smooth_tel_.yaw_deg, raw.yaw_deg);
    while (smooth_tel_.yaw_deg < 0.0f) smooth_tel_.yaw_deg += 360.0f;
    while (smooth_tel_.yaw_deg >= 360.0f) smooth_tel_.yaw_deg -= 360.0f;
    smooth_tel_.pitch_deg += (raw.pitch_deg - smooth_tel_.pitch_deg) * alpha;
    smooth_tel_.roll_deg = lerp_angle(smooth_tel_.roll_deg, raw.roll_deg);
    smooth_tel_.airspeed_kts = raw.airspeed_kts;
    smooth_tel_.mach = raw.mach;
    smooth_tel_.g_load = raw.g_load;

    smooth_tel_.has_camera = raw.has_camera;
    smooth_tel_.is_external_cam = raw.is_external_cam;
    smooth_tel_.cam_x += (raw.cam_x - smooth_tel_.cam_x) * alpha;
    smooth_tel_.cam_y += (raw.cam_y - smooth_tel_.cam_y) * alpha;
    smooth_tel_.cam_alt_ft += (raw.cam_alt_ft - smooth_tel_.cam_alt_ft) * alpha;
    smooth_tel_.cam_yaw_deg = lerp_angle(smooth_tel_.cam_yaw_deg, raw.cam_yaw_deg);
    while (smooth_tel_.cam_yaw_deg < 0.0f) smooth_tel_.cam_yaw_deg += 360.0f;
    while (smooth_tel_.cam_yaw_deg >= 360.0f) smooth_tel_.cam_yaw_deg -= 360.0f;
    smooth_tel_.cam_pitch_deg += (raw.cam_pitch_deg - smooth_tel_.cam_pitch_deg) * alpha;
    smooth_tel_.cam_roll_deg = lerp_angle(smooth_tel_.cam_roll_deg, raw.cam_roll_deg);
    smooth_tel_.world_y_min = raw.world_y_min;
    smooth_tel_.world_y_max = raw.world_y_max;
    smooth_tel_.view_mode = raw.view_mode;
    smooth_tel_.padlock_active = raw.padlock_active;
    smooth_tel_.padlock_has_target = raw.padlock_has_target;
    smooth_tel_.padlock_target_is_missile = raw.padlock_target_is_missile;
    smooth_tel_.padlock_target_x = raw.padlock_target_x;
    smooth_tel_.padlock_target_y = raw.padlock_target_y;
    smooth_tel_.padlock_target_z = raw.padlock_target_z;
    smooth_tel_.padlock_range_nm = raw.padlock_range_nm;
    std::memcpy(smooth_tel_.padlock_target_name, raw.padlock_target_name, sizeof(smooth_tel_.padlock_target_name));
    smooth_tel_.entities = raw.entities;
    smooth_tel_.ground_units = raw.ground_units;
    smooth_tel_.missiles = raw.missiles;
}

void ModernRenderer::draw_vga_quad(int x, int y, int w, int h, GLuint tex,
                                   float v_min, float v_max, bool alpha_blend) {
    glViewport(x, y, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (alpha_blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    float u0 = alpha_blend ? 0.004f : 0.0f;
    float u1 = alpha_blend ? 0.996f : 1.0f;
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v_max); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(u1, v_max); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(u1, v_min); glVertex2f( 1.0f,  1.0f);
    glTexCoord2f(u0, v_min); glVertex2f(-1.0f,  1.0f);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

void ModernRenderer::draw_hud_overlay_fullscreen(int x, int y, int w, int h, GLuint vga_hud_tex) {
    glViewport(x, y, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float win_aspect = static_cast<float>(w) / static_cast<float>(std::max(1, h));
    float hud_half_h = 1.0f;
    float hud_half_w = hud_half_h * (4.0f / 3.0f) / win_aspect;
    float y_shift = -0.53f * hud_half_h;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, vga_hud_tex);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-hud_half_w, -hud_half_h + y_shift);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( hud_half_w, -hud_half_h + y_shift);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( hud_half_w,  hud_half_h + y_shift);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-hud_half_w,  hud_half_h + y_shift);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

void ModernRenderer::update_scene_lighting() {
    // Exact Falcon 3.0 ALTPAL.IRQ sky+horizon band order (bottom horizon -> top zenith):
    // VGA DAC 186..255 holds the 70-entry time-of-day palette (Dawn / Day / Sunset / Night):
    //   204..200 = 5 lower horizon haze bands
    //   194..186 = 9 upper sky gradient bands (195..199 are the 5 Persian Gulf water shades!)
    static constexpr int kSkyIdx[kSkyBandCount] = {
        204, 203, 202, 201, 200, 194, 193, 192, 191, 190, 189, 188, 187, 186
    };

    auto set_day = [&]() {
        lit_.daylight = 1.0f;
        lit_.night_amount = 0.0f;
        lit_.sunset_amount = 0.0f;
        // Exact ALTPAL.IRQ Palette 1 (Kuwait Midday)
        static constexpr float kDayBands[kSkyBandCount][3] = {
            {0.706f, 0.627f, 0.549f}, {0.690f, 0.627f, 0.596f}, {0.675f, 0.643f, 0.627f},
            {0.659f, 0.659f, 0.722f}, {0.643f, 0.690f, 0.816f}, {0.706f, 0.706f, 0.973f},
            {0.706f, 0.722f, 0.973f}, {0.690f, 0.722f, 0.973f}, {0.675f, 0.706f, 0.973f},
            {0.659f, 0.690f, 0.973f}, {0.643f, 0.675f, 0.973f}, {0.627f, 0.659f, 0.957f},
            {0.596f, 0.627f, 0.973f}, {0.565f, 0.612f, 0.957f}
        };
        std::memcpy(lit_.sky_bands, kDayBands, sizeof(kDayBands));
        std::memcpy(lit_.sky_top, lit_.sky_bands[kSkyBandCount - 1], sizeof(lit_.sky_top));
        std::memcpy(lit_.sky_mid, lit_.sky_bands[6], sizeof(lit_.sky_mid));
        std::memcpy(lit_.horizon, lit_.sky_bands[0], sizeof(lit_.horizon));
        lit_.ground_fill[0] = 0.660f; lit_.ground_fill[1] = 0.580f; lit_.ground_fill[2] = 0.500f;
        lit_.ground_mul[0] = lit_.ground_mul[1] = lit_.ground_mul[2] = 1.0f;
        lit_.water_mul[0]  = lit_.water_mul[1]  = lit_.water_mul[2]  = 1.0f;
        lit_.cloud_rgb[0] = 1.0f; lit_.cloud_rgb[1] = 1.0f; lit_.cloud_rgb[2] = 0.933f;
    };

    if (vr_cfg_.lighting_mode == LightingMode::Day) {
        set_day();
        return;
    }
    if (vr_cfg_.lighting_mode == LightingMode::Dusk) {
        lit_.daylight = 0.72f;
        lit_.night_amount = 0.35f;
        lit_.sunset_amount = 1.0f;
        // Authentic Falcon 3.0 ALTPAL.IRQ Palette 2 (Iconic Kuwait Violet -> Magenta -> Coral -> Peach Sunset!)
        static constexpr float kSunsetBands[kSkyBandCount][3] = {
            {0.784f, 0.627f, 0.627f}, {0.769f, 0.580f, 0.580f}, {0.769f, 0.549f, 0.549f},
            {0.769f, 0.518f, 0.518f}, {0.800f, 0.502f, 0.533f}, {0.847f, 0.502f, 0.502f},
            {0.847f, 0.518f, 0.565f}, {0.847f, 0.518f, 0.675f}, {0.816f, 0.502f, 0.722f},
            {0.816f, 0.471f, 0.784f}, {0.784f, 0.439f, 0.831f}, {0.753f, 0.424f, 0.910f},
            {0.737f, 0.408f, 0.957f}, {0.675f, 0.424f, 0.941f}
        };
        std::memcpy(lit_.sky_bands, kSunsetBands, sizeof(kSunsetBands));
        std::memcpy(lit_.sky_top, lit_.sky_bands[kSkyBandCount - 1], sizeof(lit_.sky_top));
        std::memcpy(lit_.sky_mid, lit_.sky_bands[6], sizeof(lit_.sky_mid));
        std::memcpy(lit_.horizon, lit_.sky_bands[0], sizeof(lit_.horizon));
        lit_.ground_fill[0] = 0.74f; lit_.ground_fill[1] = 0.58f; lit_.ground_fill[2] = 0.56f;
        lit_.ground_mul[0] = 1.08f; lit_.ground_mul[1] = 0.96f; lit_.ground_mul[2] = 1.06f;
        lit_.water_mul[0]  = 1.14f; lit_.water_mul[1]  = 1.06f; lit_.water_mul[2]  = 1.00f;
        lit_.cloud_rgb[0]  = 0.96f; lit_.cloud_rgb[1]  = 0.74f; lit_.cloud_rgb[2]  = 0.74f;
        return;
    }
    if (vr_cfg_.lighting_mode == LightingMode::Night) {
        lit_.daylight = 0.14f;
        lit_.night_amount = 1.0f;
        lit_.sunset_amount = 0.0f;
        static constexpr float kNightBands[kSkyBandCount][3] = {
            {0.055f, 0.064f, 0.105f}, {0.090f, 0.100f, 0.130f}, {0.105f, 0.115f, 0.145f},
            {0.120f, 0.130f, 0.165f}, {0.135f, 0.140f, 0.205f}, {0.050f, 0.072f, 0.145f},
            {0.055f, 0.072f, 0.155f}, {0.060f, 0.080f, 0.170f}, {0.070f, 0.085f, 0.195f},
            {0.058f, 0.070f, 0.165f}, {0.045f, 0.055f, 0.135f}, {0.032f, 0.042f, 0.105f},
            {0.022f, 0.030f, 0.078f}, {0.012f, 0.018f, 0.048f}
        };
        std::memcpy(lit_.sky_bands, kNightBands, sizeof(kNightBands));
        std::memcpy(lit_.sky_top, lit_.sky_bands[kSkyBandCount - 1], sizeof(lit_.sky_top));
        std::memcpy(lit_.sky_mid, lit_.sky_bands[6], sizeof(lit_.sky_mid));
        std::memcpy(lit_.horizon, lit_.sky_bands[0], sizeof(lit_.horizon));
        lit_.ground_fill[0] = 0.075f; lit_.ground_fill[1] = 0.075f; lit_.ground_fill[2] = 0.092f;
        lit_.ground_mul[0] = 0.115f; lit_.ground_mul[1] = 0.125f; lit_.ground_mul[2] = 0.175f;
        lit_.water_mul[0]  = 0.080f; lit_.water_mul[1]  = 0.105f; lit_.water_mul[2]  = 0.185f;
        lit_.cloud_rgb[0] = 0.16f; lit_.cloud_rgb[1] = 0.18f; lit_.cloud_rgb[2] = 0.25f;
        return;
    }

    // LightingMode::AutoFromFalcon — dynamically read ALL 14 live sky/horizon bands & shading
    // directly from Falcon 3.0's VGA DAC (indices 186..255), preserving Sunsets, Dawns, Noon & Night 1:1!
    if (!has_vga_pal_) {
        set_day();
        return;
    }

    auto luma = [&](int idx) -> float {
        return 0.299f * vga_rgb_[idx * 3 + 0] + 0.587f * vga_rgb_[idx * 3 + 1] + 0.114f * vga_rgb_[idx * 3 + 2];
    };

    float sky_luma = 0.5f * (luma(186) + luma(193));
    float hor_luma = luma(203);
    float gnd_luma = luma(204);
    float world_luma = 0.45f * sky_luma + 0.25f * hor_luma + 0.30f * gnd_luma;

    // Detect warm sunset / dawn skies (red/coral dominance over green in sky band 192..194)
    float sunset_warmth = (vga_rgb_[194 * 3 + 0] - vga_rgb_[194 * 3 + 1]) +
                          (vga_rgb_[192 * 3 + 0] - vga_rgb_[192 * 3 + 1]);
    lit_.sunset_amount = std::clamp((sunset_warmth - 0.10f) / 0.45f, 0.0f, 1.0f);

    float dl = std::clamp((world_luma - 0.03f) / 0.54f, 0.0f, 1.0f);
    lit_.daylight = 0.14f + 0.86f * dl;
    lit_.night_amount = std::clamp((0.44f - world_luma) / 0.30f, 0.0f, 1.0f);

    // Copy all 14 live VGA sky & horizon bands directly, applying only a subtle moonlit floor at pitch-black night
    for (int i = 0; i < kSkyBandCount; ++i) {
        int idx = kSkyIdx[i];
        float r = vga_rgb_[idx * 3 + 0];
        float g = vga_rgb_[idx * 3 + 1];
        float b = vga_rgb_[idx * 3 + 2];
        // At full night, Falcon sets idx 204 = (0,0,0); smoothly use readable moonlit floor
        float t = static_cast<float>(i) / static_cast<float>(kSkyBandCount - 1);
        float min_r = 0.055f * (1.0f - t) + 0.012f * t;
        float min_g = 0.064f * (1.0f - t) + 0.018f * t;
        float min_b = 0.105f * (1.0f - t) + 0.048f * t;
        lit_.sky_bands[i][0] = std::max(min_r, r);
        lit_.sky_bands[i][1] = std::max(min_g, g);
        lit_.sky_bands[i][2] = std::max(min_b, b);
    }
    std::memcpy(lit_.sky_top, lit_.sky_bands[kSkyBandCount - 1], sizeof(lit_.sky_top));
    std::memcpy(lit_.sky_mid, lit_.sky_bands[6], sizeof(lit_.sky_mid));

    // Horizon fog matches band 0 (idx 204) by day/sunset/dawn, or idx 203/floor at night when 204 is black
    if (gnd_luma > 0.08f) {
        lit_.horizon[0] = lit_.sky_bands[0][0];
        lit_.horizon[1] = lit_.sky_bands[0][1];
        lit_.horizon[2] = lit_.sky_bands[0][2];
    } else {
        lit_.horizon[0] = std::max(0.055f, vga_rgb_[203 * 3 + 0] * 0.45f);
        lit_.horizon[1] = std::max(0.064f, vga_rgb_[203 * 3 + 1] * 0.45f);
        lit_.horizon[2] = std::max(0.105f, vga_rgb_[203 * 3 + 2] * 0.55f);
    }

    lit_.ground_fill[0] = std::max(0.075f, vga_rgb_[204 * 3 + 0]);
    lit_.ground_fill[1] = std::max(0.075f, vga_rgb_[204 * 3 + 1]);
    lit_.ground_fill[2] = std::max(0.092f, vga_rgb_[204 * 3 + 2]);

    // Ground & water multipliers track Falcon's exact live VGA color shift (Sunset rosy-peach, Dawn cool, Day sand, Night moonlit!)
    if (gnd_luma > 0.12f) {
        // Normalize directly against midday Kuwait sand idx 204 (0.706, 0.627, 0.549)
        lit_.ground_mul[0] = std::clamp(vga_rgb_[204 * 3 + 0] / 0.706f, 0.12f, 1.22f);
        lit_.ground_mul[1] = std::clamp(vga_rgb_[204 * 3 + 1] / 0.627f, 0.12f, 1.22f);
        lit_.ground_mul[2] = std::clamp(vga_rgb_[204 * 3 + 2] / 0.549f, 0.15f, 1.25f);
    } else {
        float night_tint_r = 0.115f + 0.885f * dl;
        float night_tint_g = 0.125f + 0.875f * dl;
        float night_tint_b = 0.175f + 0.825f * dl;
        lit_.ground_mul[0] = night_tint_r;
        lit_.ground_mul[1] = night_tint_g;
        lit_.ground_mul[2] = night_tint_b;
    }

    float water_luma = luma(199);
    if (water_luma > 0.10f) {
        lit_.water_mul[0] = std::clamp(vga_rgb_[199 * 3 + 0] / 0.298f, 0.10f, 1.30f);
        lit_.water_mul[1] = std::clamp(vga_rgb_[199 * 3 + 1] / 0.424f, 0.12f, 1.25f);
        lit_.water_mul[2] = std::clamp(vga_rgb_[199 * 3 + 2] / 0.816f, 0.18f, 1.15f);
    } else {
        lit_.water_mul[0] = 0.080f + 0.920f * dl;
        lit_.water_mul[1] = 0.105f + 0.895f * dl;
        lit_.water_mul[2] = 0.185f + 0.815f * dl;
    }

    // Clouds pick up Falcon's live VGA cloud tint + warm coral sunset under-lighting!
    float cr = std::max(0.15f, vga_rgb_[176 * 3 + 0]);
    float cg = std::max(0.17f, vga_rgb_[176 * 3 + 1]);
    float cb = std::max(0.24f, vga_rgb_[176 * 3 + 2]);
    if (lit_.sunset_amount > 0.05f) {
        float sw = lit_.sunset_amount * 0.42f;
        cr = cr * (1.0f - sw) + lit_.sky_bands[5][0] * sw * 1.12f;
        cg = cg * (1.0f - sw) + lit_.sky_bands[5][1] * sw * 1.12f;
        cb = cb * (1.0f - sw) + lit_.sky_bands[5][2] * sw * 1.12f;
    }
    lit_.cloud_rgb[0] = std::min(1.0f, cr);
    lit_.cloud_rgb[1] = std::min(1.0f, cg);
    lit_.cloud_rgb[2] = std::min(1.0f, cb);
}

void ModernRenderer::draw_night_stars() {
    if (lit_.night_amount < 0.30f) return;
    float star_vis = std::clamp((lit_.night_amount - 0.30f) / 0.55f, 0.0f, 1.0f);

    glDisable(GL_FOG);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPointSize(1.8f);
    glBegin(GL_POINTS);
    for (int i = 0; i < 280; ++i) {
        float u = hash2f(i, 17, 911);
        float v = hash2f(i, 53, 357);
        float b = 0.45f + 0.55f * hash2f(i, 89, 733);
        float az = u * 2.0f * kPi;
        float el = 0.07f + v * (kPi * 0.46f);
        float ce = std::cos(el);
        float sx = ce * std::sin(az) * 205.0f;
        float sy = std::sin(el) * 205.0f;
        float sz = -ce * std::cos(az) * 205.0f;
        float warm = hash2f(i, 131, 619);
        glColor4f((0.82f + 0.18f * warm) * b, (0.86f + 0.10f * warm) * b, (1.0f - 0.12f * warm) * b,
                  star_vis * (0.45f + 0.55f * b));
        glVertex3f(sx, sy, sz);
    }
    glEnd();
    glPointSize(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (vr_cfg_.horizon_fog) glEnable(GL_FOG);
}

void ModernRenderer::draw_sky_backdrop(float pitch_deg, float roll_deg, float horizon_ndc_y, float vfov_deg, float aspect) {
    float asp = std::max(0.5f, aspect);
    float span_x = std::max(4.0f, asp * 3.0f + 2.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-asp, asp, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glPushMatrix();
    glTranslatef(0.0f, horizon_ndc_y, 0.0f);
    glRotatef(roll_deg, 0.0f, 0.0f, 1.0f);

    float tan_half_fov = std::max(0.08f, std::tan(vfov_deg * 0.5f * kDeg2Rad));
    auto elev_to_ndc = [&](float el_deg) -> float {
        float rel = std::clamp(el_deg - pitch_deg, -84.0f, 84.0f);
        return std::clamp(std::tan(rel * kDeg2Rad) / tan_half_fov, -4.0f, 4.0f);
    };

    // Exact Falcon 3.0 elevation spacing for the 14 sky+horizon gradient bands (0 deg horizon -> 9.6 deg zenith):
    // Matches classic VGA sunset band proportions 1:1 while rendering as a buttery-smooth HD gradient!
    static constexpr float kBandElevDeg[kSkyBandCount] = {
        0.00f, 0.42f, 0.85f, 1.30f, 1.78f,
        2.32f, 2.92f, 3.58f, 4.30f, 5.10f,
        6.00f, 7.05f, 8.25f, 9.65f
    };

    float y_hor = elev_to_ndc(0.0f);
    glBegin(GL_QUADS);

    // 1. Horizon ground fill below 0 deg
    glColor3fv(lit_.horizon);
    glVertex2f(-span_x, y_hor);
    glVertex2f( span_x, y_hor);
    glColor3fv(lit_.ground_fill);
    glVertex2f( span_x, -6.0f);
    glVertex2f(-span_x, -6.0f);

    // 2. Smooth 14-band live sky & sunset gradient from 0 deg horizon -> 9.65 deg
    for (int i = 0; i < kSkyBandCount - 1; ++i) {
        float y0 = elev_to_ndc(kBandElevDeg[i]);
        float y1 = elev_to_ndc(kBandElevDeg[i + 1]);
        if (y1 <= y0 + 1e-5f) continue;
        glColor3fv(lit_.sky_bands[i]);
        glVertex2f(-span_x, y0);
        glVertex2f( span_x, y0);
        glColor3fv(lit_.sky_bands[i + 1]);
        glVertex2f( span_x, y1);
        glVertex2f(-span_x, y1);
    }

    // 3. Upper zenith sky fill (band 13 -> +90 deg zenith, gently deepening toward zenith)
    float y_top_band = elev_to_ndc(kBandElevDeg[kSkyBandCount - 1]);
    if (y_top_band < 6.0f) {
        const float* zb = lit_.sky_bands[kSkyBandCount - 1];
        glColor3fv(zb);
        glVertex2f(-span_x, y_top_band);
        glVertex2f( span_x, y_top_band);
        glColor3f(zb[0] * 0.86f, zb[1] * 0.86f, std::min(1.0f, zb[2] * 0.94f));
        glVertex2f( span_x, 6.0f);
        glVertex2f(-span_x, 6.0f);
    }

    glEnd();
    glPopMatrix();
}

void ModernRenderer::draw_retro_desert_facets(const FlightTelemetry& tel) {
    // Classic 1990s flat-shaded low-poly desert terrain around the camera:
    // quantized Kuwaiti sand palette + gentle deterministic wadi/dune relief for crisp speed & depth cues!
    auto desert_h = [](int64_t gx, int64_t gz) -> float {
        float h1 = hash2f(gx, gz, 11);
        float h2 = hash2f(gx >> 1, gz >> 1, 29);
        return (h1 * 0.008f + h2 * 0.010f) - 0.005f; // Subtle -5m..+13m desert undulations
    };

    // Quantized 4-tone retro Kuwaiti desert sand palette (stays 100% authentic to idx 204 RGB(178,157,137)!)
    static constexpr float kSandPal[4][3] = {
        {0.725f, 0.643f, 0.556f}, // Sunlit alkali sand
        {0.698f, 0.616f, 0.537f}, // Classic Falcon 3.0 Kuwait sand
        {0.668f, 0.586f, 0.504f}, // Warm desert ochre
        {0.635f, 0.553f, 0.472f}  // Subtle wadi/dune shade
    };

    auto draw_ring = [&](double cell_km, int half_cells, int skip_inner_cells) {
        double cell_units = cell_km / kHalfFtToKm;
        int64_t center_gx = static_cast<int64_t>(std::floor(tel.cam_y / cell_units));
        int64_t center_gz = static_cast<int64_t>(std::floor(tel.cam_x / cell_units));

        glBegin(GL_TRIANGLES);
        for (int dz = -half_cells; dz < half_cells; ++dz) {
            for (int dx = -half_cells; dx < half_cells; ++dx) {
                if (skip_inner_cells > 0 &&
                    dx >= -skip_inner_cells && dx < skip_inner_cells &&
                    dz >= -skip_inner_cells && dz < skip_inner_cells) {
                    continue;
                }
                int64_t gx = center_gx + dx;
                int64_t gz = center_gz + dz;

                double wy0 = static_cast<double>(gx) * cell_units;
                double wy1 = static_cast<double>(gx + 1) * cell_units;
                double wx0 = static_cast<double>(gz) * cell_units;
                double wx1 = static_cast<double>(gz + 1) * cell_units;

                // Keep solid Persian Gulf water flat & unobstructed
                if (is_alt_water_at((wx0 + wx1) * 0.5, (wy0 + wy1) * 0.5)) continue;

                float x0 = static_cast<float>((wy0 - tel.cam_y) * kHalfFtToKm);
                float x1 = static_cast<float>((wy1 - tel.cam_y) * kHalfFtToKm);
                float z0 = static_cast<float>(-(wx0 - tel.cam_x) * kHalfFtToKm);
                float z1 = static_cast<float>(-(wx1 - tel.cam_x) * kHalfFtToKm);

                float y00 = desert_h(gx, gz);
                float y10 = desert_h(gx + 1, gz);
                float y01 = desert_h(gx, gz + 1);
                float y11 = desert_h(gx + 1, gz + 1);

                int p0 = static_cast<int>(hash2f(gx, gz, 73) * 4.0f) & 3;
                int p1 = static_cast<int>(hash2f(gx, gz, 137) * 4.0f) & 3;

                // Slight directional sun highlight on alternating triangles for crisp faceted depth
                glColor3f(kSandPal[p0][0] * 1.015f * lit_.ground_mul[0],
                          kSandPal[p0][1] * 1.015f * lit_.ground_mul[1],
                          kSandPal[p0][2] * 1.010f * lit_.ground_mul[2]);
                glVertex3f(x0, y00, z0);
                glVertex3f(x1, y10, z0);
                glVertex3f(x1, y11, z1);

                glColor3f(kSandPal[p1][0] * 0.980f * lit_.ground_mul[0],
                          kSandPal[p1][1] * 0.980f * lit_.ground_mul[1],
                          kSandPal[p1][2] * 0.985f * lit_.ground_mul[2]);
                glVertex3f(x0, y00, z0);
                glVertex3f(x1, y11, z1);
                glVertex3f(x0, y01, z1);
            }
        }
        glEnd();
    };

    // Near high-detail retro facets (~1.5 km cells out to 30 km) + Mid ring (~6 km cells out to 120 km)
    draw_ring(1.5, 20, 0);
    draw_ring(6.0, 20, 5);
}

void ModernRenderer::draw_low_alt_sand_dots(const FlightTelemetry& tel) {
    if (!vr_cfg_.low_alt_sand_dots) return;
    // Fade in retro desert ground-rush scrub/sand stipple below 6,500 ft MSL
    float alt_ft = tel.cam_alt_ft;
    if (alt_ft > 6800.0f) return;
    float alpha = std::clamp((6800.0f - alt_ft) / 4200.0f, 0.0f, 1.0f);
    if (alpha <= 0.02f) return;

    constexpr double kDotStepKm = 0.22; // ~220 m world-locked grid (zero swimming!)
    double step_units = kDotStepKm / kHalfFtToKm;
    int64_t cgx = static_cast<int64_t>(std::floor(tel.cam_y / step_units));
    int64_t cgz = static_cast<int64_t>(std::floor(tel.cam_x / step_units));
    int radius = 44; // ~9.7 km radius around aircraft

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPointSize(alt_ft < 2500.0f ? 2.8f : 2.0f);

    glBegin(GL_POINTS);
    for (int dz = -radius; dz <= radius; ++dz) {
        int64_t gz = cgz + dz;
        for (int dx = -radius; dx <= radius; ++dx) {
            int64_t gx = cgx + dx;
            // Thin out ~35% of dots deterministically so pattern looks organic, not grid-like
            float r0 = hash2f(gx, gz, 211);
            if (r0 < 0.35f) continue;

            double jx = (static_cast<double>(hash2f(gx, gz, 307)) - 0.5) * 0.85 * step_units;
            double jz = (static_cast<double>(hash2f(gx, gz, 419)) - 0.5) * 0.85 * step_units;
            double wy = (static_cast<double>(gx) + 0.5) * step_units + jx;
            double wx = (static_cast<double>(gz) + 0.5) * step_units + jz;

            if (is_alt_water_at(wx, wy)) continue;

            float x = static_cast<float>((wy - tel.cam_y) * kHalfFtToKm);
            float z = static_cast<float>(-(wx - tel.cam_x) * kHalfFtToKm);
            float dist_sq = x * x + z * z;
            if (dist_sq > 9.8f * 9.8f) continue;

            float dist_fade = std::clamp(1.0f - std::sqrt(dist_sq) / 9.8f, 0.0f, 1.0f);
            float a = alpha * dist_fade * 0.72f;
            if (r0 > 0.72f) {
                // Warm sunlit sand pebble fleck
                glColor4f(0.84f * lit_.ground_mul[0], 0.76f * lit_.ground_mul[1], 0.65f * lit_.ground_mul[2], a);
            } else {
                // Classic retro dark desert scrub / rock dot
                glColor4f(0.47f * lit_.ground_mul[0], 0.39f * lit_.ground_mul[1], 0.31f * lit_.ground_mul[2], a);
            }
            glVertex3f(x, 0.011f, z);
        }
    }
    glEnd();
    glPointSize(1.0f);
    glDisable(GL_BLEND);
}

void ModernRenderer::draw_alt_terrain(const FlightTelemetry& tel) {
    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, vr_cfg_.wireframe_terrain ? GL_LINE : GL_FILL);

    // Horizon base floor far beyond the faceted desert rings
    glBegin(GL_QUADS);
    glColor3f(0.698f * lit_.ground_mul[0], 0.616f * lit_.ground_mul[1], 0.537f * lit_.ground_mul[2]);
    glVertex3f(-230.0f, -0.015f, -230.0f);
    glVertex3f( 230.0f, -0.015f, -230.0f);
    glVertex3f( 230.0f, -0.015f,  230.0f);
    glVertex3f(-230.0f, -0.015f,  230.0f);
    glEnd();

    if (vr_cfg_.retro_desert_facets) {
        draw_retro_desert_facets(tel);
    }

    if (!has_alt_terrain_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        return;
    }

    int p_col = static_cast<int>((tel.cam_y + kTheaterHalfSpan) / kAltTileUnits);
    int p_row = static_cast<int>((kTheaterHalfSpan - tel.cam_x) / kAltTileUnits);
    int r0 = std::max(0, p_row - 6);
    int r1 = std::min(31, p_row + 6);
    int c0 = std::max(0, p_col - 6);
    int c1 = std::min(31, p_col + 6);

    for (int r = r0; r <= r1; ++r) {
        double wx_n = kTheaterHalfSpan - static_cast<double>(r) * kAltTileUnits;
        double wx_s = wx_n - kAltTileUnits;
        for (int c = c0; c <= c1; ++c) {
            double wy_w = -kTheaterHalfSpan + static_cast<double>(c) * kAltTileUnits;
            double wy_e = wy_w + kAltTileUnits;

            uint16_t entry = alt_tile_grid_[r * 32 + c];
            uint16_t tid = entry & 0x01FFu;

            if (tid == 0 || tid == 1) {
                // Solid Persian Gulf open-water tile subdivided into subtle 8x8 retro wave facets
                // (Both tid==0 and tid==1 are open sea tiles in ALT.IRQ!)
                constexpr int kWaterSub = 8;
                double dwx = (wx_n - wx_s) / kWaterSub;
                double dwy = (wy_e - wy_w) / kWaterSub;
                glBegin(GL_QUADS);
                for (int wr = 0; wr < kWaterSub; ++wr) {
                    float z0 = static_cast<float>(-((wx_n - wr * dwx) - tel.cam_x) * kHalfFtToKm);
                    float z1 = static_cast<float>(-((wx_n - (wr + 1) * dwx) - tel.cam_x) * kHalfFtToKm);
                    for (int wc = 0; wc < kWaterSub; ++wc) {
                        float x0 = static_cast<float>(((wy_w + wc * dwy) - tel.cam_y) * kHalfFtToKm);
                        float x1 = static_cast<float>(((wy_w + (wc + 1) * dwy) - tel.cam_y) * kHalfFtToKm);
                        float shimmer = (hash2f(c * 8 + wc, r * 8 + wr, 503) - 0.5f) * 0.045f;
                        glColor3f((0.282f + shimmer * 0.5f) * lit_.water_mul[0],
                                  (0.412f + shimmer * 0.8f) * lit_.water_mul[1],
                                  (0.808f + shimmer) * lit_.water_mul[2]);
                        glVertex3f(x0, 0.015f, z0);
                        glVertex3f(x1, 0.015f, z0);
                        glVertex3f(x1, 0.015f, z1);
                        glVertex3f(x0, 0.015f, z1);
                    }
                }
                glEnd();
            } else if (tid >= 6 && tid < alt_tile_count_) {
                // Detailed 16x16 mountain/hill elevation tile from ALT.*[0x1800 + tid*256]
                const uint8_t* tdata = &alt_tile_data_[static_cast<size_t>(tid) * 256];
                double cell_step = kAltTileUnits / 15.0;
                auto sample_h = [&](int sr, int sc) -> float {
                    uint8_t b = tdata[sr * 16 + sc];
                    // All real Falcon 3.0 terrain heights across all 8 theaters are signed 0..119;
                    // bytes >= 128 (225=0xE1, 236=0xEC, 250=0xFA, 255=0xFF) are flat sea-level / special sentinels!
                    if (b >= 128) return 0.0f;
                    return static_cast<float>(b) * 0.024f * vr_cfg_.terrain_scale;
                };

                glBegin(GL_TRIANGLES);
                for (int sr = 0; sr < 15; ++sr) {
                    double swx0 = wx_n - static_cast<double>(sr) * cell_step;
                    double swx1 = wx_n - static_cast<double>(sr + 1) * cell_step;
                    float z0 = static_cast<float>(-(swx0 - tel.cam_x) * kHalfFtToKm);
                    float z1 = static_cast<float>(-(swx1 - tel.cam_x) * kHalfFtToKm);
                    for (int sc = 0; sc < 15; ++sc) {
                        float h00 = sample_h(sr, sc);
                        float h10 = sample_h(sr, sc + 1);
                        float h01 = sample_h(sr + 1, sc);
                        float h11 = sample_h(sr + 1, sc + 1);
                        if (h00 <= 0.001f && h10 <= 0.001f && h01 <= 0.001f && h11 <= 0.001f) continue;

                        double swy0 = wy_w + static_cast<double>(sc) * cell_step;
                        double swy1 = wy_w + static_cast<double>(sc + 1) * cell_step;
                        float x0 = static_cast<float>((swy0 - tel.cam_y) * kHalfFtToKm);
                        float x1 = static_cast<float>((swy1 - tel.cam_y) * kHalfFtToKm);

                        auto emit_tri = [&](float ax, float ay, float az,
                                            float bx, float by, float bz,
                                            float cx, float cy, float cz) {
                            float ux = bx - ax, uy = by - ay, uz = bz - az;
                            float vx = cx - ax, vy = cy - ay, vz = cz - az;
                            float nx = uy * vz - uz * vy;
                            float ny = uz * vx - ux * vz;
                            float nz = ux * vy - uy * vx;
                            float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                            if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
                            if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
                            float ndotl = std::clamp(nx * 0.48f + ny * 0.76f + nz * 0.44f, 0.35f, 1.05f);
                            float avg_h = (ay + by + cy) * 0.333f;
                            float rock = std::clamp(avg_h / 2.2f, 0.0f, 1.0f);
                            glColor3f(std::min(1.0f, (0.69f - 0.14f * rock) * ndotl * lit_.ground_mul[0]),
                                      std::min(1.0f, (0.61f - 0.14f * rock) * ndotl * lit_.ground_mul[1]),
                                      std::min(1.0f, (0.52f - 0.12f * rock) * ndotl * lit_.ground_mul[2]));
                            glVertex3f(ax, ay, az);
                            glVertex3f(bx, by, bz);
                            glVertex3f(cx, cy, cz);
                        };
                        emit_tri(x0, h00, z0, x1, h10, z0, x1, h11, z1);
                        emit_tri(x0, h00, z0, x1, h11, z1, x0, h01, z1);
                    }
                }
                glEnd();
            }
        }
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void ModernRenderer::draw_isldat_water(const FlightTelemetry& tel) {
    if (isldat_tris_.empty() && isldat_land_tris_.empty() && isldat_urban_tris_.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonMode(GL_FRONT_AND_BACK, vr_cfg_.wireframe_terrain ? GL_LINE : GL_FILL);

    auto draw_tri_layer = [&](const std::vector<IslTriVert>& tris, float y_km, float poly_off,
                              float base_r, float base_g, float base_b, float tone_scale,
                              const float mul[3]) {
        if (tris.empty()) return;
        glPolygonOffset(poly_off, poly_off);
        glBegin(GL_TRIANGLES);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const auto& v0 = tris[i + 0];
            const auto& v1 = tris[i + 1];
            const auto& v2 = tris[i + 2];
            float x0 = static_cast<float>((v0.wy - tel.cam_y) * kHalfFtToKm);
            float z0 = static_cast<float>(-(v0.wx - tel.cam_x) * kHalfFtToKm);
            if (std::abs(x0) > 220.0f && std::abs(z0) > 220.0f) continue;
            float x1 = static_cast<float>((v1.wy - tel.cam_y) * kHalfFtToKm);
            float z1 = static_cast<float>(-(v1.wx - tel.cam_x) * kHalfFtToKm);
            float x2 = static_cast<float>((v2.wy - tel.cam_y) * kHalfFtToKm);
            float z2 = static_cast<float>(-(v2.wx - tel.cam_x) * kHalfFtToKm);
            float tone = (static_cast<float>((i / 3) % 3) - 1.0f) * tone_scale;
            glColor3f(std::clamp((base_r + tone * 0.6f) * mul[0], 0.0f, 1.0f),
                      std::clamp((base_g + tone * 0.8f) * mul[1], 0.0f, 1.0f),
                      std::clamp((base_b + tone) * mul[2], 0.0f, 1.0f));
            glVertex3f(x0, y_km, z0);
            glVertex3f(x1, y_km, z1);
            glVertex3f(x2, y_km, z2);
        }
        glEnd();
    };

    // Layer 1 (w1 == 1): Exact Falcon 3.0 Persian Gulf, Kuwait Bay & Iraqi lakes blue (idx 199)
    draw_tri_layer(isldat_tris_, 0.017f, -2.0f, 0.282f, 0.412f, 0.808f, 0.012f, lit_.water_mul);
    // Layer 2 (w1 == 0): Exact Falcon 3.0 Land / Islands ON TOP of water (Bubiyan Island, Failaka Island, coastlines!)
    draw_tri_layer(isldat_land_tris_, 0.019f, -3.0f, 0.706f, 0.624f, 0.545f, 0.008f, lit_.ground_mul);
    // Layer 3 (w1 == 2): Exact Falcon 3.0 Urban / City / Airbase concrete polygons (Kuwait City, Bubiyan base, Las Vegas!)
    draw_tri_layer(isldat_urban_tris_, 0.021f, -4.0f, 0.635f, 0.635f, 0.645f, 0.006f, lit_.ground_mul);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void ModernRenderer::draw_roads_and_rivers(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_roads_rivers || road_segments_.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-5.0f, -5.0f);

    // Draw crisp, continuous, ladder-free highways & Tigris/Euphrates rivers within 60 km
    constexpr float kMaxDistKm = 60.0f;

    glBegin(GL_QUADS);
    for (const auto& seg : road_segments_) {
        float x0 = static_cast<float>((seg.wy0 - tel.cam_y) * kHalfFtToKm);
        float z0 = static_cast<float>(-(seg.wx0 - tel.cam_x) * kHalfFtToKm);
        if (std::abs(x0) > kMaxDistKm || std::abs(z0) > kMaxDistKm) continue;
        float x1 = static_cast<float>((seg.wy1 - tel.cam_y) * kHalfFtToKm);
        float z1 = static_cast<float>(-(seg.wx1 - tel.cam_x) * kHalfFtToKm);

        float dx = x1 - x0;
        float dz = z1 - z0;
        float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-5f) continue;

        float w = (seg.kind == 1) ? 0.072f : ((seg.kind == 2) ? 0.013f : 0.016f); // Sleek ~32m highway, 144m river, 26m rail/boundary
        float y = (seg.kind == 1) ? 0.0225f : 0.0235f;
        float px = (-dz / len) * w;
        float pz = ( dx / len) * w;
        // Slight endcap overlap (0.55*w) prevents micro-cracks at 45-degree corners
        float ex = (dx / len) * (w * 0.55f);
        float ez = (dz / len) * (w * 0.55f);

        if (seg.kind == 1) {
            // Verified hi_mask & 0x40: Real Rivers (Colorado River / Lake Mead, Tigris & Euphrates!)
            glColor3f(0.282f * lit_.water_mul[0], 0.412f * lit_.water_mul[1], 0.808f * lit_.water_mul[2]);
        } else if (seg.kind == 2) {
            // Verified hi_mask & 0x80: Railroad / Restricted Range Boundary (subtle dark muted line)
            glColor3f(0.38f * lit_.ground_mul[0], 0.34f * lit_.ground_mul[1], 0.30f * lit_.ground_mul[2]);
        } else {
            // Roads & Highways (dark asphalt grey matching 2D Classic VGA!)
            glColor3f(0.32f * lit_.ground_mul[0], 0.30f * lit_.ground_mul[1], 0.29f * lit_.ground_mul[2]);
        }

        glVertex3f(x0 - ex - px, y, z0 - ez - pz);
        glVertex3f(x0 - ex + px, y, z0 - ez + pz);
        glVertex3f(x1 + ex + px, y, z1 + ez + pz);
        glVertex3f(x1 + ex - px, y, z1 + ez - pz);
    }
    glEnd();
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void ModernRenderer::draw_ils_runways(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_ground_objects || ils_runways_.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-6.0f, -6.0f);

    for (const auto& rw : ils_runways_) {
        float x = static_cast<float>((rw.wy - tel.cam_y) * kHalfFtToKm);
        float z = static_cast<float>(-(rw.wx - tel.cam_x) * kHalfFtToKm);
        if (std::abs(x) > 75.0f || std::abs(z) > 75.0f) continue;

        glPushMatrix();
        glTranslatef(x, 0.025f, z);
        glRotatef(-rw.heading_deg, 0.0f, 1.0f, 0.0f);

        // Exact Falcon 3.0 4-cell runway dimensions: 2.00 km (6,554 ft) x 61 m (200 ft),
        // with the ILS anchor situated 0.14 km inboard from the approach threshold (+0.14 -> -1.86 km).
        constexpr float kStartZ =  0.14f;
        constexpr float kEndZ   = -1.86f;
        constexpr float kHalfW  =  0.031f;

        // Draw fallback synthetic runway slab ONLY if this airbase has no native MAPOBJ 3D runway model (26..49)
        if (!rw.has_mapobj_runway) {
            glColor3f(0.28f * lit_.ground_mul[0], 0.29f * lit_.ground_mul[1], 0.31f * lit_.ground_mul[2]);
            glBegin(GL_QUADS);
            glVertex3f(-kHalfW, 0.0f, kStartZ);
            glVertex3f( kHalfW, 0.0f, kStartZ);
            glVertex3f( kHalfW, 0.0f, kEndZ);
            glVertex3f(-kHalfW, 0.0f, kEndZ);

            // Bright white retro threshold bars & dashed centerline
            float mark_s = 0.22f + 0.78f * lit_.daylight;
            glColor3f(0.92f * mark_s, 0.92f * mark_s, 0.90f * mark_s);
            glVertex3f(-kHalfW * 0.78f, 0.0008f, kStartZ - 0.04f);
            glVertex3f( kHalfW * 0.78f, 0.0008f, kStartZ - 0.04f);
            glVertex3f( kHalfW * 0.78f, 0.0008f, kStartZ - 0.10f);
            glVertex3f(-kHalfW * 0.78f, 0.0008f, kStartZ - 0.10f);

            glVertex3f(-kHalfW * 0.78f, 0.0008f, kEndZ + 0.10f);
            glVertex3f( kHalfW * 0.78f, 0.0008f, kEndZ + 0.10f);
            glVertex3f( kHalfW * 0.78f, 0.0008f, kEndZ + 0.04f);
            glVertex3f(-kHalfW * 0.78f, 0.0008f, kEndZ + 0.04f);

            for (float zc = kStartZ - 0.18f; zc > kEndZ + 0.18f; zc -= 0.16f) {
                glVertex3f(-0.003f, 0.0008f, zc);
                glVertex3f( 0.003f, 0.0008f, zc);
                glVertex3f( 0.003f, 0.0008f, zc - 0.08f);
                glVertex3f(-0.003f, 0.0008f, zc - 0.08f);
            }
            glEnd();
        }

        // Glowing night / dusk runway edge lights, green approach thresholds & red end-bars!
        if (lit_.night_amount > 0.25f) {
            float glow = std::clamp((lit_.night_amount - 0.25f) / 0.50f, 0.0f, 1.0f);
            glDisable(GL_FOG);
            glPointSize(3.2f);
            glBegin(GL_POINTS);
            // Green approach threshold bar
            glColor3f(0.20f * glow, 1.0f * glow, 0.35f * glow);
            for (int k = -3; k <= 3; ++k) {
                glVertex3f(static_cast<float>(k) * (kHalfW * 0.30f), 0.004f, kStartZ - 0.01f);
            }
            // Red far end stop bar
            glColor3f(1.0f * glow, 0.18f * glow, 0.15f * glow);
            for (int k = -3; k <= 3; ++k) {
                glVertex3f(static_cast<float>(k) * (kHalfW * 0.30f), 0.004f, kEndZ + 0.01f);
            }
            // Warm white / amber runway edge lights every 150m
            for (float zc = kStartZ - 0.12f; zc > kEndZ + 0.10f; zc -= 0.15f) {
                bool caution = (zc < kEndZ + 0.55f);
                if (caution) glColor3f(1.0f * glow, 0.72f * glow, 0.20f * glow);
                else         glColor3f(1.0f * glow, 0.96f * glow, 0.84f * glow);
                glVertex3f(-kHalfW * 1.12f, 0.004f, zc);
                glVertex3f( kHalfW * 1.12f, 0.004f, zc);
            }
            glEnd();
            glPointSize(1.0f);
            if (vr_cfg_.horizon_fog) glEnable(GL_FOG);
        }
        glPopMatrix();
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
}

float ModernRenderer::compute_smart_scale(float slant_km, bool is_ground) const {
    if (vr_cfg_.smart_scaling_mode == SmartScalingMode::Off) return 1.0f;
    if (is_ground && !vr_cfg_.smart_scale_ground) return 1.0f;

    auto smoothstep = [](float e0, float e1, float x) -> float {
        float t = std::clamp((x - e0) / std::max(1e-5f, e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    float slant_kft = std::max(0.0f, slant_km * 3.28084f); // 1 km = 3.28084 kft (1 NM = 6.076 kft)
    float strength = std::clamp(vr_cfg_.aircraft_scale, 0.25f, 3.0f);
    float air_scale = 1.0f;

    if (vr_cfg_.smart_scaling_mode == SmartScalingMode::Serfoss2003) {
        // Exact Gary Serfoss (2003) / Classic Falcon BMS polynomial clamped at parabola peak (31.17 kft ~= 5.13 NM):
        //   Factor = 1.0 + 0.09226 * R_kft - 0.00148 * R_kft^2  (1.00x @ 0 ft -> 1.50x @ 1 NM -> 2.44x @ 5.1+ NM)
        float R = std::min(slant_kft, 31.17f);
        float delta = 0.09226f * R - 0.00148f * R * R;
        air_scale = 1.0f + std::max(0.0f, delta) * strength;
    } else if (vr_cfg_.smart_scaling_mode == SmartScalingMode::Why485FovRes) {
        // Why485 / Chihirobelmo FOV + Resolution-aware Smart Scaling:
        // Compensates for display angular resolution (px/deg) vs human 20/20 eye acuity (60 px/deg),
        // keeping close-range formation/merge strictly 1:1 while scaling distant contacts consistently across
        // 720p / 1080p / 1440p / 4K / VR and narrow vs wide FOV!
        constexpr float kHumanEyePpd = 60.0f;
        constexpr float kSerfossRefDeficit = 60.0f / 28.0f; // 1080p @ 38.6 deg reference (~2.14x)
        float ppd = std::clamp(last_display_ppd_, 4.0f, 200.0f);
        float deficit = std::clamp(kHumanEyePpd / ppd, 1.0f, 4.5f);
        float fov_res_ratio = std::clamp(deficit / kSerfossRefDeficit, 0.35f, 2.35f);

        float close_ramp = smoothstep(0.45f, 2.0f, slant_kft); // Pure 1:1 inside ~140 m, smooth blend to 600 m
        float R = std::min(slant_kft, 31.17f);
        float serfoss_delta = std::max(0.0f, 0.09226f * R - 0.00148f * R * R);
        air_scale = std::clamp(1.0f + close_ramp * serfoss_delta * fov_res_ratio * strength, 1.0f, 5.5f);
    } else {
        // LegacyLinear (original host curve)
        air_scale = strength * 1.5f;
        if (slant_km > 1.5f) {
            air_scale *= std::min(3.0f, 1.0f + (slant_km - 1.5f) * 0.20f);
        }
    }

    if (!is_ground) return air_scale;

    // Ground structures (buildings, SAM/radar sites, factories, bridges):
    // Stay strictly 1:1 inside 0.8 km (takeoff/landing/low-level strafe), then apply a gentle,
    // non-overlapping distant visibility boost capped at 2.15x so buildings never balloon over runways!
    float gnd_ramp = smoothstep(0.8f, 3.0f, slant_km);
    float gnd_scale = 1.0f + gnd_ramp * (air_scale - 1.0f) * 0.48f;
    return std::clamp(gnd_scale, 1.0f, 2.15f);
}

void ModernRenderer::draw_ground_structures(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_ground_objects || map_structures_.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);

    for (const auto& st : map_structures_) {
        if (st.model_id >= mapobj_models_.size() || mapobj_models_[st.model_id].faces.empty()) continue;
        const ObjModel& mod = mapobj_models_[st.model_id];
        float x = static_cast<float>((st.wy - tel.cam_y) * kHalfFtToKm);
        float z = static_cast<float>(-(st.wx - tel.cam_x) * kHalfFtToKm);
        float max_r = mod.is_ground_decal ? 70.0f : 55.0f;
        if (std::abs(x) > max_r || std::abs(z) > max_r) continue;

        float base_y = mod.is_ground_decal ? 0.025f : 0.020f;
        float dy = cam_y_km - base_y;
        float slant_km = std::sqrt(x * x + dy * dy + z * z);
        // Flat ground decals (runways 26..49, taxiways 103, pads) are ALWAYS strictly 1:1 physical geometry!
        float gnd_scale = mod.is_ground_decal ? 1.0f : compute_smart_scale(slant_km, true);

        if (mod.is_ground_decal) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-6.0f, -6.0f);
        }

        glPushMatrix();
        glTranslatef(x, base_y, z);
        if (std::abs(st.yaw_deg) > 0.01f) {
            glRotatef(-st.yaw_deg, 0.0f, 1.0f, 0.0f);
        }
        if (gnd_scale > 1.001f) {
            glScalef(gnd_scale, gnd_scale, gnd_scale);
        }

        for (const auto& face : mod.faces) {
            if (face.verts.size() < 3) continue;
            // For coplanar runway/decal markings (painted numbers & threshold stripes in group 1+, color_idx 122),
            // lift cleanly above the concrete slab so there is zero Z-fighting at any altitude!
            float face_y_lift = 0.0f;
            if (mod.is_ground_decal && (face.layer_idx > 0 || face.color_idx == 122)) {
                face_y_lift = 0.0005f + 0.0002f * static_cast<float>(std::min<uint8_t>(face.layer_idx, 4));
            }
            float shade = mod.is_ground_decal
                ? 0.95f
                : (0.65f + 0.35f * std::abs(face.normal.x * 0.42f + face.normal.y * 0.80f + face.normal.z * 0.42f));
            float r = 0.58f, g = 0.55f, b = 0.50f;
            if (has_vga_pal_ && face.color_idx != 0) {
                r = vga_rgb_[face.color_idx * 3 + 0];
                g = vga_rgb_[face.color_idx * 3 + 1];
                b = vga_rgb_[face.color_idx * 3 + 2];
                if (r + g + b < 0.15f) { r = 0.44f * lit_.daylight; g = 0.42f * lit_.daylight; b = 0.38f * lit_.daylight; }
                // When user forces Dusk/Night override while Falcon is in daytime, tint model colors too!
                if (vr_cfg_.lighting_mode != LightingMode::AutoFromFalcon) {
                    r *= lit_.ground_mul[0]; g *= lit_.ground_mul[1]; b *= lit_.ground_mul[2];
                }
            } else {
                r *= lit_.ground_mul[0]; g *= lit_.ground_mul[1]; b *= lit_.ground_mul[2];
            }
            glColor3f(std::min(1.0f, r * shade), std::min(1.0f, g * shade), std::min(1.0f, b * shade));
            glBegin(GL_POLYGON);
            for (uint16_t vi : face.verts) {
                const ObjVec3& v = mod.verts[vi];
                glVertex3f(v.x, v.y + face_y_lift, v.z);
            }
            glEnd();
        }
        glPopMatrix();

        if (mod.is_ground_decal) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    // Subtle warm city & facility lights across Kuwait City / Basra / Las Vegas / airbases at night!
    if (lit_.night_amount > 0.30f) {
        float glow = std::clamp((lit_.night_amount - 0.30f) / 0.50f, 0.0f, 1.0f);
        glDisable(GL_FOG);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPointSize(2.6f);
        glBegin(GL_POINTS);
        for (size_t i = 0; i < map_structures_.size(); ++i) {
            const auto& st = map_structures_[i];
            if (st.model_id < mapobj_models_.size() && mapobj_models_[st.model_id].is_ground_decal) continue;
            float x = static_cast<float>((st.wy - tel.cam_y) * kHalfFtToKm);
            float z = static_cast<float>(-(st.wx - tel.cam_x) * kHalfFtToKm);
            if (std::abs(x) > 48.0f || std::abs(z) > 48.0f) continue;
            float warm = hash2f(static_cast<int64_t>(i), st.model_id, 709);
            glColor4f(1.0f, 0.72f + 0.22f * warm, 0.30f + 0.35f * warm, glow * 0.85f);
            glVertex3f(x, 0.032f, z);
        }
        glEnd();
        glPointSize(1.0f);
        glDisable(GL_BLEND);
        if (vr_cfg_.horizon_fog) glEnable(GL_FOG);
    }
}

void ModernRenderer::draw_clouds(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_clouds) return;

    // Sparse, world-locked, organically jittered retro flat clouds matching Classic 2D Falcon 3.0!
    // (Note: ALT.*[0..4095] is a coarse terrain/mountain-basin mask covering 29% of Nevada in a contiguous
    //  block — NOT a cloud grid — which previously created an unnatural ceiling-tile grid of rectangles!)
    constexpr double kCellKm = 13.5;
    constexpr double kCellUnits = kCellKm / kHalfFtToKm;
    constexpr int kRadiusCells = 9; // ~120 km horizon reach

    int64_t pc = static_cast<int64_t>(std::floor(tel.cam_y / kCellUnits));
    int64_t pr = static_cast<int64_t>(std::floor(tel.cam_x / kCellUnits));

    auto draw_cloud_poly = [](float cx, float y, float cz, float rx, float rz, float yaw, int64_t gx, int64_t gz) {
        constexpr int kSides = 10;
        float ca = std::cos(yaw), sa = std::sin(yaw);
        glBegin(GL_POLYGON);
        for (int k = 0; k < kSides; ++k) {
            float ang = (static_cast<float>(k) / static_cast<float>(kSides)) * 2.0f * kPi;
            // Deterministic organic lobe variation per vertex so every cloud has a unique retro silhouette
            float lobe = 0.78f + 0.34f * hash2f(gx * 13 + k, gz * 17 + k, 881);
            float lx = std::cos(ang) * rx * lobe;
            float lz = std::sin(ang) * rz * lobe;
            float wx = cx + lx * ca - lz * sa;
            float wz = cz + lx * sa + lz * ca;
            glVertex3f(wx, y, wz);
        }
        glEnd();
    };

    glEnable(GL_DEPTH_TEST);
    float sun_sh = std::clamp((lit_.daylight - 0.28f) / 0.50f, 0.0f, 1.0f);

    for (int64_t gr = pr - kRadiusCells; gr <= pr + kRadiusCells; ++gr) {
        for (int64_t gc = pc - kRadiusCells; gc <= pc + kRadiusCells; ++gc) {
            // Sparse ~16% natural coverage -> 1..4 scattered clouds visible at once, zero grid pattern!
            if (hash2f(gr, gc, 101) > 0.16f) continue;

            // Strong random jitter (+/- 42% of cell size) breaks all row/column alignment completely
            double jx = (static_cast<double>(hash2f(gr, gc, 211)) - 0.5) * 0.84 * kCellUnits;
            double jy = (static_cast<double>(hash2f(gr, gc, 307)) - 0.5) * 0.84 * kCellUnits;
            double wx = (static_cast<double>(gr) + 0.5) * kCellUnits + jx;
            double wy = (static_cast<double>(gc) + 0.5) * kCellUnits + jy;

            float cx = static_cast<float>((wy - tel.cam_y) * kHalfFtToKm);
            float cz = static_cast<float>(-(wx - tel.cam_x) * kHalfFtToKm);
            float dist_sq = cx * cx + cz * cz;
            if (dist_sq > 118.0f * 118.0f) continue;

            float rx  = 1.25f + 1.65f * hash2f(gr, gc, 401);
            float rz  = rx * (0.58f + 0.48f * hash2f(gr, gc, 509));
            float yaw = hash2f(gr, gc, 607) * 2.0f * kPi;
            float alt = 3.85f + 0.95f * hash2f(gr, gc, 701); // 12,600 .. 15,750 ft MSL

            // 1. Subtle matching rounded ground shadow on desert/land (skip over open water)
            if (vr_cfg_.ground_shadows && sun_sh > 0.02f && dist_sq < 65.0f * 65.0f && !is_alt_water_at(wx, wy)) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glColor4f(0.32f * lit_.ground_mul[0], 0.27f * lit_.ground_mul[1], 0.22f * lit_.ground_mul[2], 0.13f * sun_sh);
                draw_cloud_poly(cx + 0.60f, 0.019f, cz + 0.45f, rx * 0.92f, rz * 0.92f, yaw, gr, gc);
                glDisable(GL_BLEND);
            }

            // 2. Crisp retro flat multi-sided cloud polygon (live VGA cloud color idx 176: cream-white by day, indigo at night)
            glColor3fv(lit_.cloud_rgb);
            draw_cloud_poly(cx, alt, cz, rx, rz, yaw, gr, gc);
        }
    }
}

void ModernRenderer::draw_world_entities(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_aircraft || tel.entities.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    for (const auto& ent : tel.entities) {
        // Hide player's own F-16C when sitting inside the cockpit; RENDER it in External / Chase / Flyby views!
        if (ent.is_player && !tel.is_external_cam) continue;

        float ex = static_cast<float>((ent.world_y - tel.cam_y) * kHalfFtToKm);
        float ey = static_cast<float>(ent.world_z * kHalfFtToKm);
        float ez = static_cast<float>(-(ent.world_x - tel.cam_x) * kHalfFtToKm);

        // In External Chase ('2') view, Falcon updates player slot 0 (0x43AB4) slightly asynchronously from
        // camera (0x45C10), causing 1-tick back-and-forth jitter; filter player relative offset smoothly!
        if (ent.is_player && tel.is_external_cam) {
            float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);
            float ey_rel = ey - cam_y_km;
            float dx = ex - smooth_chase_ex_;
            float dy = ey_rel - smooth_chase_ey_;
            float dz = ez - smooth_chase_ez_;
            float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!has_smooth_chase_player_ || jump > 0.25f) {
                smooth_chase_ex_ = ex;
                smooth_chase_ey_ = ey_rel;
                smooth_chase_ez_ = ez;
                has_smooth_chase_player_ = true;
            } else {
                float a = 1.0f - std::exp(-10.0f * std::clamp(last_dt_sec_, 0.001f, 0.05f));
                smooth_chase_ex_ += dx * a;
                smooth_chase_ey_ += dy * a;
                smooth_chase_ez_ += dz * a;
            }
            ex = smooth_chase_ex_;
            ey = cam_y_km + smooth_chase_ey_;
            ez = smooth_chase_ez_;
        }

        float dist_sq = ex * ex + ez * ez;
        if (dist_sq > 85.0f * 85.0f) continue;
        float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);
        float dy = ey - cam_y_km;
        float dist_km = std::sqrt(dist_sq);
        float slant_km = std::sqrt(dist_sq + dy * dy);

        // Player's own jet in External/Chase view is always strictly 1:1; all other aircraft use Smart Scaling!
        float vis_scale = ent.is_player ? 1.0f : compute_smart_scale(slant_km, false);

        // Project crisp retro ground shadow directly below nearby low-flying aircraft (< 12,000 ft)
        float sun_sh = std::clamp((lit_.daylight - 0.28f) / 0.50f, 0.0f, 1.0f);
        if (vr_cfg_.ground_shadows && sun_sh > 0.02f && dist_km < 18.0f && ey < 3.65f) {
            float sh_alpha = std::clamp(0.42f * (1.0f - ey / 3.65f), 0.05f, 0.42f) * sun_sh;
            float sh_r = 0.012f * vis_scale * (1.0f + ey * 0.35f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glPushMatrix();
            glTranslatef(ex + ey * 0.12f, 0.022f, ez + ey * 0.10f);
            glRotatef(-ent.yaw_deg, 0.0f, 1.0f, 0.0f);
            glColor4f(0.18f * lit_.daylight, 0.15f * lit_.daylight, 0.12f * lit_.daylight, sh_alpha);
            glBegin(GL_TRIANGLES);
            glVertex3f( 0.0f,  0.0f, -sh_r * 1.4f);
            glVertex3f(-sh_r,  0.0f,  sh_r * 0.9f);
            glVertex3f( sh_r,  0.0f,  sh_r * 0.9f);
            glEnd();
            glPopMatrix();
            glDisable(GL_BLEND);
        }

        glPushMatrix();
        glTranslatef(ex, ey, ez);
        glRotatef(-ent.yaw_deg, 0.0f, 1.0f, 0.0f);
        glRotatef(ent.pitch_deg, 1.0f, 0.0f, 0.0f);
        glRotatef(-ent.roll_deg, 0.0f, 0.0f, 1.0f);
        glScalef(vis_scale, vis_scale, vis_scale);

        int mid = ent.is_player ? 3 : resolve_model_index(ent.type_id);
        if (mid >= 0 && mid < static_cast<int>(models_.size()) && !models_[mid].faces.empty()) {
            const ObjModel& mod = models_[mid];
            for (const auto& face : mod.faces) {
                if (face.verts.size() < 3) continue;
                float shade = 0.62f + 0.38f * std::abs(face.normal.x * 0.35f + face.normal.y * 0.85f + face.normal.z * 0.38f);
                float r = 0.46f, g = 0.49f, b = 0.53f;
                if (has_vga_pal_ && face.color_idx != 0) {
                    r = vga_rgb_[face.color_idx * 3 + 0];
                    g = vga_rgb_[face.color_idx * 3 + 1];
                    b = vga_rgb_[face.color_idx * 3 + 2];
                    if (r + g + b < 0.15f) {
                        r = 0.30f * lit_.daylight;
                        g = 0.33f * lit_.daylight;
                        b = 0.37f * lit_.daylight;
                    }
                    if (vr_cfg_.lighting_mode != LightingMode::AutoFromFalcon) {
                        float ac_mul = 0.22f + 0.78f * lit_.daylight;
                        r *= ac_mul; g *= ac_mul; b *= std::min(1.0f, ac_mul * 1.15f);
                    }
                } else {
                    float ac_mul = 0.22f + 0.78f * lit_.daylight;
                    r *= ac_mul; g *= ac_mul; b *= ac_mul;
                }
                glColor3f(std::min(1.0f, r * shade), std::min(1.0f, g * shade), std::min(1.0f, b * shade));
                glBegin(GL_POLYGON);
                for (uint16_t vi : face.verts) {
                    const ObjVec3& v = mod.verts[vi];
                    glVertex3f(v.x, v.y, v.z);
                }
                glEnd();
            }
        } else {
            float ac_mul = 0.22f + 0.78f * lit_.daylight;
            glColor3f(0.32f * ac_mul, 0.35f * ac_mul, 0.38f * ac_mul);
            glBegin(GL_TRIANGLES);
            glVertex3f( 0.0f,  0.0f, -0.009f);
            glVertex3f(-0.006f, 0.0f,  0.005f);
            glVertex3f( 0.006f, 0.0f,  0.005f);
            glColor3f(0.22f * ac_mul, 0.25f * ac_mul, 0.28f * ac_mul);
            glVertex3f( 0.0f,  0.0f,  0.001f);
            glVertex3f( 0.0f,  0.004f, 0.006f);
            glVertex3f( 0.0f,  0.0f,  0.006f);
            glEnd();
        }
        glPopMatrix();
    }
}

void ModernRenderer::draw_ground_units(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_ground_units || tel.ground_units.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);

    for (const auto& g : tel.ground_units) {
        float gx = static_cast<float>((g.world_y - tel.cam_y) * kHalfFtToKm);
        float gz = static_cast<float>(-(g.world_x - tel.cam_x) * kHalfFtToKm);
        float dist_sq = gx * gx + gz * gz;
        if (dist_sq > 45.0f * 45.0f) continue;

        float gy = 0.024f + static_cast<float>(g.world_z * kHalfFtToKm);
        float dy = gy - cam_y_km;
        float slant_km = std::sqrt(dist_sq + dy * dy);

        // Apply Smart Scaling (ground mode) + gentle long-range visibility floor so armored columns &
        // SAM batteries remain clearly readable at 8..25 km just like Classic VGA's brown ground dots!
        float vis_scale = compute_smart_scale(slant_km, true);
        if (vr_cfg_.smart_scale_ground && vr_cfg_.smart_scaling_mode != SmartScalingMode::Off && slant_km > 3.5f) {
            float min_far_scale = std::min(4.8f, 1.0f + (slant_km - 3.5f) * 0.22f);
            vis_scale = std::max(vis_scale, min_far_scale);
        }

        // Subtle dark ground contact shadow under nearby vehicles
        float sun_sh = std::clamp((lit_.daylight - 0.28f) / 0.50f, 0.0f, 1.0f);
        if (vr_cfg_.ground_shadows && sun_sh > 0.02f && slant_km < 16.0f) {
            float sr = 0.008f * vis_scale;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.16f * lit_.daylight, 0.13f * lit_.daylight, 0.10f * lit_.daylight, 0.38f * sun_sh);
            glBegin(GL_QUADS);
            glVertex3f(gx - sr, gy - 0.001f, gz - sr);
            glVertex3f(gx + sr, gy - 0.001f, gz - sr);
            glVertex3f(gx + sr, gy - 0.001f, gz + sr);
            glVertex3f(gx - sr, gy - 0.001f, gz + sr);
            glEnd();
            glDisable(GL_BLEND);
        }

        glPushMatrix();
        glTranslatef(gx, gy, gz);
        glRotatef(-g.yaw_deg, 0.0f, 1.0f, 0.0f);
        glScalef(vis_scale, vis_scale, vis_scale);

        int mid = resolve_ground_model_index(g.type_id);
        if (mid >= 0 && mid < static_cast<int>(models_.size()) && !models_[mid].faces.empty()) {
            const ObjModel& mod = models_[mid];
            for (const auto& face : mod.faces) {
                if (face.verts.size() < 3) continue;
                float shade = 0.58f + 0.42f * std::abs(face.normal.x * 0.35f + face.normal.y * 0.85f + face.normal.z * 0.38f);
                // Authentic olive-brown armored desert vehicle tones (distinct against sand & grey city!)
                float r = 0.44f, gg = 0.33f, b = 0.22f;
                if (has_vga_pal_ && face.color_idx != 0) {
                    float pr = vga_rgb_[face.color_idx * 3 + 0];
                    float pg = vga_rgb_[face.color_idx * 3 + 1];
                    float pb = vga_rgb_[face.color_idx * 3 + 2];
                    if (pr + pg + pb >= 0.18f) {
                        r = pr; gg = pg; b = pb;
                    }
                }
                float dm = 0.22f + 0.78f * lit_.daylight;
                glColor3f(std::min(1.0f, r * shade * dm),
                          std::min(1.0f, gg * shade * dm),
                          std::min(1.0f, b * shade * dm));
                glBegin(GL_POLYGON);
                for (uint16_t vi : face.verts) {
                    const ObjVec3& v = mod.verts[vi];
                    glVertex3f(v.x, v.y, v.z);
                }
                glEnd();
            }
        } else {
            // Crisp retro flat-shaded armored vehicle / SAM launcher fallback box
            float dm = 0.22f + 0.78f * lit_.daylight;
            constexpr float hw = 0.0055f, hh = 0.0045f, hl = 0.0090f;
            glBegin(GL_QUADS);
            glColor3f(0.46f * dm, 0.34f * dm, 0.22f * dm); // Top hull
            glVertex3f(-hw, hh, -hl); glVertex3f(hw, hh, -hl); glVertex3f(hw, hh, hl); glVertex3f(-hw, hh, hl);
            glColor3f(0.36f * dm, 0.26f * dm, 0.17f * dm); // Sides
            glVertex3f(-hw, 0, hl); glVertex3f(hw, 0, hl); glVertex3f(hw, hh, hl); glVertex3f(-hw, hh, hl);
            glVertex3f(-hw, 0, -hl); glVertex3f(hw, 0, -hl); glVertex3f(hw, hh, -hl); glVertex3f(-hw, hh, -hl);
            glVertex3f(hw, 0, -hl); glVertex3f(hw, 0, hl); glVertex3f(hw, hh, hl); glVertex3f(hw, hh, -hl);
            glVertex3f(-hw, 0, -hl); glVertex3f(-hw, 0, hl); glVertex3f(-hw, hh, hl); glVertex3f(-hw, hh, -hl);
            glEnd();
        }
        glPopMatrix();
    }
}

void ModernRenderer::update_missile_trails(const FlightTelemetry& tel, float dt_sec) {
    if (!tel.in_flight) {
        missile_trails_.clear();
        return;
    }
    float dt = std::clamp(dt_sec, 0.001f, 0.05f);
    float trail_life = std::clamp(vr_cfg_.missile_trail_lifetime, 2.0f, 25.0f);

    // Age existing smoke trail points and prune expired trails
    for (auto& tr : missile_trails_) {
        tr.active_this_frame = false;
        tr.sample_accum += dt;
        for (auto& pt : tr.points) pt.age += dt;
        while (!tr.points.empty() && tr.points.front().age >= tr.points.front().max_age) {
            tr.points.erase(tr.points.begin());
        }
    }

    // Append live positions for every active in-flight SAM & AAM missile
    for (const auto& m : tel.missiles) {
        ActiveMissileTrail* found = nullptr;
        for (auto& tr : missile_trails_) {
            if (tr.slot_id == m.slot_id && tr.slot_id != 0xFFFFu) {
                double dx = m.world_x - tr.last_wx;
                double dy = m.world_y - tr.last_wy;
                double dz = m.world_z - tr.last_wz;
                // If slot was recycled for a brand-new missile far away, detach the old trail so it fades cleanly!
                if (dx * dx + dy * dy + dz * dz > 12000.0 * 12000.0) {
                    tr.slot_id = 0xFFFFu; // Orphaned fading trail
                } else {
                    found = &tr;
                    break;
                }
            }
        }
        if (!found) {
            ActiveMissileTrail nt;
            nt.slot_id = m.slot_id;
            nt.last_wx = m.world_x;
            nt.last_wy = m.world_y;
            nt.last_wz = m.world_z;
            nt.sample_accum = 1.0f;
            nt.is_sam = m.is_sam;
            nt.points.reserve(128);
            missile_trails_.push_back(std::move(nt));
            found = &missile_trails_.back();
        }
        found->active_this_frame = true;
        found->is_sam = found->is_sam || m.is_sam;

        double dx = m.world_x - found->last_wx;
        double dy = m.world_y - found->last_wy;
        double dz = m.world_z - found->last_wz;
        double moved_sq = dx * dx + dy * dy + dz * dz;
        if (found->points.empty() || moved_sq > 55.0 * 55.0 || found->sample_accum >= 0.055f) {
            TrailPoint pt;
            pt.wx = m.world_x;
            pt.wy = m.world_y;
            pt.wz = m.world_z;
            pt.age = 0.0f;
            pt.max_age = found->is_sam ? (trail_life * 1.25f) : trail_life;
            pt.is_sam = found->is_sam;
            if (found->points.size() >= 220) found->points.erase(found->points.begin());
            found->points.push_back(pt);
            found->last_wx = m.world_x;
            found->last_wy = m.world_y;
            found->last_wz = m.world_z;
            found->sample_accum = 0.0f;
        }
    }

    // Release slots that disappeared so their trails keep lingering & fading naturally,
    // and spawn a crisp retro 3D airburst / impact explosion at the detonation point!
    for (size_t i = 0; i < missile_trails_.size(); ) {
        if (!missile_trails_[i].active_this_frame) {
            if (!missile_trails_[i].exploded && missile_trails_[i].points.size() >= 3) {
                missile_trails_[i].exploded = true;
                bool ground_hit = (missile_trails_[i].last_wz < 160.0);
                spawn_explosion(missile_trails_[i].last_wx, missile_trails_[i].last_wy,
                                missile_trails_[i].last_wz,
                                missile_trails_[i].is_sam ? 0.055f : 0.042f, ground_hit);
            }
            missile_trails_[i].slot_id = 0xFFFFu;
        }
        if (missile_trails_[i].points.empty() && !missile_trails_[i].active_this_frame) {
            missile_trails_.erase(missile_trails_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ModernRenderer::spawn_explosion(double wx, double wy, double wz, float radius_km, bool is_ground_kill) {
    if (!vr_cfg_.show_explosions) return;
    if (explosions_.size() >= 48) {
        explosions_.erase(explosions_.begin());
    }
    fx_rng_ = fx_rng_ * 1664525u + 1013904223u;
    ExplosionFx e;
    e.wx = wx;
    e.wy = wy;
    e.wz = std::max(40.0, wz);
    e.age = 0.0f;
    e.max_age = is_ground_kill ? 6.5f : 4.6f;
    e.base_radius_km = radius_km;
    e.is_ground_kill = is_ground_kill;
    e.seed = fx_rng_;
    explosions_.push_back(e);
}

void ModernRenderer::update_explosions_and_contrails(const FlightTelemetry& tel, float dt_sec) {
    if (!tel.in_flight) {
        explosions_.clear();
        prev_combat_units_.clear();
        contrails_.clear();
        combat_track_frames_ = 0;
        return;
    }
    float dt = std::clamp(dt_sec, 0.001f, 0.05f);
    combat_track_frames_++;

    // 1. Age active 3D explosions
    for (size_t i = 0; i < explosions_.size(); ) {
        explosions_[i].age += dt;
        if (explosions_[i].age >= explosions_[i].max_age) {
            explosions_.erase(explosions_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    // 2. Detect destroyed AI aircraft & ground units (when a tracked nearby unit vanishes from RAM mid-flight!)
    std::vector<TrackedUnit> cur_units;
    cur_units.reserve(tel.entities.size() + tel.ground_units.size());
    std::unordered_set<uint32_t> cur_keys;
    cur_keys.reserve(tel.entities.size() + tel.ground_units.size() + 8);
    for (const auto& ent : tel.entities) {
        if (ent.is_player) continue;
        uint32_t k = 0x10000000u | (static_cast<uint32_t>(ent.flight_id) << 12) | (ent.type_id & 0x0FFFu);
        cur_units.push_back({k, ent.world_x, ent.world_y, ent.world_z, false});
        cur_keys.insert(k);
    }
    for (size_t i = 0; i < tel.ground_units.size(); ++i) {
        const auto& g = tel.ground_units[i];
        uint32_t k = 0x20000000u | (static_cast<uint32_t>(g.flight_id) << 12) | (g.type_id & 0x0FFFu);
        cur_units.push_back({k, g.world_x, g.world_y, g.world_z, true});
        cur_keys.insert(k);
    }
    if (combat_track_frames_ > 35 && vr_cfg_.show_explosions) {
        constexpr double kMaxKillDistHf = 35.0 * 12152.23; // 35 NM
        for (const auto& pu : prev_combat_units_) {
            if (cur_keys.find(pu.key) == cur_keys.end()) {
                double dn = pu.wx - tel.world_x;
                double de = pu.wy - tel.world_y;
                if (dn * dn + de * de < kMaxKillDistHf * kMaxKillDistHf) {
                    spawn_explosion(pu.wx, pu.wy, pu.wz, pu.is_ground ? 0.085f : 0.075f, pu.is_ground);
                }
            }
        }
    }
    prev_combat_units_ = std::move(cur_units);

    // 3. Update high-altitude twin wingtip contrails (>= 20,000 ft MSL = 40,000 half-ft)
    constexpr float kContrailLife = 12.0f;
    for (auto& c : contrails_) {
        c.active_this_frame = false;
        c.sample_accum += dt;
        for (auto& p : c.left_pts) p.age += dt;
        for (auto& p : c.right_pts) p.age += dt;
        while (!c.left_pts.empty() && c.left_pts.front().age >= kContrailLife) c.left_pts.erase(c.left_pts.begin());
        while (!c.right_pts.empty() && c.right_pts.front().age >= kContrailLife) c.right_pts.erase(c.right_pts.begin());
    }

    if (vr_cfg_.show_contrails) {
        for (const auto& ent : tel.entities) {
            if (ent.world_z < 40000.0) continue; // Below 20,000 ft MSL
            uint32_t key = ent.is_player ? 0u : (0x40000000u | (static_cast<uint32_t>(ent.flight_id) << 12) | (ent.type_id & 0x0FFFu));
            AircraftContrail* found = nullptr;
            for (auto& c : contrails_) {
                if (c.key == key) { found = &c; break; }
            }
            if (!found) {
                AircraftContrail nc;
                nc.key = key;
                nc.last_wx = ent.world_x; nc.last_wy = ent.world_y; nc.last_wz = ent.world_z;
                nc.sample_accum = 1.0f;
                contrails_.push_back(std::move(nc));
                found = &contrails_.back();
            }
            found->active_this_frame = true;
            double dx = ent.world_x - found->last_wx;
            double dy = ent.world_y - found->last_wy;
            double dz = ent.world_z - found->last_wz;
            if (dx * dx + dy * dy + dz * dz > 16000.0 * 16000.0) {
                found->left_pts.clear();
                found->right_pts.clear();
            }
            if (found->left_pts.empty() || dx * dx + dy * dy + dz * dz > 110.0 * 110.0 || found->sample_accum >= 0.08f) {
                // Compute aircraft body-right unit vector in world (North=wx, East=wy, Up=wz)
                float yr = ent.yaw_deg * kDeg2Rad;
                float rr = ent.roll_deg * kDeg2Rad;
                float cr = std::cos(rr), sr = std::sin(rr);
                // Wingtip half-span ~ 15 ft = 30 half-ft
                double rn = -std::sin(yr) * cr * 30.0;
                double re =  std::cos(yr) * cr * 30.0;
                double rz = -sr * 30.0;
                if (found->left_pts.size() >= 180) found->left_pts.erase(found->left_pts.begin());
                if (found->right_pts.size() >= 180) found->right_pts.erase(found->right_pts.begin());
                found->left_pts.push_back({ent.world_x - rn, ent.world_y - re, ent.world_z - rz, 0.0f});
                found->right_pts.push_back({ent.world_x + rn, ent.world_y + re, ent.world_z + rz, 0.0f});
                found->last_wx = ent.world_x; found->last_wy = ent.world_y; found->last_wz = ent.world_z;
                found->sample_accum = 0.0f;
            }
        }
    }

    for (size_t i = 0; i < contrails_.size(); ) {
        if (!contrails_[i].active_this_frame && contrails_[i].left_pts.empty() && contrails_[i].right_pts.empty()) {
            contrails_.erase(contrails_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ModernRenderer::draw_contrails(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_contrails || contrails_.empty()) return;

    float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);
    float yaw_r = tel.cam_yaw_deg * kDeg2Rad;
    float pitch_r = tel.cam_pitch_deg * kDeg2Rad;
    float right_x = std::cos(yaw_r);
    float right_z = std::sin(yaw_r);
    float up_x = -std::sin(pitch_r) * std::sin(yaw_r);
    float up_y =  std::cos(pitch_r);
    float up_z =  std::sin(pitch_r) * std::cos(yaw_r);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    float cr = 0.88f * (0.28f + 0.72f * lit_.daylight);
    float cg = 0.90f * (0.28f + 0.72f * lit_.daylight);
    float cb = 0.94f * (0.34f + 0.66f * lit_.daylight);
    constexpr float kContrailLife = 12.0f;

    auto draw_ribbon = [&](const std::vector<ContrailPoint>& pts) {
        if (pts.size() < 2) return;
        glBegin(GL_QUAD_STRIP);
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto& pt = pts[i];
            float px = static_cast<float>((pt.wy - tel.cam_y) * kHalfFtToKm);
            float py = static_cast<float>(pt.wz * kHalfFtToKm);
            float pz = static_cast<float>(-(pt.wx - tel.cam_x) * kHalfFtToKm);
            float life = std::clamp(1.0f - pt.age / kContrailLife, 0.0f, 1.0f);
            float dist_km = std::sqrt(px * px + (py - cam_y_km) * (py - cam_y_km) + pz * pz);
            float hw = std::clamp(0.003f + (1.0f - life) * 0.012f + dist_km * 0.0007f, 0.003f, 0.045f);
            float sx = (right_x * 0.7f + up_x * 0.7f) * hw;
            float sy = (up_y * 0.7f) * hw;
            float sz = (right_z * 0.7f + up_z * 0.7f) * hw;
            float alpha = life * life * 0.42f;
            glColor4f(cr, cg, cb, alpha);
            glVertex3f(px - sx, py - sy, pz - sz);
            glVertex3f(px + sx, py + sy, pz + sz);
        }
        glEnd();
    };

    for (const auto& c : contrails_) {
        draw_ribbon(c.left_pts);
        draw_ribbon(c.right_pts);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void ModernRenderer::draw_explosions(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_explosions || explosions_.empty()) return;

    float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);

    auto draw_octa = [](float cx, float cy, float cz, float rx, float ry, float rz) {
        // Crisp 8-face retro low-poly octahedron (matches Falcon 3.0's classic faceted explosions!)
        glBegin(GL_TRIANGLES);
        // Top 4 faces
        glVertex3f(cx, cy + ry, cz); glVertex3f(cx + rx, cy, cz); glVertex3f(cx, cy, cz + rz);
        glVertex3f(cx, cy + ry, cz); glVertex3f(cx, cy, cz + rz); glVertex3f(cx - rx, cy, cz);
        glVertex3f(cx, cy + ry, cz); glVertex3f(cx - rx, cy, cz); glVertex3f(cx, cy, cz - rz);
        glVertex3f(cx, cy + ry, cz); glVertex3f(cx, cy, cz - rz); glVertex3f(cx + rx, cy, cz);
        // Bottom 4 faces
        glVertex3f(cx, cy - ry, cz); glVertex3f(cx, cy, cz + rz); glVertex3f(cx + rx, cy, cz);
        glVertex3f(cx, cy - ry, cz); glVertex3f(cx - rx, cy, cz); glVertex3f(cx, cy, cz + rz);
        glVertex3f(cx, cy - ry, cz); glVertex3f(cx, cy, cz - rz); glVertex3f(cx - rx, cy, cz);
        glVertex3f(cx, cy - ry, cz); glVertex3f(cx + rx, cy, cz); glVertex3f(cx, cy, cz - rz);
        glEnd();
    };

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    for (const auto& e : explosions_) {
        float ex = static_cast<float>((e.wy - tel.cam_y) * kHalfFtToKm);
        float ey = static_cast<float>(e.wz * kHalfFtToKm);
        float ez = static_cast<float>(-(e.wx - tel.cam_x) * kHalfFtToKm);
        float dist_km = std::sqrt(ex * ex + (ey - cam_y_km) * (ey - cam_y_km) + ez * ez);
        if (dist_km > 70.0f) continue;

        float dist_boost = std::clamp(1.0f + dist_km * 0.055f, 1.0f, 4.2f);
        float r0 = e.base_radius_km * dist_boost;

        // 1. Expanding Dark Smoke Plume (rises slowly and lingers 0.2s .. max_age)
        if (e.age > 0.18f) {
            float st = std::clamp((e.age - 0.18f) / std::max(0.5f, e.max_age - 0.18f), 0.0f, 1.0f);
            float sm_a = (1.0f - st) * (1.0f - st) * 0.68f;
            float sm_r = r0 * (0.65f + st * 2.15f);
            float rise = e.age * (e.is_ground_kill ? 0.032f : 0.018f) * dist_boost;
            float dm = 0.22f + 0.78f * lit_.daylight;
            for (int k = 0; k < 3; ++k) {
                float ang = static_cast<float>(k) * 2.094f + static_cast<float>(e.seed & 0xFFu) * 0.025f;
                float ox = std::cos(ang) * sm_r * 0.36f;
                float oz = std::sin(ang) * sm_r * 0.36f;
                float oy = rise * (0.65f + 0.28f * static_cast<float>(k));
                float shade = (0.16f + 0.04f * static_cast<float>(k)) * dm;
                glColor4f(shade, shade * 0.96f, shade * 0.92f, sm_a);
                draw_octa(ex + ox, ey + oy, ez + oz, sm_r * 0.72f, sm_r * 0.68f, sm_r * 0.72f);
            }
        }

        // 2. Layered Retro Fireball (0.0 .. 1.65s: bright yellow -> fiery orange -> deep ember red)
        if (e.age < 1.65f) {
            float ft = e.age / 1.65f;
            float fb_r = r0 * (0.45f + std::sqrt(ft) * 1.45f);
            float fb_a = (1.0f - ft) * (1.0f - ft * 0.4f) * 0.92f;
            float fr = 1.0f;
            float fg = std::clamp(0.88f - ft * 0.78f, 0.12f, 0.88f);
            float fb = std::clamp(0.32f - ft * 0.30f, 0.03f, 0.32f);
            glColor4f(fr, fg, fb, fb_a);
            draw_octa(ex, ey + e.age * 0.015f, ez, fb_r, fb_r * 0.92f, fb_r);

            // Secondary offset fireball lobe
            float ox = std::cos(static_cast<float>(e.seed & 0xFFu)) * fb_r * 0.42f;
            float oz = std::sin(static_cast<float>(e.seed & 0xFFu)) * fb_r * 0.42f;
            glColor4f(1.0f, std::max(0.08f, fg * 0.72f), 0.04f, fb_a * 0.78f);
            draw_octa(ex + ox, ey + fb_r * 0.28f, ez + oz, fb_r * 0.68f, fb_r * 0.68f, fb_r * 0.68f);
        }

        // 3. Incandescent Initial Detonation Flash Core (0.0 .. 0.32s)
        if (e.age < 0.32f) {
            float ct = 1.0f - (e.age / 0.32f);
            float cr = r0 * (0.55f + (1.0f - ct) * 0.55f);
            glColor4f(1.0f, 0.98f, 0.84f, ct * 0.96f);
            draw_octa(ex, ey, ez, cr * 0.65f, cr * 0.65f, cr * 0.65f);
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void ModernRenderer::draw_missiles_and_trails(const FlightTelemetry& tel) {
    if (!vr_cfg_.show_missiles_trails) return;
    if (tel.missiles.empty() && missile_trails_.empty()) return;

    float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);
    // Camera right & up vectors in world space for crisp long-range rocket flares & smoke billboards
    float yaw_r = tel.cam_yaw_deg * kDeg2Rad;
    float pitch_r = tel.cam_pitch_deg * kDeg2Rad;
    float right_x = std::cos(yaw_r);
    float right_z = std::sin(yaw_r);
    float up_x = -std::sin(pitch_r) * std::sin(yaw_r);
    float up_y =  std::cos(pitch_r);
    float up_z =  std::sin(pitch_r) * std::cos(yaw_r);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_FOG); // Keep SAM smoke trails & rocket ignition plumes clearly visible through horizon haze out to 25+ NM!
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    // Near/far blend helper: 0 at/below e0, 1 at/above e1 (smoothstep)
    auto near_far = [](float d, float e0, float e1) {
        float t = std::clamp((d - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    // 1. Long-range high-visibility 3D SAM & AAM smoke trail ribbons!
    // Far away (8+ km) the ribbon is drawn exactly as before: wide, solid, readable at 15-25 NM.
    // Up close it shrinks to a physical rocket-smoke width (a few metres, expanding as it ages) and gets
    // feathered edges (opaque centre line, transparent edges) so it never reads as a flat white slab.
    struct TrailVert {
        float x, y, z, sx, sy, sz, hw, r, g, b, a, edge_a;
    };
    std::vector<TrailVert> tv;
    for (const auto& tr : missile_trails_) {
        if (tr.points.size() < 2) continue;
        // Quick distance cull on newest point before starting quad strip
        {
            const auto& newest = tr.points.back();
            float nx = static_cast<float>((newest.wy - tel.cam_y) * kHalfFtToKm);
            float ny = static_cast<float>(newest.wz * kHalfFtToKm) - cam_y_km;
            float nz = static_cast<float>(-(newest.wx - tel.cam_x) * kHalfFtToKm);
            if (nx * nx + ny * ny + nz * nz > 60.0f * 60.0f) continue;
        }
        tv.clear();
        tv.reserve(tr.points.size());
        for (size_t i = 0; i < tr.points.size(); ++i) {
            const auto& pt = tr.points[i];
            float x = static_cast<float>((pt.wy - tel.cam_y) * kHalfFtToKm);
            float y = static_cast<float>(pt.wz * kHalfFtToKm);
            float z = static_cast<float>(-(pt.wx - tel.cam_x) * kHalfFtToKm);

            // Compute local tangent along trail
            const auto& p_prev = tr.points[i > 0 ? i - 1 : i];
            const auto& p_next = tr.points[i + 1 < tr.points.size() ? i + 1 : i];
            float tx = static_cast<float>((p_next.wy - p_prev.wy) * kHalfFtToKm);
            float ty = static_cast<float>((p_next.wz - p_prev.wz) * kHalfFtToKm);
            float tz = static_cast<float>(-(p_next.wx - p_prev.wx) * kHalfFtToKm);

            // Camera-to-point vector (camera is at (0, cam_y_km, 0) in camera-relative world coords)
            float vx = x, vy = y - cam_y_km, vz = z;
            float dist_km = std::sqrt(vx * vx + vy * vy + vz * vz);

            // Billboard perpendicular = normalize(cross(tangent, to_cam))
            float sx = ty * vz - tz * vy;
            float sy = tz * vx - tx * vz;
            float sz = tx * vy - ty * vx;
            float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (sl > 1e-6f) {
                sx /= sl; sy /= sl; sz /= sl;
            } else {
                sx = right_x; sy = 0.0f; sz = right_z;
            }

            float life = std::clamp(pt.age / std::max(0.1f, pt.max_age), 0.0f, 1.0f);
            // Far: original exaggerated plume width. Near: physical rocket smoke (~1.5 m fresh -> ~10 m aged).
            float far_w  = pt.is_sam ? (0.009f + 0.036f * life) : (0.005f + 0.022f * life);
            float near_w = pt.is_sam ? (0.0015f + 0.0085f * life) : (0.0009f + 0.0055f * life);
            float far_t = near_far(dist_km, 1.5f, 8.0f);
            float base_w = near_w + (far_w - near_w) * far_t;
            // Gentle angular minimum width so SAM smoke trails stay visible at 15-25 NM
            float angular_min = dist_km * (pt.is_sam ? 0.0024f : 0.0017f);
            float hw = std::max(base_w, std::min(angular_min, 0.14f));

            // Bright cream-white fresh SAM rocket smoke fading gently into warm atmospheric grey
            float fade = (1.0f - life) * (1.0f - 0.35f * life);
            float dist_fade = std::clamp(1.0f - (dist_km - 32.0f) / 24.0f, 0.18f, 1.0f);
            float alpha = std::clamp(fade * dist_fade * (pt.is_sam ? 0.90f : 0.78f), 0.0f, 0.92f);
            // Close-up smoke is a little translucent instead of an opaque white wall
            alpha *= 0.70f + 0.30f * far_t;
            float smk_lit = 0.40f + 0.60f * lit_.daylight;
            float cr = (0.96f - 0.12f * life) * smk_lit + 0.12f * (1.0f - life);
            float cg = (0.94f - 0.14f * life) * smk_lit + 0.10f * (1.0f - life);
            float cb = (0.90f - 0.12f * life) * smk_lit + 0.08f * (1.0f - life);

            // Feathered edges up close, solid edges far away (identical to the old long-range look)
            float edge_hard = near_far(dist_km, 1.0f, 6.0f);
            tv.push_back({x, y, z, sx, sy, sz, hw,
                          std::min(1.0f, cr), std::min(1.0f, cg), std::min(1.0f, cb),
                          alpha, alpha * edge_hard});
        }
        // Two half-ribbons: left edge -> centre line, then centre line -> right edge
        for (int side = -1; side <= 1; side += 2) {
            const float fs = static_cast<float>(side);
            glBegin(GL_QUAD_STRIP);
            for (const auto& v : tv) {
                float ex = v.x + fs * v.sx * v.hw;
                float ey = v.y + fs * v.sy * v.hw;
                float ez = v.z + fs * v.sz * v.hw;
                if (side < 0) {
                    glColor4f(v.r, v.g, v.b, v.edge_a);
                    glVertex3f(ex, ey, ez);
                    glColor4f(v.r, v.g, v.b, v.a);
                    glVertex3f(v.x, v.y, v.z);
                } else {
                    glColor4f(v.r, v.g, v.b, v.a);
                    glVertex3f(v.x, v.y, v.z);
                    glColor4f(v.r, v.g, v.b, v.edge_a);
                    glVertex3f(ex, ey, ez);
                }
            }
            glEnd();
        }
    }

    glDepthMask(GL_TRUE);

    // 2. Active missile 3D bodies + blazing long-range rocket motor flares (visible from 25+ NM!)
    for (const auto& m : tel.missiles) {
        float x = static_cast<float>((m.world_y - tel.cam_y) * kHalfFtToKm);
        float y = static_cast<float>(m.world_z * kHalfFtToKm);
        float z = static_cast<float>(-(m.world_x - tel.cam_x) * kHalfFtToKm);
        float dy = y - cam_y_km;
        float dist_km = std::sqrt(x * x + dy * dy + z * z);
        if (dist_km > 55.0f) continue;

        // 2a. Crisp 3D missile dart body (scaled for readability)
        float body_scale = std::max(1.0f, std::min(dist_km * 1.4f, 18.0f));
        glPushMatrix();
        glTranslatef(x, y, z);
        glRotatef(-m.yaw_deg, 0.0f, 1.0f, 0.0f);
        glRotatef(m.pitch_deg, 1.0f, 0.0f, 0.0f);
        glScalef(body_scale, body_scale, body_scale);
        float dm = 0.35f + 0.65f * lit_.daylight;
        glColor4f(0.92f * dm, 0.92f * dm, 0.90f * dm, 1.0f);
        glBegin(GL_TRIANGLES);
        // Fuselage dart (length ~4m = 0.004 km)
        glVertex3f( 0.0000f,  0.0000f, -0.0030f);
        glVertex3f(-0.0005f,  0.0000f,  0.0022f);
        glVertex3f( 0.0005f,  0.0000f,  0.0022f);
        glVertex3f( 0.0000f,  0.0000f, -0.0030f);
        glVertex3f( 0.0000f, -0.0005f,  0.0022f);
        glVertex3f( 0.0000f,  0.0005f,  0.0022f);
        glEnd();
        glPopMatrix();

        // 2b. Blazing incandescent SAM/AAM rocket motor glow + white core (camera-facing billboard!)
        // Angular size scales cleanly with distance so a distant SAM launch flashes like a real booster flare!
        // Far (6+ km): the original solid square flare. Near: a small soft radial glow (a few metres) fading
        // to transparent at its rim, so a close missile is a hot point of light rather than an orange block.
        float glow_far_t = near_far(dist_km, 1.0f, 3.5f);
        float min_r = m.is_sam ? (0.004f + 0.014f * glow_far_t) : (0.0025f + 0.0085f * glow_far_t);
        float outer_r = std::max(min_r, dist_km * (m.is_sam ? 0.0052f : 0.0036f));
        outer_r = std::min(outer_r, 0.22f);
        float core_r = outer_r * 0.45f;
        float square_w = near_far(dist_km, 2.5f, 6.0f); // weight of legacy square flare (1 = old far look)

        glDepthMask(GL_FALSE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE); // Additive glow for unmistakable rocket ignition!
        auto glow_quad = [&](float r, float cr_, float cg_, float cb_, float a) {
            glColor4f(cr_, cg_, cb_, a);
            glVertex3f(x - right_x * r - up_x * r, y - up_y * r, z - right_z * r - up_z * r);
            glVertex3f(x + right_x * r - up_x * r, y - up_y * r, z + right_z * r - up_z * r);
            glVertex3f(x + right_x * r + up_x * r, y + up_y * r, z + right_z * r + up_z * r);
            glVertex3f(x - right_x * r + up_x * r, y + up_y * r, z - right_z * r + up_z * r);
        };
        auto glow_disc = [&](float r, float cr_, float cg_, float cb_, float a) {
            constexpr int kSeg = 20;
            glBegin(GL_TRIANGLE_FAN);
            glColor4f(cr_, cg_, cb_, a);
            glVertex3f(x, y, z);
            glColor4f(cr_, cg_, cb_, 0.0f);
            for (int k = 0; k <= kSeg; ++k) {
                float ang = static_cast<float>(k) * (6.2831853f / static_cast<float>(kSeg));
                float ca = std::cos(ang) * r, sa = std::sin(ang) * r;
                glVertex3f(x + right_x * ca + up_x * sa, y + up_y * sa, z + right_z * ca + up_z * sa);
            }
            glEnd();
        };
        if (square_w > 0.001f) {
            glBegin(GL_QUADS);
            glow_quad(outer_r, 1.0f, 0.52f, 0.12f, 0.88f * square_w); // Warm orange-amber rocket bloom
            glow_quad(core_r, 1.0f, 0.98f, 0.82f, 0.98f * square_w);  // Blazing white-yellow rocket core
            glEnd();
        }
        if (square_w < 0.999f) {
            float soft_w = 1.0f - square_w;
            glow_disc(outer_r, 1.0f, 0.52f, 0.12f, 0.88f * soft_w);
            glow_disc(core_r, 1.0f, 0.98f, 0.82f, 1.0f * soft_w);
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);
    }

    glDisable(GL_BLEND);
    if (vr_cfg_.horizon_fog) glEnable(GL_FOG);
}

void ModernRenderer::spawn_chaff(const FlightTelemetry& tel) {
    if (!tel.in_flight) return;
    auto next_f = [&]() -> float {
        fx_rng_ ^= fx_rng_ << 13; fx_rng_ ^= fx_rng_ >> 17; fx_rng_ ^= fx_rng_ << 5;
        return static_cast<float>(fx_rng_ & 0xFFFFu) / 65535.0f;
    };
    double yaw = tel.yaw_deg * kDeg2Rad;
    double pitch = tel.pitch_deg * kDeg2Rad;
    double fx = std::cos(pitch) * std::cos(yaw);
    double fy = std::cos(pitch) * std::sin(yaw);
    double fz = std::sin(pitch);
    double rx = -std::sin(yaw), ry = std::cos(yaw);
    double spd_hf = std::max(120.0, static_cast<double>(tel.airspeed_kts) * 1.6878) * 2.0; // 0.5 ft/s

    // Spawn 16 fluttering metallic chaff strips behind the F-16C tail dispenser
    for (int i = 0; i < 16; ++i) {
        if (world_fx_.size() >= 256) world_fx_.erase(world_fx_.begin());
        WorldFx p;
        p.kind = WorldFx::Kind::Chaff;
        double lat = (next_f() - 0.5) * 26.0;
        double vrt = (next_f() - 0.6) * 22.0;
        p.wx = tel.world_x - fx * 30.0 + rx * lat;
        p.wy = tel.world_y - fy * 30.0 + ry * lat;
        p.wz = tel.altitude_ft * 2.0 - fz * 30.0 + vrt;
        // Chaff rapidly decelerates in air relative to the jet, spreading outward & slightly down
        p.vx = fx * spd_hf * 0.25 + rx * (next_f() - 0.5) * 160.0;
        p.vy = fy * spd_hf * 0.25 + ry * (next_f() - 0.5) * 160.0;
        p.vz = fz * spd_hf * 0.25 - 18.0 - next_f() * 30.0;
        p.age = 0.0f;
        p.max_age = 2.8f + next_f() * 1.2f;
        p.seed = fx_rng_;
        world_fx_.push_back(p);
    }
}

void ModernRenderer::spawn_flare(const FlightTelemetry& tel) {
    if (!tel.in_flight) return;
    auto next_f = [&]() -> float {
        fx_rng_ ^= fx_rng_ << 13; fx_rng_ ^= fx_rng_ >> 17; fx_rng_ ^= fx_rng_ << 5;
        return static_cast<float>(fx_rng_ & 0xFFFFu) / 65535.0f;
    };
    double yaw = tel.yaw_deg * kDeg2Rad;
    double pitch = tel.pitch_deg * kDeg2Rad;
    double fx = std::cos(pitch) * std::cos(yaw);
    double fy = std::cos(pitch) * std::sin(yaw);
    double fz = std::sin(pitch);
    double rx = -std::sin(yaw), ry = std::cos(yaw);
    double spd_hf = std::max(120.0, static_cast<double>(tel.airspeed_kts) * 1.6878) * 2.0; // 0.5 ft/s

    // Eject 2 blazing IR flares downward & slightly outward from belly ALE-40 pods!
    for (int side = -1; side <= 1; side += 2) {
        if (world_fx_.size() >= 256) world_fx_.erase(world_fx_.begin());
        WorldFx p;
        p.kind = WorldFx::Kind::Flare;
        p.wx = tel.world_x - fx * 26.0 + rx * (side * 8.0);
        p.wy = tel.world_y - fy * 26.0 + ry * (side * 8.0);
        p.wz = tel.altitude_ft * 2.0 - fz * 26.0 - 8.0;
        p.vx = fx * spd_hf * 0.72 + rx * (side * (45.0 + next_f() * 35.0));
        p.vy = fy * spd_hf * 0.72 + ry * (side * (45.0 + next_f() * 35.0));
        p.vz = fz * spd_hf * 0.72 - (65.0 + next_f() * 30.0);
        p.age = 0.0f;
        p.max_age = 3.8f + next_f() * 0.9f;
        p.seed = fx_rng_;
        world_fx_.push_back(p);
    }
}

void ModernRenderer::spawn_gun_tracer(const FlightTelemetry& tel) {
    if (!tel.in_flight) return;
    double yaw = tel.yaw_deg * kDeg2Rad;
    double pitch = tel.pitch_deg * kDeg2Rad;
    double fx = std::cos(pitch) * std::cos(yaw);
    double fy = std::cos(pitch) * std::sin(yaw);
    double fz = std::sin(pitch);
    double rx = -std::sin(yaw), ry = std::cos(yaw);
    // M61A1 muzzle velocity ~3,450 ft/s + aircraft speed (in 0.5 ft/s units)
    double spd_hf = (3450.0 + std::max(150.0, static_cast<double>(tel.airspeed_kts) * 1.6878)) * 2.0;
    if (world_fx_.size() >= 256) world_fx_.erase(world_fx_.begin());
    WorldFx p;
    p.kind = WorldFx::Kind::Tracer;
    p.wx = tel.world_x + fx * 32.0 - rx * 5.0;
    p.wy = tel.world_y + fy * 32.0 - ry * 5.0;
    p.wz = tel.altitude_ft * 2.0 + fz * 32.0 - 2.0;
    p.vx = fx * spd_hf;
    p.vy = fy * spd_hf;
    p.vz = fz * spd_hf;
    p.age = 0.0f;
    p.max_age = 1.4f;
    p.seed = ++fx_rng_;
    world_fx_.push_back(p);
}

void ModernRenderer::update_world_fx(float dt_sec, bool in_flight) {
    if (!in_flight) {
        world_fx_.clear();
        return;
    }
    float dt = std::clamp(dt_sec, 0.001f, 0.05f);
    for (size_t i = 0; i < world_fx_.size(); ) {
        WorldFx& p = world_fx_[i];
        p.age += dt;
        if (p.age >= p.max_age || p.wz <= 2.0) {
            world_fx_.erase(world_fx_.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        p.wx += p.vx * dt;
        p.wy += p.vy * dt;
        p.wz += p.vz * dt;
        if (p.kind == WorldFx::Kind::Chaff) {
            float drag = std::exp(-2.8f * dt);
            p.vx *= drag; p.vy *= drag;
            p.vz = p.vz * drag - 14.0 * dt;
        } else if (p.kind == WorldFx::Kind::Flare) {
            float drag = std::exp(-0.95f * dt);
            p.vx *= drag; p.vy *= drag;
            p.vz = p.vz * drag - 64.4 * dt; // 32.2 ft/s^2 gravity in 0.5 ft units!
        } else {
            p.vz -= 64.4 * dt; // Ballistic drop on 20mm tracers
        }
        ++i;
    }
}

void ModernRenderer::draw_world_fx(const FlightTelemetry& tel) {
    if (world_fx_.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 1. Shimmering metallic Chaff strips + blazing IR Flares (glowing white-gold core + orange halo!)
    for (const auto& p : world_fx_) {
        float x = static_cast<float>((p.wy - tel.cam_y) * kHalfFtToKm);
        float y = static_cast<float>(p.wz * kHalfFtToKm);
        float z = static_cast<float>(-(p.wx - tel.cam_x) * kHalfFtToKm);
        float life = 1.0f - std::clamp(p.age / p.max_age, 0.0f, 1.0f);

        if (p.kind == WorldFx::Kind::Chaff) {
            float twinkle = 0.55f + 0.45f * std::sin(p.age * 28.0f + static_cast<float>(p.seed & 255u));
            glPointSize(3.5f);
            glBegin(GL_POINTS);
            glColor4f(0.88f * twinkle, 0.92f * twinkle, 1.0f * twinkle, life * 0.90f);
            glVertex3f(x, y, z);
            glEnd();
        } else if (p.kind == WorldFx::Kind::Flare) {
            float flicker = 0.85f + 0.15f * std::sin(p.age * 42.0f + static_cast<float>(p.seed & 63u));
            // Outer warm amber-orange IR halo + trailing smoke puff
            glPointSize(11.0f * (0.55f + 0.45f * life));
            glBegin(GL_POINTS);
            glColor4f(1.0f, 0.52f * flicker, 0.10f, life * 0.58f);
            glVertex3f(x, y, z);
            glEnd();
            // Incandescent white-gold core
            glPointSize(5.5f * (0.65f + 0.35f * life));
            glBegin(GL_POINTS);
            glColor4f(1.0f, 0.96f, 0.80f, std::min(1.0f, life * 1.4f));
            glVertex3f(x, y, z);
            glEnd();
            // Short glowing smoke streak behind the falling flare
            float tail_scale = 0.045f * static_cast<float>(kHalfFtToKm);
            glLineWidth(2.0f);
            glBegin(GL_LINES);
            glColor4f(1.0f, 0.72f, 0.30f, life * 0.65f);
            glVertex3f(x, y, z);
            glColor4f(0.75f, 0.72f, 0.68f, 0.0f);
            glVertex3f(x - static_cast<float>(p.vy) * tail_scale,
                       y - static_cast<float>(p.vz) * tail_scale,
                       z + static_cast<float>(p.vx) * tail_scale);
            glEnd();
        } else if (p.kind == WorldFx::Kind::Tracer) {
            float streak = 0.028f * static_cast<float>(kHalfFtToKm);
            glLineWidth(2.2f);
            glBegin(GL_LINES);
            glColor4f(1.0f, 0.90f, 0.38f, life * 0.95f);
            glVertex3f(x, y, z);
            glColor4f(1.0f, 0.45f, 0.08f, 0.0f);
            glVertex3f(x - static_cast<float>(p.vy) * streak,
                       y - static_cast<float>(p.vz) * streak,
                       z + static_cast<float>(p.vx) * streak);
            glEnd();
        }
    }

    glPointSize(1.0f);
    glLineWidth(1.0f);
    glDisable(GL_BLEND);
}

void ModernRenderer::render_3d_view(int x, int y, int w, int h, const FlightTelemetry& tel,
                                    float eye_offset_m, float cy_vga, bool vga_frustum,
                                    bool widescreen_cockpit, float vga_frustum_aspect) {
    if (w <= 0 || h <= 0) return;
    glViewport(x, y, w, h);

    // Use Falcon 3.0's active camera (0x45C10) for Forward Cockpit, Side/Rear, Padlock, Chase, Orbit, Flyby!
    float pitch = tel.cam_pitch_deg + vr_cfg_.head_pitch_deg;
    float yaw   = tel.cam_yaw_deg   + vr_cfg_.head_yaw_deg;
    float roll  = tel.cam_roll_deg;

    constexpr float kWideCyFrac = 0.295f;
    float active_vfov = widescreen_cockpit ? std::clamp(vr_cfg_.widescreen_fov_deg, 36.0f, 68.0f)
                                           : (vr_cfg_.match_falcon_fov ? 38.58f : vr_cfg_.fov_deg);
    last_display_ppd_ = static_cast<float>(h) / std::max(10.0f, active_vfov);
    float horizon_ndc_y = widescreen_cockpit ? (1.0f - 2.0f * kWideCyFrac)
                                             : (vga_frustum ? (1.0f - 2.0f * (cy_vga / 200.0f)) : 0.0f);

    // Sync 3D world lighting automatically to Falcon 3.0's live Day / Dusk / Night VGA palette!
    update_scene_lighting();

    // 1. Authentic Falcon 3.0 retro sky + horizon haze backdrop (aspect-corrected so bank angle matches 3D terrain 1:1!)
    float vp_aspect = static_cast<float>(w) / static_cast<float>(std::max(1, h));
    draw_sky_backdrop(pitch, roll, horizon_ndc_y, active_vfov, vp_aspect);

    // 2. 3D perspective camera (exact off-center frustum matching active VGA view!)
    glMatrixMode(GL_PROJECTION);
    if (widescreen_cockpit) {
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        set_widescreen_cockpit_frustum(aspect, active_vfov, kWideCyFrac, 0.01f, 250.0f);
    } else if (vga_frustum && vr_cfg_.match_falcon_fov) {
        set_falcon_vga_frustum(cy_vga, 0.01f, 250.0f, vga_frustum_aspect);
    } else {
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        set_perspective(active_vfov, aspect, 0.01f, 250.0f);
    }

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float cam_y_km = std::max(0.029f, tel.cam_alt_ft * 0.0003048f);

    glRotatef(roll, 0.0f, 0.0f, 1.0f);
    glRotatef(-pitch, 1.0f, 0.0f, 0.0f);
    glRotatef(yaw, 0.0f, 1.0f, 0.0f);

    // 2b. World-locked retro night starfield (rotates cleanly with jet pitch/roll/yaw!)
    draw_night_stars();

    glTranslatef(-eye_offset_m * 0.001f, -cam_y_km, 0.0f);

    // 3. Authentic Falcon 3.0 horizon distance haze (GL_FOG matched live to Day/Dusk/Night horizon!)
    if (vr_cfg_.horizon_fog) {
        GLfloat fog_color[4] = {lit_.horizon[0], lit_.horizon[1], lit_.horizon[2], 1.0f};
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogfv(GL_FOG_COLOR, fog_color);
        glFogf(GL_FOG_START, 42.0f);
        glFogf(GL_FOG_END, 185.0f);
        glHint(GL_FOG_HINT, GL_FASTEST);
    } else {
        glDisable(GL_FOG);
    }

    // 4. Render 1:1 Falcon 3.0 Retro HD 3D world!
    draw_alt_terrain(tel);
    draw_isldat_water(tel);
    draw_roads_and_rivers(tel);
    draw_ils_runways(tel);
    draw_ground_structures(tel);
    draw_ground_units(tel);
    draw_low_alt_sand_dots(tel);
    draw_clouds(tel);
    draw_world_entities(tel);
    draw_contrails(tel);
    draw_missiles_and_trails(tel);
    draw_explosions(tel);
    draw_world_fx(tel);

    glDisable(GL_FOG);
}

void ModernRenderer::draw_hud_digit_string(float x, float y, float scale, const std::string& text) {
    glBegin(GL_LINES);
    float cx = x;
    for (char c : text) {
        if (c != ' ') stroke_char(cx, y, scale * 0.65f, scale, c);
        cx += scale * 0.9f;
    }
    glEnd();
}

void ModernRenderer::render_vector_hud(int x, int y, int w, int h, const FlightTelemetry& tel) {
    if (!vr_cfg_.show_vector_hud || w <= 0 || h <= 0) return;

    glViewport(x, y, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glLineWidth(1.8f);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    glOrtho(-aspect, aspect, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor4f(0.15f, 1.0f, 0.35f, 0.92f);

    glPushMatrix();
    glRotatef(tel.roll_deg, 0.0f, 0.0f, 1.0f);
    float p_off = -tel.pitch_deg * 0.025f;
    glBegin(GL_LINES);
    glVertex2f(-0.45f, p_off); glVertex2f(-0.08f, p_off);
    glVertex2f( 0.08f, p_off); glVertex2f( 0.45f, p_off);
    for (int deg = -30; deg <= 30; deg += 5) {
        if (deg == 0) continue;
        float ry = p_off + deg * 0.025f;
        if (ry < -0.65f || ry > 0.65f) continue;
        float tick = (deg > 0) ? -0.025f : 0.025f;
        glVertex2f(-0.18f, ry); glVertex2f(-0.07f, ry);
        glVertex2f(-0.18f, ry); glVertex2f(-0.18f, ry + tick);
        glVertex2f( 0.07f, ry); glVertex2f( 0.18f, ry);
        glVertex2f( 0.18f, ry); glVertex2f( 0.18f, ry + tick);
    }
    glEnd();
    glPopMatrix();

    glBegin(GL_LINES);
    glVertex2f(-0.03f, 0.0f); glVertex2f(0.03f, 0.0f);
    glVertex2f(0.0f, -0.03f); glVertex2f(0.0f, 0.03f);
    glEnd();

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%03d KTS", static_cast<int>(tel.airspeed_kts));
    draw_hud_digit_string(-0.62f, 0.02f, 0.055f, buf);
    std::snprintf(buf, sizeof(buf), "M .%02d", static_cast<int>(tel.mach * 100.0f) % 100);
    draw_hud_digit_string(-0.62f, -0.09f, 0.045f, buf);

    std::snprintf(buf, sizeof(buf), "%05d FT", static_cast<int>(tel.altitude_ft));
    draw_hud_digit_string(0.28f, 0.02f, 0.055f, buf);

    int hdg = (static_cast<int>(std::round(tel.yaw_deg)) % 360 + 360) % 360;
    std::snprintf(buf, sizeof(buf), "HDG %03d", hdg);
    draw_hud_digit_string(-0.18f, -0.78f, 0.055f, buf);

    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
}

void ModernRenderer::render_3d_cockpit_pass(int x, int y, int w, int h, const FlightTelemetry& tel,
                                            float cam_yaw, float cam_pitch, float cam_roll,
                                            float cy_vga, bool vga_frustum,
                                            GLuint live_fwd_tex, bool use_live_fwd,
                                            bool widescreen_cockpit,
                                            float vga_frustum_aspect,
                                            bool show_modern_aux) {
    if (w <= 0 || h <= 0) return;

    glViewport(x, y, w, h);
    // Clear ONLY depth so the 3D Virtual Cockpit (0.05 m .. 30 m) renders cleanly over the 3D world with zero Z-fighting!
    glClear(GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    if (widescreen_cockpit) {
        float active_vfov = std::clamp(vr_cfg_.widescreen_fov_deg, 36.0f, 68.0f);
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        set_widescreen_cockpit_frustum(aspect, active_vfov, 0.295f, 0.05f, 30.0f);
    } else if (vga_frustum && vr_cfg_.match_falcon_fov) {
        set_falcon_vga_frustum(cy_vga, 0.05f, 30.0f, vga_frustum_aspect);
    } else {
        float active_vfov = vr_cfg_.match_falcon_fov ? 38.58f : vr_cfg_.fov_deg;
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        set_perspective(active_vfov, aspect, 0.05f, 30.0f);
    }

    // Exact World-to-Camera * Aircraft-Body-to-World composition:
    //   When cam == aircraft, this equals Identity (Forward panel aligns 1:1 with HUD!).
    //   When aircraft banks/pitches while head is padlocked on a bandit/SAM, the entire F-16 cockpit,
    //   HUD struts, aux panels, consoles, wings, nose & tail rotate accurately around the pilot's eye!
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(cam_roll,        0.0f, 0.0f, 1.0f);
    glRotatef(-cam_pitch,      1.0f, 0.0f, 0.0f);
    glRotatef(cam_yaw,         0.0f, 1.0f, 0.0f);
    glRotatef(-tel.yaw_deg,    0.0f, 1.0f, 0.0f);
    glRotatef(tel.pitch_deg,   1.0f, 0.0f, 0.0f);
    glRotatef(-tel.roll_deg,   0.0f, 0.0f, 1.0f);

    if (has_vga_pal_) {
        cockpit_3d_.sync_palette(vga_dac_raw_.data());
    }
    Cockpit3D::LiveCockpitState lcs{};
    lcs.yaw_deg = tel.yaw_deg;
    lcs.pitch_deg = tel.pitch_deg;
    lcs.roll_deg = tel.roll_deg;
    lcs.airspeed_kts = tel.airspeed_kts;
    lcs.agl_ft = tel.agl_ft;
    lcs.g_load = tel.g_load;
    lcs.sim_time_sec = sim_time_sec_;
    lcs.missile_warning = !tel.missiles.empty();
    cockpit_3d_.render(lit_.daylight, lit_.ground_mul, live_fwd_tex, use_live_fwd, &lcs, show_modern_aux);
}

void ModernRenderer::draw_padlock_xray_box(int x, int y, int w, int h, const FlightTelemetry& tel,
                                           float cam_yaw, float cam_pitch, float cam_roll,
                                           float cy_vga, bool vga_frustum, float top_clip_vga_y,
                                           float vga_frustum_aspect) {
    if (!tel.padlock_active || !tel.padlock_has_target || w <= 0 || h <= 0) return;

    // 1. Target relative vector in OpenGL world space (+X East, +Y Up, -Z North)
    float cam_y_km = std::max(0.029f, tel.altitude_ft * 0.0003048f);
    float wx = static_cast<float>((tel.padlock_target_y - tel.world_y) * kHalfFtToKm);
    float wy = static_cast<float>(tel.padlock_target_z * kHalfFtToKm) - cam_y_km;
    float wz = static_cast<float>(-(tel.padlock_target_x - tel.world_x) * kHalfFtToKm);
    float dist_km = std::sqrt(wx * wx + wy * wy + wz * wz);
    if (dist_km < 1e-4f) return;

    // 2. Transform world direction into Camera View space: V = R_z(cam_roll) * R_x(-cam_pitch) * R_y(cam_yaw) * W
    auto rot_y = [](float& px, float& py, float& pz, float deg) {
        (void)py;
        float r = deg * kDeg2Rad, c = std::cos(r), s = std::sin(r);
        float nx =  c * px + s * pz;
        float nz = -s * px + c * pz;
        px = nx; pz = nz;
    };
    auto rot_x = [](float& px, float& py, float& pz, float deg) {
        (void)px;
        float r = deg * kDeg2Rad, c = std::cos(r), s = std::sin(r);
        float ny = c * py - s * pz;
        float nz = s * py + c * pz;
        py = ny; pz = nz;
    };
    auto rot_z = [](float& px, float& py, float& pz, float deg) {
        (void)pz;
        float r = deg * kDeg2Rad, c = std::cos(r), s = std::sin(r);
        float nx = c * px - s * py;
        float ny = s * px + c * py;
        px = nx; py = ny;
    };

    float cx = wx, cy = wy, cz = wz;
    rot_y(cx, cy, cz, cam_yaw);
    rot_x(cx, cy, cz, -cam_pitch);
    rot_z(cx, cy, cz, cam_roll);

    // 3. Also transform unit direction into Aircraft Body space to detect cockpit/nose occlusion ("X-Ray mode")!
    float bx = wx / dist_km, by = wy / dist_km, bz = wz / dist_km;
    rot_y(bx, by, bz, tel.yaw_deg);
    rot_x(bx, by, bz, -tel.pitch_deg);
    rot_z(bx, by, bz, tel.roll_deg);
    // In aircraft body space: +X Right, +Y Up, -Z Forward.
    // Below canopy sill/nose coaming (by < -0.14) OR behind front dashboard/MFD (-bz > 0.50 && by < 0.04) -> X-Ray!
    bool in_cockpit_xray = (by < -0.14f) || (-bz > 0.50f && by < 0.04f && std::abs(bx) < 0.48f) ||
                           (bz > 0.55f && by < 0.10f && std::abs(bx) < 0.42f);

    // 4. Project Camera View vector to NDC [-1..+1] using exact active frustum
    float ndc_x = 0.0f, ndc_y = 0.0f;
    bool in_front = (cz < -1e-4f);
    float depth = std::max(0.001f, -cz);
    if (vga_frustum && vr_cfg_.match_falcon_fov) {
        float cy_cl = std::clamp(cy_vga, 20.0f, 180.0f);
        float top = cy_cl / kFalconFyVga;
        float bot = -(200.0f - cy_cl) / kFalconFyVga;
        float half_w = (100.0f / kFalconFyVga) * std::max(4.0f / 3.0f, vga_frustum_aspect);
        ndc_x = (cx / depth) / half_w;
        float y_proj = cy / depth;
        ndc_y = 2.0f * (y_proj - bot) / (top - bot) - 1.0f;
    } else {
        float active_vfov = vr_cfg_.match_falcon_fov ? 38.58f : vr_cfg_.fov_deg;
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        float f = 1.0f / std::tan(active_vfov * 0.5f * kDeg2Rad);
        ndc_x = (cx / depth) * (f / aspect);
        ndc_y = (cy / depth) * f;
    }
    if (!in_front) {
        ndc_x = -ndc_x * 4.0f;
        ndc_y = -ndc_y * 4.0f;
    }

    // Clamp top edge cleanly below the Falcon 3.0 top Padlock data strip (y = 0..34)
    float max_ndc_y = 1.0f - 2.0f * (top_clip_vga_y / 200.0f);
    bool offscreen = !in_front || std::abs(ndc_x) > 0.94f || ndc_y < -0.94f || ndc_y > max_ndc_y;
    float draw_x = std::clamp(ndc_x, -0.92f, 0.92f);
    float draw_y = std::clamp(ndc_y, -0.90f, max_ndc_y - 0.04f);

    // 5. Render Red Padlock Box (always ON TOP of 3D cockpit with GL_DEPTH_TEST disabled!)
    glViewport(x, y, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float aspect = static_cast<float>(w) / static_cast<float>(h);
    float box_hy = tel.padlock_target_is_missile ? 0.068f : 0.056f;
    float box_hx = box_hy / aspect;
    float brk_x = box_hx * 0.42f;
    float brk_y = box_hy * 0.42f;

    if (offscreen) {
        // Red edge directional arrow pointing straight toward off-screen target during fast slew
        float ang = std::atan2(ndc_y - draw_y, (ndc_x - draw_x) * aspect);
        float ca = std::cos(ang), sa = std::sin(ang);
        float tip_x = draw_x + (ca * 0.055f) / aspect;
        float tip_y = draw_y + sa * 0.055f;
        float b1_x  = draw_x + ((-ca * 0.025f - sa * 0.032f)) / aspect;
        float b1_y  = draw_y + (-sa * 0.025f + ca * 0.032f);
        float b2_x  = draw_x + ((-ca * 0.025f + sa * 0.032f)) / aspect;
        float b2_y  = draw_y + (-sa * 0.025f - ca * 0.032f);
        glColor4f(1.0f, 0.18f, 0.14f, 0.95f);
        glBegin(GL_TRIANGLES);
        glVertex2f(tip_x, tip_y);
        glVertex2f(b1_x, b1_y);
        glVertex2f(b2_x, b2_y);
        glEnd();
    } else {
        // Subtle translucent red interior tint when target is occluded inside/behind the cockpit ("X-Ray Box"!)
        if (in_cockpit_xray) {
            glColor4f(1.0f, 0.10f, 0.08f, 0.16f);
            glBegin(GL_QUADS);
            glVertex2f(draw_x - box_hx, draw_y - box_hy);
            glVertex2f(draw_x + box_hx, draw_y - box_hy);
            glVertex2f(draw_x + box_hx, draw_y + box_hy);
            glVertex2f(draw_x - box_hx, draw_y + box_hy);
            glEnd();
        }

        // Outer crisp red corner brackets (thick, immediately readable over sky OR cockpit metal!)
        glLineWidth(in_cockpit_xray ? 2.6f : 2.2f);
        glColor4f(1.0f, in_cockpit_xray ? 0.16f : 0.22f, 0.14f, 0.98f);
        glBegin(GL_LINES);
        // Top-left corner
        glVertex2f(draw_x - box_hx, draw_y + box_hy); glVertex2f(draw_x - box_hx + brk_x, draw_y + box_hy);
        glVertex2f(draw_x - box_hx, draw_y + box_hy); glVertex2f(draw_x - box_hx, draw_y + box_hy - brk_y);
        // Top-right corner
        glVertex2f(draw_x + box_hx, draw_y + box_hy); glVertex2f(draw_x + box_hx - brk_x, draw_y + box_hy);
        glVertex2f(draw_x + box_hx, draw_y + box_hy); glVertex2f(draw_x + box_hx, draw_y + box_hy - brk_y);
        // Bottom-left corner
        glVertex2f(draw_x - box_hx, draw_y - box_hy); glVertex2f(draw_x - box_hx + brk_x, draw_y - box_hy);
        glVertex2f(draw_x - box_hx, draw_y - box_hy); glVertex2f(draw_x - box_hx, draw_y - box_hy + brk_y);
        // Bottom-right corner
        glVertex2f(draw_x + box_hx, draw_y - box_hy); glVertex2f(draw_x + box_hx - brk_x, draw_y - box_hy);
        glVertex2f(draw_x + box_hx, draw_y - box_hy); glVertex2f(draw_x + box_hx, draw_y - box_hy + brk_y);
        glEnd();

        if (in_cockpit_xray) {
            // Dashed inner red frame + center reticle pip to signify X-Ray tracking through the cockpit!
            float ihx = box_hx * 0.72f, ihy = box_hy * 0.72f;
            float pip_x = 0.014f / aspect, pip_y = 0.014f;
            glLineWidth(1.5f);
            glColor4f(1.0f, 0.35f, 0.20f, 0.92f);
            glBegin(GL_LINES);
            glVertex2f(draw_x - ihx, draw_y + ihy); glVertex2f(draw_x + ihx, draw_y + ihy);
            glVertex2f(draw_x - ihx, draw_y - ihy); glVertex2f(draw_x + ihx, draw_y - ihy);
            glVertex2f(draw_x - ihx, draw_y - ihy); glVertex2f(draw_x - ihx, draw_y + ihy);
            glVertex2f(draw_x + ihx, draw_y - ihy); glVertex2f(draw_x + ihx, draw_y + ihy);
            // Center pip
            glVertex2f(draw_x - pip_x, draw_y); glVertex2f(draw_x + pip_x, draw_y);
            glVertex2f(draw_x, draw_y - pip_y); glVertex2f(draw_x, draw_y + pip_y);
            glEnd();
        } else if (tel.padlock_target_is_missile) {
            // Red threat diamond inside box when padlocked on an incoming SAM/AAM!
            glLineWidth(1.8f);
            glColor4f(1.0f, 0.45f, 0.12f, 0.95f);
            glBegin(GL_LINE_LOOP);
            glVertex2f(draw_x, draw_y + box_hy * 0.65f);
            glVertex2f(draw_x + box_hx * 0.65f, draw_y);
            glVertex2f(draw_x, draw_y - box_hy * 0.65f);
            glVertex2f(draw_x - box_hx * 0.65f, draw_y);
            glEnd();
        }

        // Compact retro vector tag directly under the red box (e.g. "SAM 1.2" or "XRAY 0.8")
        char tag[32];
        if (tel.padlock_target_is_missile) {
            std::snprintf(tag, sizeof(tag), "%s %.1f", in_cockpit_xray ? "SAM XRAY" : "SAM", tel.padlock_range_nm);
        } else if (in_cockpit_xray) {
            std::snprintf(tag, sizeof(tag), "MASK %.1f", tel.padlock_range_nm);
        } else {
            std::snprintf(tag, sizeof(tag), "%.1f", tel.padlock_range_nm);
        }
        float txt_h = 0.028f;
        float txt_w = static_cast<float>(std::strlen(tag)) * txt_h * 0.9f / aspect;
        glLineWidth(1.5f);
        glColor4f(1.0f, 0.30f, 0.22f, 0.95f);
        glPushMatrix();
        glTranslatef(draw_x - txt_w * 0.5f, draw_y - box_hy - txt_h * 1.45f, 0.0f);
        glScalef(1.0f / aspect, 1.0f, 1.0f);
        draw_hud_digit_string(0.0f, 0.0f, txt_h, tag);
        glPopMatrix();
    }

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
}

void ModernRenderer::draw_native_padlock_strip(int px, int py, int pw, int ph, const FlightTelemetry& tel) {
    if (!vr_cfg_.keep_padlock_top_strip || pw <= 0 || ph <= 0) return;

    constexpr int W = 320;
    constexpr int H = 35;
    std::array<uint32_t, W * H> buf{};

    auto pack = [](int r, int g, int b, int a = 255) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(r, 0, 255)) |
               (static_cast<uint32_t>(std::clamp(g, 0, 255)) << 8) |
               (static_cast<uint32_t>(std::clamp(b, 0, 255)) << 16) |
               (static_cast<uint32_t>(std::clamp(a, 0, 255)) << 24);
    };
    auto set_px = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < W && y >= 0 && y < H) buf[y * W + x] = c;
    };
    auto fill_rect = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        int a0 = std::max(0, x0), a1 = std::min(W - 1, x1);
        int b0 = std::max(0, y0), b1 = std::min(H - 1, y1);
        for (int y = b0; y <= b1; ++y)
            for (int x = a0; x <= a1; ++x)
                buf[y * W + x] = c;
    };
    auto draw_rect = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int x = x0; x <= x1; ++x) { set_px(x, y0, c); set_px(x, y1, c); }
        for (int y = y0; y <= y1; ++y) { set_px(x0, y, c); set_px(x1, y, c); }
    };
    auto draw_line = [&](int x0, int y0, int x1, int y1, uint32_t c, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
        int dx = std::abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
        int err = dx + dy;
        for (int iter = 0; iter < 128; ++iter) {
            if (x0 >= clip_x0 && x0 <= clip_x1 && y0 >= clip_y0 && y0 <= clip_y1) set_px(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    };

    // Compact 4x5 1991 Spectrum HoloByte-style bitmap font (5 rows, 4-bit row mask MSB=left)
    auto glyph_rows = [](char ch, uint8_t out[5]) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        auto set5 = [&](uint8_t r0, uint8_t r1, uint8_t r2, uint8_t r3, uint8_t r4) {
            out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3; out[4] = r4;
        };
        switch (ch) {
            case '0': set5(0x6, 0x9, 0x9, 0x9, 0x6); break;
            case '1': set5(0x2, 0x6, 0x2, 0x2, 0x7); break;
            case '2': set5(0xE, 0x1, 0x6, 0x8, 0xF); break;
            case '3': set5(0xE, 0x1, 0x6, 0x1, 0xE); break;
            case '4': set5(0x9, 0x9, 0xF, 0x1, 0x1); break;
            case '5': set5(0xF, 0x8, 0xE, 0x1, 0xE); break;
            case '6': set5(0x7, 0x8, 0xE, 0x9, 0x6); break;
            case '7': set5(0xF, 0x1, 0x2, 0x4, 0x4); break;
            case '8': set5(0x6, 0x9, 0x6, 0x9, 0x6); break;
            case '9': set5(0x6, 0x9, 0x7, 0x1, 0xE); break;
            case 'A': set5(0x6, 0x9, 0xF, 0x9, 0x9); break;
            case 'B': set5(0xE, 0x9, 0xE, 0x9, 0xE); break;
            case 'C': set5(0x7, 0x8, 0x8, 0x8, 0x7); break;
            case 'D': set5(0xE, 0x9, 0x9, 0x9, 0xE); break;
            case 'E': set5(0xF, 0x8, 0xE, 0x8, 0xF); break;
            case 'F': set5(0xF, 0x8, 0xE, 0x8, 0x8); break;
            case 'G': set5(0x7, 0x8, 0xB, 0x9, 0x7); break;
            case 'H': set5(0x9, 0x9, 0xF, 0x9, 0x9); break;
            case 'I': set5(0xE, 0x4, 0x4, 0x4, 0xE); break;
            case 'J': set5(0x7, 0x2, 0x2, 0xA, 0x4); break;
            case 'K': set5(0x9, 0xA, 0xC, 0xA, 0x9); break;
            case 'L': set5(0x8, 0x8, 0x8, 0x8, 0xF); break;
            case 'M': set5(0x9, 0xF, 0xF, 0x9, 0x9); break;
            case 'N': set5(0x9, 0xD, 0xF, 0xB, 0x9); break;
            case 'O': set5(0x6, 0x9, 0x9, 0x9, 0x6); break;
            case 'P': set5(0xE, 0x9, 0xE, 0x8, 0x8); break;
            case 'Q': set5(0x6, 0x9, 0x9, 0xA, 0x5); break;
            case 'R': set5(0xE, 0x9, 0xE, 0xA, 0x9); break;
            case 'S': set5(0x7, 0x8, 0x6, 0x1, 0xE); break;
            case 'T': set5(0xF, 0x4, 0x4, 0x4, 0x4); break;
            case 'U': set5(0x9, 0x9, 0x9, 0x9, 0x6); break;
            case 'V': set5(0x9, 0x9, 0x9, 0x6, 0x6); break;
            case 'W': set5(0x9, 0x9, 0xF, 0xF, 0x9); break;
            case 'X': set5(0x9, 0x6, 0x6, 0x6, 0x9); break;
            case 'Y': set5(0x9, 0x9, 0x6, 0x4, 0x4); break;
            case 'Z': set5(0xF, 0x2, 0x4, 0x8, 0xF); break;
            case '.': set5(0x0, 0x0, 0x0, 0x0, 0x4); break;
            case '-': set5(0x0, 0x0, 0xE, 0x0, 0x0); break;
            case '\'': set5(0x4, 0x4, 0x0, 0x0, 0x0); break;
            case '(': set5(0x2, 0x4, 0x4, 0x4, 0x2); break;
            case ')': set5(0x4, 0x2, 0x2, 0x2, 0x4); break;
            case '/': set5(0x1, 0x2, 0x4, 0x8, 0x0); break;
            default:  set5(0x0, 0x0, 0x0, 0x0, 0x0); break;
        }
    };
    auto draw_text = [&](int x, int y, const char* s, uint32_t col, int max_x = 79) {
        int cx = x;
        for (; s && *s; ++s) {
            if (cx + 3 > max_x) break;
            uint8_t rows[5]{};
            glyph_rows(*s, rows);
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 4; ++c) {
                    if (rows[r] & (0x8u >> c)) set_px(cx + c, y + r, col);
                }
            }
            cx += 5;
        }
    };

    const uint32_t col_black  = pack(0, 0, 0, 255);
    const uint32_t col_amber  = pack(244, 196, 36, 255);
    const uint32_t col_white  = pack(232, 236, 240, 255);
    const uint32_t col_border = pack(146, 152, 160, 255);
    const uint32_t col_dark   = pack(70, 74, 82, 255);
    const uint32_t col_cyan   = pack(72, 178, 190, 255);
    const uint32_t col_green  = pack(24, 236, 62, 255);
    const uint32_t col_red    = pack(248, 52, 44, 255);
    const uint32_t col_box    = pack(212, 82, 80, 255);

    // 1. LEFT PANEL (x = 0..79): Authentic black Padlock telemetry box
    fill_rect(0, 0, 79, H - 1, col_black);
    const char* tname = (tel.padlock_has_target && tel.padlock_target_name[0]) ? tel.padlock_target_name : "NONE";
    char v_rng[16], v_alt[16], v_spd[16], v_gld[16];
    if (tel.padlock_has_target) {
        std::snprintf(v_rng, sizeof(v_rng), "%04.1f", std::clamp(tel.padlock_range_nm, 0.0f, 99.9f));
    } else {
        std::snprintf(v_rng, sizeof(v_rng), "--.-");
    }
    int agl_i = std::clamp(static_cast<int>(std::round(tel.agl_ft)), 0, 99999);
    int spd_i = std::clamp(static_cast<int>(std::round(tel.airspeed_kts)), 0, 9999);
    float g_v = std::clamp(tel.g_load, -9.9f, 99.9f);
    std::snprintf(v_alt, sizeof(v_alt), "%05d", agl_i);
    std::snprintf(v_spd, sizeof(v_spd), "%04d", spd_i);
    std::snprintf(v_gld, sizeof(v_gld), "%04.1f", std::abs(g_v));

    draw_text(1,  1, "TARGET", col_amber);
    draw_text(33, 1, tname,    tel.padlock_target_is_missile ? pack(255, 224, 96) : col_white, 79);
    draw_text(1,  8, "RANGE",  col_amber);
    draw_text(33, 8, v_rng,    col_white, 58);
    draw_text(59, 8, "N.M.",   col_amber, 79);
    draw_text(1, 15, "ALT",    col_amber);
    draw_text(33,15, v_alt,    col_white, 58);
    draw_text(59,15, "AGL",    col_amber, 79);
    draw_text(1, 22, "SPEED",  col_amber);
    draw_text(33,22, v_spd,    col_white, 58);
    draw_text(59,22, "KTAS",   col_amber, 79);
    draw_text(1, 29, "LOAD",   col_amber);
    draw_text(33,29, v_gld,    col_white, 58);
    draw_text(59,29, "G'S",    col_amber, 79);

    // 2. CENTER PANEL (x = 80..239): 360-deg Panoramic Canopy Strip
    fill_rect(80, 0, 239, H - 1, pack(8, 12, 20, 152));
    for (int y = 0; y < H; ++y) { set_px(80, y, col_border); set_px(239, y, col_border); }
    for (int x = 80; x <= 239; ++x) set_px(x, H - 1, col_border);

    // Corner rear-tail ticks (matches Falcon 3.0's canopy strip edges)
    for (int i = 0; i < 4; ++i) {
        fill_rect(81, 30 + i, 81 + i, 30 + i, col_border);
        fill_rect(238 - i, 30 + i, 238, 30 + i, col_border);
    }

    // Miniature forward F-16 HUD icon at bottom-center (cx = 160)
    fill_rect(155, 27, 165, 33, pack(26, 34, 42, 235));
    for (int x = 156; x <= 164; ++x) set_px(x, 26, col_cyan);
    for (int y = 27; y <= 33; ++y) { set_px(154, y, col_dark); set_px(166, y, col_dark); }
    for (int y = 28; y <= 32; y += 2) { set_px(157, y, col_green); set_px(163, y, col_green); }
    set_px(160, 29, col_green); set_px(159, 30, col_green); set_px(161, 30, col_green);

    // Compute target azimuth & elevation in Aircraft Body Frame (+X Right, +Y Up, -Z Forward)
    int box_cx = 160, box_cy = 24;
    bool target_below_sill = false;
    if (tel.padlock_has_target) {
        float cam_y_km = std::max(0.029f, tel.altitude_ft * 0.0003048f);
        float wx = static_cast<float>((tel.padlock_target_y - tel.world_y) * kHalfFtToKm);
        float wy = static_cast<float>(tel.padlock_target_z * kHalfFtToKm) - cam_y_km;
        float wz = static_cast<float>(-(tel.padlock_target_x - tel.world_x) * kHalfFtToKm);
        float dist = std::sqrt(wx * wx + wy * wy + wz * wz);
        if (dist > 1e-4f) {
            float bx = wx / dist, by = wy / dist, bz = wz / dist;
            auto rot_y = [](float& px0, float& py0, float& pz0, float deg) {
                (void)py0;
                float r = deg * kDeg2Rad, c = std::cos(r), s = std::sin(r);
                float nx = c * px0 + s * pz0, nz = -s * px0 + c * pz0;
                px0 = nx; pz0 = nz;
            };
            auto rot_x = [](float& px0, float& py0, float& pz0, float deg) {
                (void)px0;
                float r = deg * kDeg2Rad, c = std::cos(r), s = std::sin(r);
                float ny = c * py0 - s * pz0, nz = s * py0 + c * pz0;
                py0 = ny; pz0 = nz;
            };
            auto rot_z = [](float& px0, float& py0, float& pz0, float deg) {
                (void)pz0;
                float r = deg * kDeg2Rad, c = std::cos(r), s = std::sin(r);
                float nx = c * px0 - s * py0, ny = s * px0 + c * py0;
                px0 = nx; py0 = ny;
            };
            rot_y(bx, by, bz, tel.yaw_deg);
            rot_x(bx, by, bz, -tel.pitch_deg);
            rot_z(bx, by, bz, tel.roll_deg);

            float az_deg = std::atan2(bx, -bz) / kDeg2Rad; // -180..+180 (0 = nose)
            float el_deg = std::asin(std::clamp(by, -1.0f, 1.0f)) / kDeg2Rad; // -90..+90
            box_cx = std::clamp(static_cast<int>(std::round(160.0f + (az_deg / 180.0f) * 75.0f)), 85, 234);
            box_cy = std::clamp(static_cast<int>(std::round(26.0f - (el_deg / 90.0f) * 23.0f)), 3, 31);
            target_below_sill = (by < -0.14f);

            // Red target '+' pip
            set_px(box_cx, box_cy, col_red);
            set_px(box_cx - 1, box_cy, col_red);
            set_px(box_cx + 1, box_cy, col_red);
            set_px(box_cx, box_cy - 1, col_red);
            set_px(box_cx, box_cy + 1, col_red);
        }
    }
    // Red view-framing rectangle inside center panoramic strip
    int rx0 = std::clamp(box_cx - 13, 81, 212);
    int rx1 = rx0 + 26;
    int ry0 = std::clamp(box_cy - 7, 1, H - 15);
    int ry1 = ry0 + 13;
    draw_rect(rx0, ry0, rx1, ry1, col_box);
    if (target_below_sill) {
        // Subtle red downward chevron indicating target is below canopy sill (X-Ray!)
        int mx = (rx0 + rx1) / 2;
        set_px(mx - 2, H - 3, col_red);
        set_px(mx - 1, H - 2, col_red);
        set_px(mx,     H - 1, col_red);
        set_px(mx + 1, H - 2, col_red);
        set_px(mx + 2, H - 3, col_red);
    }

    // 3. RIGHT PANEL (x = 240..319): Miniature Forward HUD Artificial Horizon
    fill_rect(240, 0, W - 1, H - 1, pack(8, 12, 20, 138));
    for (int x = 240; x < W; ++x) set_px(x, H - 1, col_border);

    // Mini F-16 HUD arch centered at x = 280
    constexpr int kHudCx = 280;
    constexpr int kHudCy = 23;
    fill_rect(266, 13, 294, 33, pack(6, 10, 16, 195));
    for (int y = 13; y <= 33; ++y) {
        set_px(264, y, col_dark); set_px(265, y, col_border);
        set_px(295, y, col_border); set_px(296, y, col_dark);
    }
    for (int x = 266; x <= 294; ++x) {
        float u = static_cast<float>(x - kHudCx) / 15.0f;
        int ay = 10 + static_cast<int>(std::round(u * u * 3.0f));
        set_px(x, ay, col_cyan);
        set_px(x, ay + 1, pack(32, 92, 102, 220));
    }

    // Live green bank/pitch horizon bar inside mini HUD arch
    float roll_r = -tel.roll_deg * kDeg2Rad;
    float pitch_px = std::clamp(tel.pitch_deg * 0.22f, -7.0f, 7.0f);
    float cr = std::cos(roll_r), sr = std::sin(roll_r);
    int hx0 = kHudCx + static_cast<int>(std::round(-9.0f * cr));
    int hy0 = kHudCy + static_cast<int>(std::round(pitch_px - 9.0f * sr));
    int hx1 = kHudCx + static_cast<int>(std::round( 9.0f * cr));
    int hy1 = kHudCy + static_cast<int>(std::round(pitch_px + 9.0f * sr));
    draw_line(hx0, hy0, hx1, hy1, col_green, 267, 14, 293, 32);
    // Small downward pitch ticks on left/right ends of mini horizon bar
    draw_line(hx0, hy0, hx0 + static_cast<int>(std::round(-2.5f * sr)), hy0 + static_cast<int>(std::round(2.5f * cr)),
              col_green, 267, 14, 293, 32);
    draw_line(hx1, hy1, hx1 + static_cast<int>(std::round(-2.5f * sr)), hy1 + static_cast<int>(std::round(2.5f * cr)),
              col_green, 267, 14, 293, 32);
    // Tiny center boresight dot
    set_px(kHudCx, kHudCy, pack(220, 120, 110, 255));

    // Upload to persistent 320x35 GL_NEAREST texture and blit cleanly at top of viewport
    if (padlock_strip_tex_ == 0) {
        glGenTextures(1, &padlock_strip_tex_);
        glBindTexture(GL_TEXTURE_2D, padlock_strip_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    } else {
        glBindTexture(GL_TEXTURE_2D, padlock_strip_tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    }
    int strip_h = static_cast<int>(std::round(static_cast<float>(ph) * (35.0f / 200.0f)));
    if (strip_h > 0) {
        draw_vga_quad(px, py + ph - strip_h, pw, strip_h, padlock_strip_tex_, 0.0f, 1.0f, true);
    }
}

void ModernRenderer::render_frame(int win_w, int win_h, GLuint vga_tex, GLuint vga_cockpit_tex,
                                  GLuint vga_hud_tex, const FlightTelemetry& raw_tel, float dt_sec) {
    last_dt_sec_ = dt_sec;
    sim_time_sec_ += std::clamp(dt_sec, 0.0005f, 0.05f);
    update_world_fx(dt_sec, raw_tel.in_flight);
    update_missile_trails(raw_tel, dt_sec);
    update_explosions_and_contrails(raw_tel, dt_sec);
    update_telemetry(raw_tel, dt_sec);
    const FlightTelemetry& tel = smooth_tel_;
    if (!tel.in_flight || !tel.is_external_cam) {
        has_smooth_chase_player_ = false;
    }

    glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float win_aspect = static_cast<float>(win_w) / static_cast<float>(std::max(1, win_h));
    float target_aspect = 4.0f / 3.0f;
    int vx = 0, vy = 0, vw = win_w, vh = win_h;
    if (win_aspect > target_aspect) {
        vw = static_cast<int>(win_h * target_aspect);
        vx = (win_w - vw) / 2;
    } else {
        vh = static_cast<int>(win_w / target_aspect);
        vy = (win_h - vh) / 2;
    }

    if (!tel.in_flight || mode_ == RenderMode::ClassicVga) {
        has_smooth_padlock_ = false;
        draw_vga_quad(vx, vy, vw, vh, vga_tex);
        return;
    }

    // Detect enhanced Unclamped 3D Padlock (8) vs Free-Look 3D Virtual Cockpit vs Standard Views!
    // Lock is_forward_cockpit directly to RAM view_mode == 0 (never depend on world_y_min pixel-counting,
    // which can flicker on overcast skies, radio text overlays, or VGA page flips!)
    bool use_3d_padlock = (tel.padlock_active && vr_cfg_.enhanced_3d_padlock);
    bool is_forward_cockpit = (!tel.is_external_cam && tel.view_mode == 0);
    bool is_freelook_active = (is_forward_cockpit && vr_cfg_.virtual_cockpit_freelook &&
                               (std::abs(vr_cfg_.head_yaw_deg) > 0.5f || std::abs(vr_cfg_.head_pitch_deg) > 0.5f));

    // Prepare unclamped 3D Padlock camera telemetry when in Padlock View (8)!
    FlightTelemetry active_tel = tel;
    if (use_3d_padlock && tel.padlock_has_target) {
        float cam_y_km = std::max(0.029f, tel.altitude_ft * 0.0003048f);
        float dx = static_cast<float>((tel.padlock_target_y - tel.world_y) * kHalfFtToKm);
        float dy = static_cast<float>(tel.padlock_target_z * kHalfFtToKm) - cam_y_km;
        float dz = static_cast<float>(-(tel.padlock_target_x - tel.world_x) * kHalfFtToKm);
        float hdist = std::sqrt(dx * dx + dz * dz);

        float tgt_yaw = std::atan2(dx, -dz) / kDeg2Rad;
        while (tgt_yaw < 0.0f) tgt_yaw += 360.0f;
        while (tgt_yaw >= 360.0f) tgt_yaw -= 360.0f;
        float tgt_pitch = std::clamp(std::atan2(dy, std::max(1e-5f, hdist)) / kDeg2Rad, -86.0f, 86.0f);
        float tgt_roll = 0.0f; // Horizon-leveled head so the 3D F-16C cockpit banks naturally around you!

        if (!has_smooth_padlock_) {
            smooth_pad_yaw_ = tgt_yaw;
            smooth_pad_pitch_ = tgt_pitch;
            smooth_pad_roll_ = tgt_roll;
            has_smooth_padlock_ = true;
        } else {
            float a = 1.0f - std::exp(-20.0f * std::clamp(dt_sec, 0.001f, 0.05f));
            float dyaw = tgt_yaw - smooth_pad_yaw_;
            while (dyaw > 180.0f) dyaw -= 360.0f;
            while (dyaw < -180.0f) dyaw += 360.0f;
            smooth_pad_yaw_ += dyaw * a;
            while (smooth_pad_yaw_ < 0.0f) smooth_pad_yaw_ += 360.0f;
            while (smooth_pad_yaw_ >= 360.0f) smooth_pad_yaw_ -= 360.0f;
            smooth_pad_pitch_ += (tgt_pitch - smooth_pad_pitch_) * a;
            smooth_pad_roll_ += (tgt_roll - smooth_pad_roll_) * a;
        }

        // Override camera to pilot eye + unclamped smooth 3D padlock orientation!
        active_tel.cam_x = tel.world_x;
        active_tel.cam_y = tel.world_y;
        active_tel.cam_alt_ft = tel.altitude_ft;
        active_tel.cam_yaw_deg = smooth_pad_yaw_;
        active_tel.cam_pitch_deg = smooth_pad_pitch_;
        active_tel.cam_roll_deg = smooth_pad_roll_;
        active_tel.is_external_cam = false;
    } else {
        has_smooth_padlock_ = false;
    }

    // Auto-compute exact optical horizon row cy_vga for the current Falcon view:
    //  - Forward Cockpit (world_y 0..105):  cy = 53.0 (verified 1:1 with HUD 0-deg horizon & TD lock box!)
    //  - External Chase (world_y 0..169):   cy = 85.0
    //  - Padlock (world_y 35..199):         cy = 117.0 (or 100.0 if top strip hidden)
    float cy_vga = 53.0f;
    if (use_3d_padlock) {
        cy_vga = vr_cfg_.keep_padlock_top_strip ? 117.0f : 100.0f;
    } else if (is_forward_cockpit) {
        cy_vga = 53.0f + vr_cfg_.hud_cy_trim_px;
    } else {
        cy_vga = std::clamp(0.5f * static_cast<float>(tel.world_y_min + tel.world_y_max), 47.0f, 130.0f) + vr_cfg_.hud_cy_trim_px;
    }

    // When DOS stays in Forward Cockpit (view_mode == 0) during Host-Driven Native 3D Padlock ('8'),
    // vga_cockpit_tex holds Falcon 3.0's 100% REAL LIVE 2D HUD + LIVE MFD RADAR + LIVE RWR!
    // Map it directly onto the 3D Virtual Cockpit forward panel & HUD glass, and draw our matching
    // retro pixel-art top Padlock data strip!
    bool use_live_padlock_hud = (use_3d_padlock && tel.view_mode == 0 && vga_cockpit_tex != 0);

    auto render_padlock_top_strip = [&](int px, int py, int pw, int ph) {
        if (!vr_cfg_.keep_padlock_top_strip) return;
        if (tel.view_mode == 0) {
            draw_native_padlock_strip(px, py, pw, ph, active_tel);
        } else if (vga_cockpit_tex != 0) {
            int strip_h = static_cast<int>(std::round(static_cast<float>(ph) * (35.0f / 200.0f)));
            if (strip_h > 0) {
                // Fallback: composite DOS Falcon 3.0's own top Padlock bar (rows 0..34)
                draw_vga_quad(px, py + ph - strip_h, pw, strip_h, vga_cockpit_tex, 0.0f, 35.0f / 200.0f, true);
            }
        }
    };

    float final_cam_yaw   = active_tel.cam_yaw_deg   + vr_cfg_.head_yaw_deg;
    float final_cam_pitch = active_tel.cam_pitch_deg + vr_cfg_.head_pitch_deg;
    float final_cam_roll  = active_tel.cam_roll_deg;

    if (mode_ == RenderMode::Modern3D) {
        bool use_modern_3d_pit = (vr_cfg_.widescreen_3d_cockpit && is_forward_cockpit && !use_3d_padlock);
        if (use_3d_padlock) {
            render_3d_view(0, 0, win_w, win_h, active_tel, 0.0f, cy_vga, false);
            render_3d_cockpit_pass(0, 0, win_w, win_h, active_tel,
                                   final_cam_yaw, final_cam_pitch, final_cam_roll,
                                   cy_vga, false, vga_cockpit_tex, use_live_padlock_hud,
                                   false, 4.0f / 3.0f, vr_cfg_.widescreen_3d_cockpit);
            draw_padlock_xray_box(0, 0, win_w, win_h, active_tel,
                                  final_cam_yaw, final_cam_pitch, final_cam_roll,
                                  cy_vga, false, vr_cfg_.keep_padlock_top_strip ? 36.0f : 4.0f);
            render_padlock_top_strip(vx, vy, vw, vh);
        } else if (use_modern_3d_pit) {
            // Modern 3D Mode: Full-window 16:9 3D Virtual F-16 Cockpit with Falcon 3.0's real live 2D HUD/MFD/RWR
            // dead-center + Left/Right Aux + Front Live Warning Matrix + Live Backup ADI/HSI Pedestal!
            render_3d_view(0, 0, win_w, win_h, active_tel, 0.0f, cy_vga, false, true);
            render_3d_cockpit_pass(0, 0, win_w, win_h, active_tel,
                                   final_cam_yaw, final_cam_pitch, final_cam_roll,
                                   cy_vga, false, vga_cockpit_tex, true, true, 4.0f / 3.0f, true);
        } else if (is_freelook_active) {
            render_3d_view(0, 0, win_w, win_h, active_tel, 0.0f, 100.0f, false);
            render_3d_cockpit_pass(0, 0, win_w, win_h, active_tel,
                                   final_cam_yaw, final_cam_pitch, final_cam_roll,
                                   100.0f, false, vga_cockpit_tex, true,
                                   false, 4.0f / 3.0f, vr_cfg_.widescreen_3d_cockpit);
        } else {
            render_3d_view(0, 0, win_w, win_h, active_tel, 0.0f, 100.0f, false);
            // Show floating original green HUD only when in Forward Cockpit view!
            if (vr_cfg_.use_original_hud && is_forward_cockpit && vga_hud_tex != 0) {
                draw_hud_overlay_fullscreen(0, 0, win_w, win_h, vga_hud_tex);
            }
            if (vr_cfg_.show_vector_hud && !tel.is_external_cam) {
                render_vector_hud(0, 0, win_w, win_h, tel);
            }
        }
    } else if (mode_ == RenderMode::HybridCockpit) {
        bool fill_wide = (vr_cfg_.hybrid_fill_widescreen && win_aspect > target_aspect + 0.01f);
        int rx = fill_wide ? 0 : vx;
        int ry = fill_wide ? 0 : vy;
        int rw = fill_wide ? win_w : vw;
        int rh = fill_wide ? win_h : vh;
        float r_asp = fill_wide ? win_aspect : target_aspect;

        if (use_3d_padlock) {
            // Unclamped 3D Padlock (8) inside Falcon 3.0's clean 4-view 3D Virtual Cockpit (NO Modern aux panels!)
            // Respects Hybrid widescreen fill so it never snaps back into 4:3!
            render_3d_view(rx, ry, rw, rh, active_tel, 0.0f, cy_vga, true, false, r_asp);
            render_3d_cockpit_pass(rx, ry, rw, rh, active_tel,
                                   final_cam_yaw, final_cam_pitch, final_cam_roll,
                                   cy_vga, true, vga_cockpit_tex, use_live_padlock_hud,
                                   false, r_asp, false);
            draw_padlock_xray_box(rx, ry, rw, rh, active_tel,
                                  final_cam_yaw, final_cam_pitch, final_cam_roll,
                                  cy_vga, true, vr_cfg_.keep_padlock_top_strip ? 36.0f : 4.0f, r_asp);
            render_padlock_top_strip(vx, vy, vw, vh);
        } else if (is_freelook_active) {
            // Smooth 3D Virtual Cockpit Free-Look using Falcon 3.0's 4 authentic native sprites
            render_3d_view(rx, ry, rw, rh, active_tel, 0.0f, cy_vga, true, false, r_asp);
            render_3d_cockpit_pass(rx, ry, rw, rh, active_tel,
                                   final_cam_yaw, final_cam_pitch, final_cam_roll,
                                   cy_vga, true, vga_cockpit_tex, true,
                                   false, r_asp, false);
        } else {
            // Classic authentic 4:3 Falcon 3.0 2D Cockpit + Live HUD!
            // When hybrid_fill_widescreen is ON, expand raw 3D world horizontally across the entire window
            // while keeping the center 4:3 cockpit & HUD 100% 1:1 pixel-locked!
            render_3d_view(rx, ry, rw, rh, tel, 0.0f, cy_vga, true, false, r_asp);
            if (vga_cockpit_tex != 0) {
                draw_vga_quad(vx, vy, vw, vh, vga_cockpit_tex, 0.0f, 1.0f, true);
            }
            if (vr_cfg_.show_vector_hud && is_forward_cockpit) {
                render_vector_hud(vx, vy + vh / 2, vw, vh / 2, tel);
            }
        }
    } else if (mode_ == RenderMode::VrStereoPreview) {
        int half_w = win_w / 2;
        float half_ipd_m = (vr_cfg_.ipd_mm * 0.001f) * 0.5f;
        for (int eye = 0; eye < 2; ++eye) {
            int ex = (eye == 0) ? 0 : half_w;
            int ew = (eye == 0) ? half_w : (win_w - half_w);
            float ipd = (eye == 0) ? -half_ipd_m : +half_ipd_m;
            if (eye == 1) glClear(GL_DEPTH_BUFFER_BIT);
            render_3d_view(ex, 0, ew, win_h, active_tel, ipd, 100.0f, false);
            if (use_3d_padlock || is_freelook_active) {
                render_3d_cockpit_pass(ex, 0, ew, win_h, active_tel,
                                       final_cam_yaw, final_cam_pitch, final_cam_roll,
                                       100.0f, false, vga_cockpit_tex, is_freelook_active || use_live_padlock_hud);
                if (use_3d_padlock) {
                    draw_padlock_xray_box(ex, 0, ew, win_h, active_tel,
                                          final_cam_yaw, final_cam_pitch, final_cam_roll,
                                          100.0f, false, 4.0f);
                }
            } else {
                if (vr_cfg_.use_original_hud && is_forward_cockpit && vga_hud_tex != 0) {
                    draw_hud_overlay_fullscreen(ex, 0, ew, win_h, vga_hud_tex);
                }
                if (vr_cfg_.show_vector_hud && !tel.is_external_cam) render_vector_hud(ex, 0, ew, win_h, tel);
            }
        }
    }
}

} // namespace falcon
