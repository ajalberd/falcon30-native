#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <objbase.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define APP_TITLE       "Falcon 3.0 Gold - Native Setup"
#define APP_VERSION     "v0.3.0"
#define PAYLOAD_MAGIC   "F3NATV01"

#define IDC_PATH_EDIT   101
#define IDC_BROWSE_BTN  102
#define IDC_STATUS_TXT  103
#define IDC_CHK_SHORT   104
#define IDC_CHK_CONFIG  105
#define IDC_INSTALL_BTN 106
#define IDC_UNINST_BTN  107
#define IDC_CLOSE_BTN   108

#pragma pack(push, 1)
typedef struct {
    uint32_t offsetFromPayload;
    uint32_t size;
    uint32_t crc32;
    char     relPath[128];
} PayloadEntry;

typedef struct {
    char     magic[8];
    uint32_t payloadStartOffset;
    uint32_t entryTableOffset;
    uint32_t entryCount;
    uint32_t reserved;
} PayloadTrailer;
#pragma pack(pop)

static HWND  g_hwnd = NULL;
static HFONT g_fontBody = NULL;
static HFONT g_fontBold = NULL;
static HFONT g_fontTitle = NULL;
static char  g_selfPath[MAX_PATH] = {0};
static int   g_autoDetected = 0;

static uint32_t CalcCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static int FileExistsA(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int DirExistsA(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void EnsureParentDirs(const char* filePath) {
    char tmp[MAX_PATH];
    lstrcpynA(tmp, filePath, MAX_PATH);
    for (char* p = tmp + 3; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            if (!DirExistsA(tmp)) CreateDirectoryA(tmp, NULL);
            *p = saved;
        }
    }
}

static void TrimTrailingSlashes(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\\' || s[n - 1] == '/' || s[n - 1] == ' ' || s[n - 1] == '"')) {
        s[--n] = '\0';
    }
}

static int ContainsCaseInsensitive(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return 0;
    size_t nlen = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static int DirHasAnyFalconExe(const char* dir) {
    if (!dir || !dir[0]) return 0;
    const char* exes[] = { "FALCONCD.EXE", "HORNETCD.EXE", "MIGCD.EXE" };
    char p[MAX_PATH];
    for (int i = 0; i < 3; ++i) {
        snprintf(p, sizeof(p), "%s\\%s", dir, exes[i]);
        if (FileExistsA(p)) return 1;
    }
    return 0;
}

// Checks whether <dir> or <dir>\FALCON3 contains FALCONCD.EXE / HORNETCD.EXE / MIGCD.EXE.
// If outResolved is non-NULL, writes the exact directory containing the game EXEs.
static int ResolveFalconDir(const char* dir, char* outResolved, size_t outCap) {
    if (!dir || !dir[0]) return 0;
    char clean[MAX_PATH];
    lstrcpynA(clean, dir, MAX_PATH);
    TrimTrailingSlashes(clean);
    if (!clean[0]) return 0;

    if (DirHasAnyFalconExe(clean)) {
        if (outResolved && outCap > 0) lstrcpynA(outResolved, clean, (int)outCap);
        return 1;
    }
    char sub[MAX_PATH];
    snprintf(sub, sizeof(sub), "%s\\FALCON3", clean);
    if (DirHasAnyFalconExe(sub)) {
        if (outResolved && outCap > 0) lstrcpynA(outResolved, sub, (int)outCap);
        return 1;
    }
    return 0;
}

static int ReadRegSz(HKEY hk, const char* valName, char* out, size_t cap) {
    char buf[1024] = {0};
    DWORD cb = sizeof(buf) - 1;
    DWORD type = 0;
    if (RegQueryValueExA(hk, valName, NULL, &type, (LPBYTE)buf, &cb) != ERROR_SUCCESS) return 0;
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || !buf[0]) return 0;
    TrimTrailingSlashes(buf);
    if (!buf[0]) return 0;
    lstrcpynA(out, buf, (int)cap);
    return 1;
}

static int ScanGogGamesTree(HKEY root, const char* baseKey, REGSAM sam, char* outDir, size_t outCap) {
    HKEY hkBase = NULL;
    if (RegOpenKeyExA(root, baseKey, 0, KEY_READ | sam, &hkBase) != ERROR_SUCCESS) return 0;

    const char* vals[] = { "path", "workingDir", "PATH" };

    // Fast path: GOG Falcon Gold ID 1443160656
    {
        HKEY hkFg = NULL;
        if (RegOpenKeyExA(hkBase, "1443160656", 0, KEY_READ | sam, &hkFg) == ERROR_SUCCESS) {
            for (int v = 0; v < 3; ++v) {
                char cand[MAX_PATH] = {0};
                if (ReadRegSz(hkFg, vals[v], cand, sizeof(cand)) && ResolveFalconDir(cand, NULL, 0)) {
                    lstrcpynA(outDir, cand, (int)outCap);
                    RegCloseKey(hkFg);
                    RegCloseKey(hkBase);
                    return 1;
                }
            }
            RegCloseKey(hkFg);
        }
    }

    // Enumerate ALL GOG.com\Games\* subkeys
    char subName[256];
    for (DWORD idx = 0;; ++idx) {
        DWORD nameLen = sizeof(subName);
        if (RegEnumKeyExA(hkBase, idx, subName, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hkSub = NULL;
        if (RegOpenKeyExA(hkBase, subName, 0, KEY_READ | sam, &hkSub) != ERROR_SUCCESS) continue;
        for (int v = 0; v < 3; ++v) {
            char cand[MAX_PATH] = {0};
            if (ReadRegSz(hkSub, vals[v], cand, sizeof(cand)) && ResolveFalconDir(cand, NULL, 0)) {
                lstrcpynA(outDir, cand, (int)outCap);
                RegCloseKey(hkSub);
                RegCloseKey(hkBase);
                return 1;
            }
        }
        RegCloseKey(hkSub);
    }

    RegCloseKey(hkBase);
    return 0;
}

static int ScanUninstallTree(HKEY root, const char* baseKey, REGSAM sam, char* outDir, size_t outCap) {
    HKEY hkBase = NULL;
    if (RegOpenKeyExA(root, baseKey, 0, KEY_READ | sam, &hkBase) != ERROR_SUCCESS) return 0;

    const char* vals[] = { "InstallLocation", "Inno Setup: App Path" };

    // Fast path: GOG Falcon Gold Inno Setup uninstall key
    {
        HKEY hkIs = NULL;
        if (RegOpenKeyExA(hkBase, "1443160656_is1", 0, KEY_READ | sam, &hkIs) == ERROR_SUCCESS) {
            for (int v = 0; v < 2; ++v) {
                char cand[MAX_PATH] = {0};
                if (ReadRegSz(hkIs, vals[v], cand, sizeof(cand)) && ResolveFalconDir(cand, NULL, 0)) {
                    lstrcpynA(outDir, cand, (int)outCap);
                    RegCloseKey(hkIs);
                    RegCloseKey(hkBase);
                    return 1;
                }
            }
            RegCloseKey(hkIs);
        }
    }

    // Enumerate Uninstall entries mentioning "Falcon"
    char subName[256];
    for (DWORD idx = 0;; ++idx) {
        DWORD nameLen = sizeof(subName);
        if (RegEnumKeyExA(hkBase, idx, subName, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hkSub = NULL;
        if (RegOpenKeyExA(hkBase, subName, 0, KEY_READ | sam, &hkSub) != ERROR_SUCCESS) continue;
        char disp[256] = {0};
        if (ReadRegSz(hkSub, "DisplayName", disp, sizeof(disp)) && ContainsCaseInsensitive(disp, "falcon")) {
            for (int v = 0; v < 2; ++v) {
                char cand[MAX_PATH] = {0};
                if (ReadRegSz(hkSub, vals[v], cand, sizeof(cand)) && ResolveFalconDir(cand, NULL, 0)) {
                    lstrcpynA(outDir, cand, (int)outCap);
                    RegCloseKey(hkSub);
                    RegCloseKey(hkBase);
                    return 1;
                }
            }
        }
        RegCloseKey(hkSub);
    }

    RegCloseKey(hkBase);
    return 0;
}

static int CheckSteamLibRoot(const char* libRoot, char* outDir, size_t outCap) {
    const char* appDirs[] = {
        "Falcon Gold",
        "Falcon 3.0",
        "Falcon 3.0 Gold",
        "Falcon Collection"
    };
    char cand[MAX_PATH];
    for (int i = 0; i < 4; ++i) {
        snprintf(cand, sizeof(cand), "%s\\steamapps\\common\\%s", libRoot, appDirs[i]);
        if (ResolveFalconDir(cand, NULL, 0)) {
            lstrcpynA(outDir, cand, (int)outCap);
            return 1;
        }
    }
    return 0;
}

static int ScanSteamInstalls(char* outDir, size_t outCap) {
    struct { HKEY root; const char* key; const char* val; } regs[] = {
        { HKEY_CURRENT_USER,  "Software\\Valve\\Steam", "SteamPath" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath" },
    };
    const REGSAM sams[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        for (int s = 0; s < 2; ++s) {
            HKEY hk = NULL;
            if (RegOpenKeyExA(regs[i].root, regs[i].key, 0, KEY_READ | sams[s], &hk) != ERROR_SUCCESS) continue;
            char steamRoot[MAX_PATH] = {0};
            int ok = ReadRegSz(hk, regs[i].val, steamRoot, sizeof(steamRoot));
            RegCloseKey(hk);
            if (!ok) continue;

            // Normalize forward slashes from SteamPath
            for (char* p = steamRoot; *p; ++p) if (*p == '/') *p = '\\';
            if (CheckSteamLibRoot(steamRoot, outDir, outCap)) return 1;

            // Parse steamapps\libraryfolders.vdf for secondary Steam libraries
            char vdfPath[MAX_PATH];
            snprintf(vdfPath, sizeof(vdfPath), "%s\\steamapps\\libraryfolders.vdf", steamRoot);
            FILE* f = fopen(vdfPath, "r");
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    const char* pkey = strstr(line, "\"path\"");
                    if (!pkey) continue;
                    const char* q1 = strchr(pkey + 6, '"');
                    if (!q1) continue;
                    ++q1;
                    const char* q2 = strchr(q1, '"');
                    if (!q2) continue;
                    char lib[MAX_PATH];
                    size_t dst = 0;
                    for (const char* p = q1; p < q2 && dst + 1 < sizeof(lib); ++p) {
                        if (*p == '\\' && (p + 1) < q2 && p[1] == '\\') {
                            lib[dst++] = '\\';
                            ++p;
                        } else {
                            lib[dst++] = *p;
                        }
                    }
                    lib[dst] = '\0';
                    TrimTrailingSlashes(lib);
                    if (lib[0] && CheckSteamLibRoot(lib, outDir, outCap)) {
                        fclose(f);
                        return 1;
                    }
                }
                fclose(f);
            }
        }
    }
    return 0;
}

static void DetectGameDir(char* outDir, size_t outCap) {
    outDir[0] = '\0';
    g_autoDetected = 0;

    // 1. Check same folder as installer
    char selfDir[MAX_PATH];
    lstrcpynA(selfDir, g_selfPath, MAX_PATH);
    char* slash = strrchr(selfDir, '\\');
    if (slash) {
        *slash = '\0';
        if (ResolveFalconDir(selfDir, NULL, 0)) {
            lstrcpynA(outDir, selfDir, (int)outCap);
            g_autoDetected = 1;
            return;
        }
    }

    const REGSAM sams[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

    // 2. Enumerate GOG.com\Games\* (both 64-bit & 32-bit views, HKLM & HKCU)
    struct { HKEY root; const char* key; } gogTrees[] = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\GOG.com\\Games" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\GOG.com\\Games" },
        { HKEY_CURRENT_USER,  "SOFTWARE\\GOG.com\\Games" },
    };
    for (size_t i = 0; i < sizeof(gogTrees) / sizeof(gogTrees[0]); ++i) {
        for (int s = 0; s < 2; ++s) {
            if (ScanGogGamesTree(gogTrees[i].root, gogTrees[i].key, sams[s], outDir, outCap)) {
                g_autoDetected = 1;
                return;
            }
        }
    }

    // 3. Windows Uninstall keys (GOG Inno Setup + Retail)
    struct { HKEY root; const char* key; } uninstTrees[] = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (size_t i = 0; i < sizeof(uninstTrees) / sizeof(uninstTrees[0]); ++i) {
        for (int s = 0; s < 2; ++s) {
            if (ScanUninstallTree(uninstTrees[i].root, uninstTrees[i].key, sams[s], outDir, outCap)) {
                g_autoDetected = 1;
                return;
            }
        }
    }

    // 4. Steam SteamPath & libraryfolders.vdf
    if (ScanSteamInstalls(outDir, outCap)) {
        g_autoDetected = 1;
        return;
    }

    // 5. Scan common GOG / Steam / Retail paths across drives C: through H:
    const char* relPaths[] = {
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
        for (size_t i = 0; i < sizeof(relPaths) / sizeof(relPaths[0]); ++i) {
            char cand[MAX_PATH];
            snprintf(cand, sizeof(cand), "%c:\\%s", drv, relPaths[i]);
            if (ResolveFalconDir(cand, NULL, 0)) {
                lstrcpynA(outDir, cand, (int)outCap);
                g_autoDetected = 1;
                return;
            }
        }
    }
}

static int HasMt32RomsIn(const char* dir) {
    if (!dir || !dir[0]) return 0;
    char p1[MAX_PATH], p2[MAX_PATH], p3[MAX_PATH], p4[MAX_PATH];
    snprintf(p1, sizeof(p1), "%s\\CM32L_CONTROL.ROM", dir);
    snprintf(p2, sizeof(p2), "%s\\CM32L_PCM.ROM", dir);
    if (FileExistsA(p1) && FileExistsA(p2)) return 1;
    snprintf(p3, sizeof(p3), "%s\\MT32_CONTROL.ROM", dir);
    snprintf(p4, sizeof(p4), "%s\\MT32_PCM.ROM", dir);
    return (FileExistsA(p3) && FileExistsA(p4));
}

static void DetectRomDir(const char* installDir, char* outDir, size_t outCap) {
    char cand[MAX_PATH];
    snprintf(cand, sizeof(cand), "%s\\roms", installDir);
    if (HasMt32RomsIn(cand)) { lstrcpynA(outDir, cand, (int)outCap); return; }

    const char* env1 = getenv("MT32_ROM_DIR");
    if (env1 && HasMt32RomsIn(env1)) { lstrcpynA(outDir, env1, (int)outCap); return; }

    const char* appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        snprintf(cand, sizeof(cand), "%s\\munt\\roms", appdata);
        if (HasMt32RomsIn(cand)) { lstrcpynA(outDir, cand, (int)outCap); return; }
    }
    if (HasMt32RomsIn("C:\\munt\\roms")) { lstrcpynA(outDir, "C:\\munt\\roms", (int)outCap); return; }
    if (HasMt32RomsIn("C:\\mt32-roms"))  { lstrcpynA(outDir, "C:\\mt32-roms", (int)outCap);  return; }
    lstrcpynA(outDir, "roms", (int)outCap);
}

static void WriteJsonEscaped(FILE* f, const char* s) {
    fputc('"', f);
    for (const char* p = s; p && *p; ++p) {
        if (*p == '\\' || *p == '"') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void WriteDefaultConfigJson(const char* installDir, const char* resolvedGameDir) {
    char cfgPath[MAX_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s\\falcon3_config.json", installDir);
    if (FileExistsA(cfgPath)) return; // Preserve existing user settings

    char romDir[MAX_PATH] = {0};
    DetectRomDir(installDir, romDir, sizeof(romDir));

    FILE* f = fopen(cfgPath, "w");
    if (!f) return;
    fprintf(f, "{\n  \"paths\": {\n    \"game_dir\": ");
    WriteJsonEscaped(f, resolvedGameDir);
    fprintf(f, ",\n    \"cm32l_rom_dir\": ");
    WriteJsonEscaped(f, romDir);
    fprintf(f,
            "\n  },\n"
            "  \"audio\": {\n"
            "    \"cm32l_rom_dir\": ");
    WriteJsonEscaped(f, romDir);
    fprintf(f,
            "\n  },\n"
            "  \"host\": {\n"
            "    \"show_launcher_on_startup\": true,\n"
            "    \"skip_intro\": true,\n"
            "    \"last_exe\": \"FALCONCD.EXE\"\n"
            "  }\n"
            "}\n");
    fclose(f);
}

static int GetDesktopShortcutPath(char* outPath, size_t cap) {
    char desk[MAX_PATH] = {0};
    if (SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk) != S_OK) return 0;
    snprintf(outPath, cap, "%s\\Falcon 3.0 Gold [Native].lnk", desk);
    return 1;
}

static int CreateDesktopShortcut(const char* installDir) {
    char lnkPath[MAX_PATH];
    if (!GetDesktopShortcutPath(lnkPath, sizeof(lnkPath))) return 0;

    char targetExe[MAX_PATH];
    snprintf(targetExe, sizeof(targetExe), "%s\\falcon30_native.exe", installDir);

    HRESULT hr = CoInitialize(NULL);
    IShellLinkA* psl = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void**)&psl))) {
        IShellLinkA_SetPath(psl, targetExe);
        IShellLinkA_SetWorkingDirectory(psl, installDir);
        IShellLinkA_SetDescription(psl, "Falcon 3.0 Gold Native Host (SDL3/OpenGL 3D + CM-32L)");
        IPersistFile* ppf = NULL;
        if (SUCCEEDED(IShellLinkA_QueryInterface(psl, &IID_IPersistFile, (void**)&ppf))) {
            WCHAR wpath[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, lnkPath, -1, wpath, MAX_PATH);
            if (SUCCEEDED(IPersistFile_Save(ppf, wpath, TRUE))) ok = 1;
            IPersistFile_Release(ppf);
        }
        IShellLinkA_Release(psl);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
}

static int DoInstall(const char* installDir, int doShortcut, int doWriteConfig, char* errMsg, size_t errCap) {
    char resolvedGameDir[MAX_PATH] = {0};
    if (!ResolveFalconDir(installDir, resolvedGameDir, sizeof(resolvedGameDir))) {
        snprintf(errMsg, errCap,
                 "FALCONCD.EXE (or HORNETCD.EXE / MIGCD.EXE) was not found in:\n%s\n(or its FALCON3 subfolder).\n\n"
                 "Please select your Falcon Gold installation folder.",
                 installDir);
        return 0;
    }

    HANDLE hSelf = CreateFileA(g_selfPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSelf == INVALID_HANDLE_VALUE) {
        snprintf(errMsg, errCap, "Could not open installer executable to read payload.");
        return 0;
    }
    DWORD fileSize = GetFileSize(hSelf, NULL);
    if (fileSize < sizeof(PayloadTrailer)) {
        CloseHandle(hSelf);
        snprintf(errMsg, errCap, "Installer payload is missing or corrupted.");
        return 0;
    }

    PayloadTrailer tr;
    SetFilePointer(hSelf, (LONG)(fileSize - sizeof(PayloadTrailer)), NULL, FILE_BEGIN);
    DWORD rd = 0;
    if (!ReadFile(hSelf, &tr, sizeof(tr), &rd, NULL) || rd != sizeof(tr) ||
        memcmp(tr.magic, PAYLOAD_MAGIC, 8) != 0 || tr.entryCount == 0 || tr.entryCount > 64) {
        CloseHandle(hSelf);
        snprintf(errMsg, errCap, "Invalid installer payload signature. Please re-download the release binary.");
        return 0;
    }

    PayloadEntry* entries = (PayloadEntry*)malloc(tr.entryCount * sizeof(PayloadEntry));
    if (!entries) {
        CloseHandle(hSelf);
        snprintf(errMsg, errCap, "Out of memory.");
        return 0;
    }
    SetFilePointer(hSelf, (LONG)tr.entryTableOffset, NULL, FILE_BEGIN);
    if (!ReadFile(hSelf, entries, tr.entryCount * sizeof(PayloadEntry), &rd, NULL) ||
        rd != tr.entryCount * sizeof(PayloadEntry)) {
        free(entries);
        CloseHandle(hSelf);
        snprintf(errMsg, errCap, "Failed to read embedded file table.");
        return 0;
    }

    for (uint32_t i = 0; i < tr.entryCount; ++i) {
        uint8_t* buf = (uint8_t*)malloc(entries[i].size > 0 ? entries[i].size : 1);
        if (!buf) {
            free(entries);
            CloseHandle(hSelf);
            snprintf(errMsg, errCap, "Out of memory extracting %s.", entries[i].relPath);
            return 0;
        }
        SetFilePointer(hSelf, (LONG)(tr.payloadStartOffset + entries[i].offsetFromPayload), NULL, FILE_BEGIN);
        if (entries[i].size > 0 && (!ReadFile(hSelf, buf, entries[i].size, &rd, NULL) || rd != entries[i].size)) {
            free(buf);
            free(entries);
            CloseHandle(hSelf);
            snprintf(errMsg, errCap, "Read error extracting %s.", entries[i].relPath);
            return 0;
        }
        if (CalcCrc32(buf, entries[i].size) != entries[i].crc32) {
            free(buf);
            free(entries);
            CloseHandle(hSelf);
            snprintf(errMsg, errCap, "CRC checksum mismatch on %s. Download may be corrupted.", entries[i].relPath);
            return 0;
        }

        char dest[MAX_PATH];
        snprintf(dest, sizeof(dest), "%s\\%s", installDir, entries[i].relPath);
        EnsureParentDirs(dest);

        HANDLE hOut = CreateFileA(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOut == INVALID_HANDLE_VALUE) {
            DWORD gle = GetLastError();
            free(buf);
            free(entries);
            CloseHandle(hSelf);
            snprintf(errMsg, errCap,
                     "Failed to write:\n%s\n\nError code: %lu\n"
                     "(Close Falcon 3.0 Native if it is running, or run installer as Administrator if installed in Program Files.)",
                     dest, (unsigned long)gle);
            return 0;
        }
        DWORD wr = 0;
        BOOL ok = (entries[i].size == 0) || (WriteFile(hOut, buf, entries[i].size, &wr, NULL) && wr == entries[i].size);
        CloseHandle(hOut);
        free(buf);
        if (!ok) {
            free(entries);
            CloseHandle(hSelf);
            snprintf(errMsg, errCap, "Disk write failed on %s.", dest);
            return 0;
        }
    }

    free(entries);
    CloseHandle(hSelf);

    if (doWriteConfig) {
        WriteDefaultConfigJson(installDir, resolvedGameDir);
    }
    if (doShortcut) {
        CreateDesktopShortcut(installDir);
    }

    return 1;
}

static int DoUninstall(const char* installDir, char* errMsg, size_t errCap) {
    if (!ResolveFalconDir(installDir, NULL, 0)) {
        snprintf(errMsg, errCap, "Falcon Gold game files were not found in:\n%s", installDir);
        return 0;
    }

    const char* removeFiles[] = {
        "falcon30_native.exe",
        "SDL3.dll",
        "THIRD_PARTY_LICENSES.txt",
        "LICENSE_NATIVE.txt",
        "README_NATIVE.md",
        "docs\\Falcon30_Quickstart_Keys.png"
    };
    for (size_t i = 0; i < sizeof(removeFiles) / sizeof(removeFiles[0]); ++i) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\%s", installDir, removeFiles[i]);
        if (FileExistsA(p)) DeleteFileA(p);
    }

    char docsDir[MAX_PATH];
    snprintf(docsDir, sizeof(docsDir), "%s\\docs", installDir);
    RemoveDirectoryA(docsDir); // Succeeds only if empty

    char lnkPath[MAX_PATH];
    if (GetDesktopShortcutPath(lnkPath, sizeof(lnkPath)) && FileExistsA(lnkPath)) {
        DeleteFileA(lnkPath);
    }
    return 1;
}

static void RefreshPathStatus(HWND hwnd) {
    char dir[MAX_PATH] = {0};
    GetDlgItemTextA(hwnd, IDC_PATH_EDIT, dir, MAX_PATH);
    TrimTrailingSlashes(dir);
    int ok = ResolveFalconDir(dir, NULL, 0);
    SetDlgItemTextA(hwnd, IDC_STATUS_TXT,
                    ok ? (g_autoDetected
                              ? "Status: Auto-detected Falcon Gold (GOG/Steam/Retail) - ready for 1-click install!"
                              : "Status: Found Falcon Gold game files - ready for 1-click installation!")
                       : "Status: Falcon Gold not auto-detected - click Browse... to select your game folder.");
    EnableWindow(GetDlgItem(hwnd, IDC_INSTALL_BTN), ok);
    EnableWindow(GetDlgItem(hwnd, IDC_UNINST_BTN), ok);
}

static void BrowseForFolder(HWND hwnd) {
    BROWSEINFOA bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select your Falcon Gold folder (containing FALCONCD.EXE or FALCON3\\):";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        char path[MAX_PATH] = {0};
        if (SHGetPathFromIDListA(pidl, path)) {
            g_autoDetected = 0;
            SetDlgItemTextA(hwnd, IDC_PATH_EDIT, path);
            RefreshPathStatus(hwnd);
        }
        CoTaskMemFree(pidl);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_fontTitle = CreateFontA(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            g_fontBold  = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            g_fontBody  = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, "Segoe UI");

            HWND h;
            h = CreateWindowA("STATIC", "Falcon 3.0 Gold - Native Host " APP_VERSION,
                              WS_CHILD | WS_VISIBLE, 20, 16, 520, 28, hwnd, NULL, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);

            h = CreateWindowA("STATIC",
                              "100% Native SDL3 + OpenGL 3D Trilogy Host (F-16C / F/A-18C / MiG-29A).\n"
                              "360-deg Virtual Cockpit, 80387 Hi-Fi FM, CM-32L Audio, HOTAS & OpenTrack - no DOSBox.",
                              WS_CHILD | WS_VISIBLE, 20, 48, 520, 36, hwnd, NULL, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);

            h = CreateWindowA("STATIC", "Falcon Gold Install Folder:",
                              WS_CHILD | WS_VISIBLE, 20, 96, 300, 20, hwnd, NULL, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

            h = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                20, 118, 420, 26, hwnd, (HMENU)IDC_PATH_EDIT, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);

            h = CreateWindowA("BUTTON", "Browse...",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              448, 117, 92, 28, hwnd, (HMENU)IDC_BROWSE_BTN, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);

            h = CreateWindowA("STATIC", "",
                              WS_CHILD | WS_VISIBLE, 20, 150, 520, 20, hwnd, (HMENU)IDC_STATUS_TXT, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

            h = CreateWindowA("BUTTON", "Create Desktop shortcut (Falcon 3.0 Gold [Native])",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              20, 186, 520, 22, hwnd, (HMENU)IDC_CHK_SHORT, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
            SendMessageA(h, BM_SETCHECK, BST_CHECKED, 0);

            h = CreateWindowA("BUTTON", "Pre-configure falcon3_config.json with detected Falcon Gold && ROM paths",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              20, 216, 520, 22, hwnd, (HMENU)IDC_CHK_CONFIG, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
            SendMessageA(h, BM_SETCHECK, BST_CHECKED, 0);

            h = CreateWindowA("BUTTON", "Install / Update",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              20, 268, 170, 36, hwnd, (HMENU)IDC_INSTALL_BTN, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

            h = CreateWindowA("BUTTON", "Uninstall Native",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              202, 268, 160, 36, hwnd, (HMENU)IDC_UNINST_BTN, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);

            h = CreateWindowA("BUTTON", "Close",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              448, 268, 92, 36, hwnd, (HMENU)IDC_CLOSE_BTN, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)g_fontBody, TRUE);

            char detected[MAX_PATH] = {0};
            DetectGameDir(detected, sizeof(detected));
            SetDlgItemTextA(hwnd, IDC_PATH_EDIT, detected);
            RefreshPathStatus(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IDC_PATH_EDIT && code == EN_CHANGE) {
                RefreshPathStatus(hwnd);
                return 0;
            }
            if (id == IDC_BROWSE_BTN) {
                BrowseForFolder(hwnd);
                return 0;
            }
            if (id == IDC_CLOSE_BTN) {
                PostQuitMessage(0);
                return 0;
            }
            if (id == IDC_INSTALL_BTN) {
                char dir[MAX_PATH] = {0}, err[512] = {0};
                GetDlgItemTextA(hwnd, IDC_PATH_EDIT, dir, MAX_PATH);
                TrimTrailingSlashes(dir);

                int doShort  = (SendDlgItemMessageA(hwnd, IDC_CHK_SHORT,  BM_GETCHECK, 0, 0) == BST_CHECKED);
                int doConfig = (SendDlgItemMessageA(hwnd, IDC_CHK_CONFIG, BM_GETCHECK, 0, 0) == BST_CHECKED);

                if (!DoInstall(dir, doShort, doConfig, err, sizeof(err))) {
                    MessageBoxA(hwnd, err, APP_TITLE, MB_OK | MB_ICONERROR);
                    return 0;
                }
                int res = MessageBoxA(hwnd,
                    "Falcon 3.0 Gold Native Host " APP_VERSION " installed successfully!\n\n"
                    "- Launch anytime via falcon30_native.exe or your Desktop shortcut.\n"
                    "- Press F12 in-flight for 3D/VR, HOTAS, Audio, Quick Keys & Manuals.\n\n"
                    "Open the Falcon 3.0 Gold Trilogy Launcher now?",
                    APP_TITLE, MB_YESNO | MB_ICONINFORMATION);
                if (res == IDYES) {
                    char exe[MAX_PATH];
                    snprintf(exe, sizeof(exe), "%s\\falcon30_native.exe", dir);
                    ShellExecuteA(NULL, "open", exe, "--launcher", dir, SW_SHOWNORMAL);
                }
                PostQuitMessage(0);
                return 0;
            }
            if (id == IDC_UNINST_BTN) {
                char dir[MAX_PATH] = {0}, err[512] = {0};
                GetDlgItemTextA(hwnd, IDC_PATH_EDIT, dir, MAX_PATH);
                TrimTrailingSlashes(dir);

                if (MessageBoxA(hwnd,
                        "Remove Falcon 3.0 Native Host files and Desktop shortcut?\n"
                        "(Your original Falcon Gold files, pilot logs, and JSON configs will be kept.)",
                        APP_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    if (!DoUninstall(dir, err, sizeof(err))) {
                        MessageBoxA(hwnd, err, APP_TITLE, MB_OK | MB_ICONERROR);
                    } else {
                        MessageBoxA(hwnd, "Falcon 3.0 Native Host uninstalled cleanly.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            if (g_fontTitle) DeleteObject(g_fontTitle);
            if (g_fontBold)  DeleteObject(g_fontBold);
            if (g_fontBody)  DeleteObject(g_fontBody);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int ParseSilentArgs(const char* cmd, int* outSilent, int* outUninstall, char* outDir, size_t cap) {
    *outSilent = 0;
    *outUninstall = 0;
    outDir[0] = '\0';
    if (!cmd || !cmd[0]) return 0;

    if (strstr(cmd, "/S") || strstr(cmd, "/s") || strstr(cmd, "--silent")) *outSilent = 1;
    if (strstr(cmd, "/U") || strstr(cmd, "/u") || strstr(cmd, "--uninstall")) *outUninstall = 1;

    const char* d = strstr(cmd, "/D=");
    if (!d) d = strstr(cmd, "/d=");
    if (d) {
        d += 3;
        int quoted = (*d == '"');
        if (quoted) ++d;
        size_t i = 0;
        while (*d && i + 1 < cap) {
            if (quoted && *d == '"') break;
            if (!quoted && (*d == ' ' || *d == '\t')) break;
            outDir[i++] = *d++;
        }
        outDir[i] = '\0';
        TrimTrailingSlashes(outDir);
    }
    return (*outSilent || *outUninstall);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShowCmd) {
    (void)hPrev; (void)nShowCmd;
    GetModuleFileNameA(NULL, g_selfPath, MAX_PATH);

    // Silent CLI mode: /S /D="C:\Path\To\Falcon Gold" or /U /D="C:\Path\To\Falcon Gold"
    int silent = 0, uninst = 0;
    char cliDir[MAX_PATH] = {0};
    if (ParseSilentArgs(lpCmdLine, &silent, &uninst, cliDir, sizeof(cliDir))) {
        if (!cliDir[0]) DetectGameDir(cliDir, sizeof(cliDir));
        char err[512] = {0};
        if (uninst) {
            return DoUninstall(cliDir, err, sizeof(err)) ? 0 : 1;
        }
        return DoInstall(cliDir, 0, 1, err, sizeof(err)) ? 0 : 1;
    }

    InitCommonControls();

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);
    wc.lpszClassName = "Falcon30NativeSetupWnd";
    RegisterClassA(&wc);

    int w = 576, h = 362;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hwnd = CreateWindowA(wc.lpszClassName, APP_TITLE " " APP_VERSION,
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           sx, sy, w, h, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
