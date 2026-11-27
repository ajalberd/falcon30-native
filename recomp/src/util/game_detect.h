#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace falcon {

namespace detail {

inline std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline std::string to_upper_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

inline void trim_path_string(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '"' || s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
}

inline bool dir_has_any_falcon_exe(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string nm = to_upper_ascii(entry.path().filename().string());
        if (nm == "FALCONCD.EXE" || nm == "HORNETCD.EXE" || nm == "MIGCD.EXE") {
            return true;
        }
    }
    return false;
}

} // namespace detail

inline std::filesystem::path get_executable_dir() {
#ifdef _WIN32
    char buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::error_code ec;
        return std::filesystem::path(buf).parent_path();
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

inline std::string resolve_falcon_game_dir(const std::filesystem::path& base) {
    if (base.empty()) return "";
    std::error_code ec;
    if (detail::dir_has_any_falcon_exe(base)) {
        return std::filesystem::absolute(base, ec).string();
    }
    std::filesystem::path sub = base / "FALCON3";
    if (detail::dir_has_any_falcon_exe(sub)) {
        return std::filesystem::absolute(sub, ec).string();
    }
    return "";
}

#ifdef _WIN32
namespace detail {

inline bool read_reg_sz(HKEY hk, const char* val_name, std::string& out) {
    char buf[1024]{};
    DWORD cb = sizeof(buf) - 1;
    DWORD type = 0;
    if (RegQueryValueExA(hk, val_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) != ERROR_SUCCESS) {
        return false;
    }
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || !buf[0]) return false;
    out = buf;
    trim_path_string(out);
    return !out.empty();
}

inline std::string try_reg_path_value(HKEY root, const char* subkey, REGSAM sam, const char* val_name) {
    HKEY hk = nullptr;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ | sam, &hk) != ERROR_SUCCESS) return "";
    std::string raw;
    std::string resolved;
    if (read_reg_sz(hk, val_name, raw)) {
        resolved = resolve_falcon_game_dir(raw);
    }
    RegCloseKey(hk);
    return resolved;
}

inline std::string scan_gog_games_tree(HKEY root, const char* base_key, REGSAM sam) {
    HKEY hk_base = nullptr;
    if (RegOpenKeyExA(root, base_key, 0, KEY_READ | sam, &hk_base) != ERROR_SUCCESS) return "";

    // Fast path: known GOG Falcon Gold game ID (1443160656)
    {
        HKEY hk_fg = nullptr;
        if (RegOpenKeyExA(hk_base, "1443160656", 0, KEY_READ | sam, &hk_fg) == ERROR_SUCCESS) {
            const char* vals[] = {"path", "workingDir", "PATH"};
            for (const char* v : vals) {
                std::string raw;
                if (read_reg_sz(hk_fg, v, raw)) {
                    std::string r = resolve_falcon_game_dir(raw);
                    if (!r.empty()) {
                        RegCloseKey(hk_fg);
                        RegCloseKey(hk_base);
                        return r;
                    }
                }
            }
            RegCloseKey(hk_fg);
        }
    }

    // Thorough enumeration across ALL GOG.com\Games\* subkeys
    char sub_name[256];
    for (DWORD idx = 0;; ++idx) {
        DWORD name_len = sizeof(sub_name);
        LONG rc = RegEnumKeyExA(hk_base, idx, sub_name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) break;
        HKEY hk_sub = nullptr;
        if (RegOpenKeyExA(hk_base, sub_name, 0, KEY_READ | sam, &hk_sub) != ERROR_SUCCESS) continue;
        const char* vals[] = {"path", "workingDir", "PATH"};
        for (const char* v : vals) {
            std::string raw;
            if (read_reg_sz(hk_sub, v, raw)) {
                std::string r = resolve_falcon_game_dir(raw);
                if (!r.empty()) {
                    RegCloseKey(hk_sub);
                    RegCloseKey(hk_base);
                    return r;
                }
            }
        }
        RegCloseKey(hk_sub);
    }

    RegCloseKey(hk_base);
    return "";
}

inline std::string scan_uninstall_tree(HKEY root, const char* base_key, REGSAM sam) {
    HKEY hk_base = nullptr;
    if (RegOpenKeyExA(root, base_key, 0, KEY_READ | sam, &hk_base) != ERROR_SUCCESS) return "";

    // Direct check for GOG Falcon Gold Inno Setup uninstall key
    {
        HKEY hk_is = nullptr;
        if (RegOpenKeyExA(hk_base, "1443160656_is1", 0, KEY_READ | sam, &hk_is) == ERROR_SUCCESS) {
            const char* vals[] = {"InstallLocation", "Inno Setup: App Path"};
            for (const char* v : vals) {
                std::string raw;
                if (read_reg_sz(hk_is, v, raw)) {
                    std::string r = resolve_falcon_game_dir(raw);
                    if (!r.empty()) {
                        RegCloseKey(hk_is);
                        RegCloseKey(hk_base);
                        return r;
                    }
                }
            }
            RegCloseKey(hk_is);
        }
    }

    // Enumerate all Uninstall entries whose DisplayName mentions "Falcon"
    char sub_name[256];
    for (DWORD idx = 0;; ++idx) {
        DWORD name_len = sizeof(sub_name);
        if (RegEnumKeyExA(hk_base, idx, sub_name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        HKEY hk_sub = nullptr;
        if (RegOpenKeyExA(hk_base, sub_name, 0, KEY_READ | sam, &hk_sub) != ERROR_SUCCESS) continue;
        std::string disp;
        if (read_reg_sz(hk_sub, "DisplayName", disp) && to_lower_ascii(disp).find("falcon") != std::string::npos) {
            const char* vals[] = {"InstallLocation", "Inno Setup: App Path"};
            for (const char* v : vals) {
                std::string raw;
                if (read_reg_sz(hk_sub, v, raw)) {
                    std::string r = resolve_falcon_game_dir(raw);
                    if (!r.empty()) {
                        RegCloseKey(hk_sub);
                        RegCloseKey(hk_base);
                        return r;
                    }
                }
            }
        }
        RegCloseKey(hk_sub);
    }

    RegCloseKey(hk_base);
    return "";
}

inline void parse_steam_library_folders(const std::filesystem::path& steam_root, std::vector<std::filesystem::path>& out_libs) {
    out_libs.push_back(steam_root);
    std::ifstream in(steam_root / "steamapps" / "libraryfolders.vdf");
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        std::string low = to_lower_ascii(line);
        size_t pos = low.find("\"path\"");
        if (pos == std::string::npos) continue;
        size_t q1 = line.find('"', pos + 6);
        if (q1 == std::string::npos) continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string p = line.substr(q1 + 1, q2 - q1 - 1);
        // Unescape double backslashes in VDF
        std::string clean;
        for (size_t i = 0; i < p.size(); ++i) {
            if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') {
                clean.push_back('\\');
                ++i;
            } else {
                clean.push_back(p[i]);
            }
        }
        if (!clean.empty()) out_libs.emplace_back(clean);
    }
}

inline std::string scan_steam_installs() {
    struct SteamReg { HKEY root; const char* key; const char* val; };
    const SteamReg regs[] = {
        { HKEY_CURRENT_USER,  "Software\\Valve\\Steam", "SteamPath" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath" },
    };
    const REGSAM sams[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    const char* app_dirs[] = {
        "Falcon Gold",
        "Falcon 3.0",
        "Falcon 3.0 Gold",
        "Falcon Collection",
    };

    for (const auto& r : regs) {
        for (REGSAM sam : sams) {
            HKEY hk = nullptr;
            if (RegOpenKeyExA(r.root, r.key, 0, KEY_READ | sam, &hk) != ERROR_SUCCESS) continue;
            std::string sp;
            bool ok = read_reg_sz(hk, r.val, sp);
            RegCloseKey(hk);
            if (!ok) continue;

            std::vector<std::filesystem::path> libs;
            parse_steam_library_folders(sp, libs);
            for (const auto& lib : libs) {
                for (const char* app : app_dirs) {
                    std::string resolved = resolve_falcon_game_dir(lib / "steamapps" / "common" / app);
                    if (!resolved.empty()) return resolved;
                }
            }
        }
    }
    return "";
}

} // namespace detail
#endif

// Thorough GOG / Steam / Uninstall Registry + C:..H: auto-detection for Falcon Gold
inline std::string detect_falcon_gold_auto() {
#ifdef _WIN32
    const REGSAM sams[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

    // 1. GOG.com\Games\* (both WOW6432Node and native views, HKLM & HKCU)
    struct TreeCheck { HKEY root; const char* key; };
    const TreeCheck gog_trees[] = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\GOG.com\\Games" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\GOG.com\\Games" },
        { HKEY_CURRENT_USER,  "SOFTWARE\\GOG.com\\Games" },
    };
    for (const auto& t : gog_trees) {
        for (REGSAM sam : sams) {
            std::string r = detail::scan_gog_games_tree(t.root, t.key, sam);
            if (!r.empty()) return r;
        }
    }

    // 2. Windows Uninstall keys (GOG Inno Setup + Retail installers)
    const TreeCheck uninst_trees[] = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (const auto& t : uninst_trees) {
        for (REGSAM sam : sams) {
            std::string r = detail::scan_uninstall_tree(t.root, t.key, sam);
            if (!r.empty()) return r;
        }
    }

    // 3. Steam SteamPath + libraryfolders.vdf
    {
        std::string r = detail::scan_steam_installs();
        if (!r.empty()) return r;
    }

    // 4. Common GOG / Steam / Retail paths across drives C: through H:
    const char* rel_paths[] = {
        "GOG Games\\Falcon Gold",
        "Program Files (x86)\\GOG Galaxy\\Games\\Falcon Gold",
        "Program Files\\GOG Galaxy\\Games\\Falcon Gold",
        "Program Files (x86)\\Steam\\steamapps\\common\\Falcon Gold",
        "Program Files (x86)\\Steam\\steamapps\\common\\Falcon 3.0",
        "Games\\Falcon Gold",
        "Games\\GOG\\Falcon Gold",
        "FALCON3",
        "Falcon Gold",
        "MicroProse\\Falcon Gold",
        "Spectrum HoloByte\\Falcon 3.0",
    };
    for (char drv = 'C'; drv <= 'H'; ++drv) {
        for (const char* rel : rel_paths) {
            std::string cand = std::string(1, drv) + ":\\" + rel;
            std::string r = resolve_falcon_game_dir(cand);
            if (!r.empty()) return r;
        }
    }
#endif
    return "";
}

inline std::string find_quickstart_keys_png(const std::string& game_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir = get_executable_dir();
    fs::path cwd = fs::current_path(ec);
    fs::path gd(game_dir);

    const std::vector<fs::path> candidates = {
        exe_dir / "docs" / "Falcon30_Quickstart_Keys.png",
        exe_dir / "Falcon30_Quickstart_Keys.png",
        exe_dir.parent_path() / "docs" / "Falcon30_Quickstart_Keys.png",
        exe_dir.parent_path().parent_path() / "docs" / "Falcon30_Quickstart_Keys.png",
        cwd / "docs" / "Falcon30_Quickstart_Keys.png",
        cwd / "Falcon30_Quickstart_Keys.png",
        cwd.parent_path() / "docs" / "Falcon30_Quickstart_Keys.png",
        gd / "docs" / "Falcon30_Quickstart_Keys.png",
        gd.parent_path() / "docs" / "Falcon30_Quickstart_Keys.png",
    };
    for (const auto& c : candidates) {
        if (!c.empty() && fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
            return fs::absolute(c, ec).string();
        }
    }
    return "";
}

struct DiscoveredDoc {
    std::string label;
    std::string path;
    bool is_pdf = false;
    int priority = 50;
};

inline std::vector<DiscoveredDoc> discover_falcon_docs(const std::string& game_dir) {
    namespace fs = std::filesystem;
    std::vector<DiscoveredDoc> docs;
    std::set<std::string> seen;
    std::error_code ec;

    if (game_dir.empty() || !fs::exists(game_dir, ec)) return docs;

    fs::path gd = fs::absolute(fs::path(game_dir), ec);
    std::vector<fs::path> roots;
    roots.push_back(gd);
    roots.push_back(gd / "Manuals");
    roots.push_back(gd / "manuals");
    roots.push_back(gd / "Docs");
    roots.push_back(gd / "docs");
    roots.push_back(gd / "Documentation");

    // If game_dir is FALCON3\ or game_data\, also check parent folder & its Manuals/Docs subfolders!
    std::string leaf = detail::to_lower_ascii(gd.filename().string());
    if ((leaf == "falcon3" || leaf == "game_data") && gd.has_parent_path()) {
        fs::path par = gd.parent_path();
        roots.push_back(par);
        roots.push_back(par / "Manuals");
        roots.push_back(par / "manuals");
        roots.push_back(par / "Docs");
        roots.push_back(par / "docs");
        roots.push_back(par / "Documentation");
    }

    auto consider_file = [&](const fs::path& p) {
        if (!fs::is_regular_file(p, ec)) return;
        std::string ext = detail::to_lower_ascii(p.extension().string());
        bool is_pdf = (ext == ".pdf");
        bool is_doc = (ext == ".txt" || ext == ".doc" || ext == ".htm" || ext == ".html" || ext == ".rtf");
        if (!is_pdf && !is_doc) return;

        std::string fname = p.filename().string();
        std::string low = detail::to_lower_ascii(fname);
        // Skip unrelated/generated files
        if (low.find("third_party_licenses") != std::string::npos ||
            low.find("sha256sums") != std::string::npos ||
            low.find("unins") == 0 ||
            low.find("dosbox") != std::string::npos) {
            return;
        }
        // Only include useful txt files (manuals, readmes, reference, makeboot, patch notes)
        if (!is_pdf) {
            bool interesting = (low.find("readme") != std::string::npos ||
                                low.find("manual") != std::string::npos ||
                                low.find("falcon") != std::string::npos ||
                                low.find("hornet") != std::string::npos ||
                                low.find("mig") != std::string::npos ||
                                low.find("makeboot") != std::string::npos ||
                                low.find("key") != std::string::npos ||
                                low.find("ref") != std::string::npos ||
                                low.find("guide") != std::string::npos);
            if (!interesting) return;
        }

        std::string canon = detail::to_lower_ascii(fs::weakly_canonical(p, ec).string());
        if (seen.count(canon)) return;
        seen.insert(canon);

        int prio = 50;
        std::string label = fname;
        if (is_pdf) {
            prio = (low == "manual.pdf" || low.find("manual") != std::string::npos) ? 10 : 20;
            if (low == "manual.pdf") label = "Falcon Gold Manual (PDF)";
            else label = p.stem().string() + " (PDF)";
        } else if (low.find("readme") != std::string::npos) {
            prio = 30;
            label = fname;
        } else if (low.find("makeboot") != std::string::npos) {
            prio = 40;
            label = "Original Notes (MAKEBOOT.TXT)";
        }

        if (label.size() > 30) {
            label = label.substr(0, 27) + "...";
        }
        docs.push_back({label, p.string(), is_pdf, prio});
    };

    for (const auto& dir : roots) {
        if (dir.empty() || !fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            consider_file(entry.path());
        }
    }

    std::sort(docs.begin(), docs.end(), [](const DiscoveredDoc& a, const DiscoveredDoc& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.label < b.label;
    });
    if (docs.size() > 6) docs.resize(6);
    return docs;
}

inline bool open_path_external(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    HINSTANCE res = ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(res) > 32;
#else
    return false;
#endif
}

} // namespace falcon
