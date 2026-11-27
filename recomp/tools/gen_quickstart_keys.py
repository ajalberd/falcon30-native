#!/usr/bin/env python3
"""
Generate a crisp, high-resolution (1920x1080) Falcon 3.0 Gold Quickstart
Keyboard & HOTAS Reference image (docs/Falcon30_Quickstart_Keys.png).
Controls verified against official Falcon 3.0 Gold Manual.pdf,
recomp/src/input/hotas_mapper.cpp, and falcon30-native host hotkeys.
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
BG = (13, 19, 27)
PANEL_BG = (19, 27, 38)
PANEL_BORDER = (48, 70, 92)

# Category palettes: (fill, border, accent_text)
CATS = {
    "view":   ((22, 46, 68),  (66, 165, 245), (125, 210, 255)),
    "weap":   ((64, 34, 24),  (239, 132, 68), (255, 192, 128)),
    "flight": ((22, 56, 36),  (74, 212, 122), (138, 245, 172)),
    "radar":  ((44, 32, 64),  (168, 125, 245),(208, 180, 255)),
    "native": ((18, 66, 48),  (60, 240, 145), (110, 255, 180)),
    "dim":    ((25, 33, 44),  (58, 72, 88),   (120, 138, 156)),
}


def load_font(size, bold=False):
    candidates = [
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/arialbd.ttf"  if bold else "C:/Windows/Fonts/arial.ttf",
    ]
    for p in candidates:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


F_TITLE  = load_font(38, bold=True)
F_SUB    = load_font(20, bold=False)
F_LEG    = load_font(18, bold=True)
F_KEY    = load_font(17, bold=True)
F_LBL    = load_font(14, bold=True)
F_SHIFT  = load_font(13, bold=False)
F_SEC    = load_font(19, bold=True)
F_BODY   = load_font(16, bold=False)
F_BODYB  = load_font(16, bold=True)


def draw_key(d, x, y, w, h, key_txt, main_lbl="", shift_lbl="", cat="dim"):
    fill, border, accent = CATS[cat]
    r = 8
    d.rounded_rectangle([x, y, x + w, y + h], radius=r, fill=fill, outline=border, width=2)
    d.text((x + 8, y + 6), key_txt, font=F_KEY, fill=(240, 246, 252) if cat != "dim" else (150, 165, 180))
    if main_lbl:
        lines = main_lbl.split("\n")
        ly = y + 29
        for ln in lines:
            d.text((x + 8, ly), ln, font=F_LBL, fill=accent)
            ly += 16
    if shift_lbl:
        d.text((x + 8, y + h - 20), shift_lbl, font=F_SHIFT, fill=(228, 212, 132))


def build_image(out_path):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # Top header banner
    d.rectangle([0, 0, W, 94], fill=(17, 26, 36))
    d.line([(0, 94), (W, 94)], fill=(58, 155, 95), width=3)
    d.text((34, 15), "FALCON 3.0 GOLD", font=F_TITLE, fill=(85, 242, 145))
    d.text((380, 15), "QUICKSTART KEYBOARD & HOTAS REFERENCE", font=F_TITLE, fill=(235, 242, 248))
    d.text((36, 61),
           "Verified against Falcon 3.0 Gold Manual + Native Host Hotkeys (F-16C / F/A-18C Hornet / MiG-29A)  |  Yellow = Shift / Alt / Mod",
           font=F_SUB, fill=(158, 178, 198))

    # Category legend pills
    legend = [
        ("VIEWS & PADLOCK",    "view"),
        ("WEAPONS & CM",       "weap"),
        ("FLIGHT & THROTTLE",  "flight"),
        ("RADAR & AVIONICS",   "radar"),
        ("NATIVE HOST HOTKEYS","native"),
    ]
    lx = 36
    ly = 110
    for text, cat in legend:
        fill, border, accent = CATS[cat]
        pw = int(d.textlength(text, font=F_LEG)) + 34
        d.rounded_rectangle([lx, ly, lx + pw, ly + 32], radius=6, fill=fill, outline=border, width=2)
        d.ellipse([lx + 10, ly + 10, lx + 22, ly + 22], fill=border)
        d.text((lx + 28, ly + 5), text, font=F_LEG, fill=accent)
        lx += pw + 14

    # Main keyboard frame
    kb_x, kb_y = 34, 156
    kb_w, kb_h = W - 68, 608
    d.rounded_rectangle([kb_x, kb_y, kb_x + kb_w, kb_y + kb_h], radius=12, fill=PANEL_BG, outline=PANEL_BORDER, width=2)

    u = 90    # base key unit width
    gap = 7
    kh = 84   # key height
    sx = kb_x + 22  # 56

    # --- ROW 0: Function Keys ---
    ry = kb_y + 18
    f_row = [
        ("ESC", "Menu /\nExit Flt",   "",           "dim",    0.95),
        (None,  "",                   "",           "dim",    0.25),
        ("F1",  "Comms /\nWingman",   "",           "radar",  1.0),
        ("F2",  "Comms /\nElement",   "",           "radar",  1.0),
        ("F3",  "Comms /\nFlight",    "",           "radar",  1.0),
        ("F4",  "Comms /\nAWACS",     "",           "radar",  1.0),
        (None,  "",                   "",           "dim",    0.18),
        ("F5",  "Radar\nNAM Mode",    "",           "radar",  1.0),
        ("F6",  "Radar\nACM Mode",    "",           "radar",  1.0),
        ("F7",  "Radar\nGnd Map",     "",           "radar",  1.0),
        ("F8",  "Radar\nRange",       "",           "radar",  1.0),
        (None,  "",                   "",           "dim",    0.18),
        ("F9",  "Recenter\nHead/IR",  "DOS:Azim",   "native", 1.0),
        ("F10", "Radar\nElev",        "",           "radar",  1.0),
        ("F11", "Cycle 3D\nRender",   "",           "native", 1.0),
        ("F12", "Native\nOverlay",    "",           "native", 1.0),
    ]
    cx = sx
    for k, m, sh, c, mult in f_row:
        kw = int(u * mult)
        if k is not None:
            draw_key(d, cx, ry, kw, kh, k, m, sh, c)
        cx += kw + gap

    # --- ROW 1: Number Row (Views + Throttle + A-G Weapon) ---
    ry += kh + 14
    num_row = [
        ("`",   "",            "",          "dim",    0.85),
        ("1",   "Satellite\nView","",       "view",   1.0),
        ("2",   "Ground\nView","",          "view",   1.0),
        ("3",   "Forward\nCockpit","Recenter","native",1.0),
        ("4",   "Left\nCockpit","",         "view",   1.0),
        ("5",   "Rear\nCockpit","",         "view",   1.0),
        ("6",   "Right\nCockpit","",        "view",   1.0),
        ("7",   "Wingman\nView","",         "view",   1.0),
        ("8",   "3D\nPadlock", "S:Prev",    "native", 1.0),
        ("9",   "Track /\nChase","",        "view",   1.0),
        ("0",   "Orbit /\nEnemy","",        "view",   1.0),
        ("-",   "Throttle\nDown","",        "flight", 1.0),
        ("=",   "Throttle\nUp (+)","",      "flight", 1.0),
        ("BKSP","Cycle A-G\nWeapon","",     "weap",   1.85),
    ]
    cx = sx
    for k, m, sh, c, mult in num_row:
        kw = int(u * mult)
        draw_key(d, cx, ry, kw, kh, k, m, sh, c)
        cx += kw + gap

    # --- ROW 2: QWERTY ---
    ry += kh + 10
    q_row = [
        ("TAB", "Time Comp\n1x-8x", "S:Dn C:DOS","native", 1.35),
        ("Q",   "Target /\nMissile","",          "view",   1.0),
        ("W",   "Wheel\nBrakes",   "",           "flight", 1.0),
        ("E",   "ECM\nJammer",     "",           "weap",   1.0),
        ("R",   "Radar\nOn / Off", "",           "radar",  1.0),
        ("T",   "Cycle Rdr\nTarget","",          "radar",  1.0),
        ("Y",   "",                "",           "dim",    1.0),
        ("U",   "",                "",           "dim",    1.0),
        ("I",   "",                "",           "dim",    1.0),
        ("O",   "",                "",           "dim",    1.0),
        ("P",   "Pause\nSim",      "",           "dim",    1.0),
        ("[",   "Rudder\nLeft",    "",           "flight", 1.0),
        ("]",   "Rudder\nRight",   "",           "flight", 1.0),
        ("\\",  "ILS Mode\nToggle","",           "radar",  1.35),
    ]
    cx = sx
    for k, m, sh, c, mult in q_row:
        kw = int(u * mult)
        draw_key(d, cx, ry, kw, kh, k, m, sh, c)
        cx += kw + gap

    # --- ROW 3: ASDF ---
    ry += kh + 10
    a_row = [
        ("CAPS","",            "",            "dim",    1.60),
        ("A",   "Autopilot\nToggle","",       "flight", 1.0),
        ("S",   "",            "",            "dim",    1.0),
        ("D",   "",            "",            "dim",    1.0),
        ("F",   "",            "Alt:FM Mod",  "flight", 1.0),
        ("G",   "Landing\nGear","",           "flight", 1.0),
        ("H",   "HUD\nMode",   "",            "radar",  1.0),
        ("J",   "Jettison\nStores","",        "weap",   1.0),
        ("K",   "",            "",            "dim",    1.0),
        ("L",   "",            "",            "dim",    1.0),
        (";",   "",            "",            "dim",    1.0),
        ("'",   "",            "",            "dim",    1.0),
        ("ENTER","Cycle A-A\nWeapon","",      "weap",   2.18),
    ]
    cx = sx
    for k, m, sh, c, mult in a_row:
        kw = int(u * mult)
        draw_key(d, cx, ry, kw, kh, k, m, sh, c)
        cx += kw + gap

    # --- ROW 4: ZXCV ---
    ry += kh + 10
    z_row = [
        ("SHIFT","Modifier\n(Yellow)","",     "dim",    2.08),
        ("Z",   "Radar\nLock STT","",         "radar",  1.0),
        ("X",   "Clear\nRdr Lock","",         "radar",  1.0),
        ("C",   "REO Map /\nRadar","",        "radar",  1.0),
        ("V",   "",            "",            "dim",    1.0),
        ("B",   "Speed\nBrakes","",           "flight", 1.0),
        ("N",   "",            "",            "dim",    1.0),
        ("M",   "Maverick /\nMap","",         "radar",  1.0),
        (",",   "AB Stage\nDown","",          "flight", 1.0),
        (".",   "AB Stage\nUp","",            "flight", 1.0),
        ("/",   "AB\nToggle",  "",            "flight", 1.0),
        ("SHIFT","Modifier",   "",            "dim",    2.78),
    ]
    cx = sx
    for k, m, sh, c, mult in z_row:
        kw = int(u * mult)
        draw_key(d, cx, ry, kw, kh, k, m, sh, c)
        cx += kw + gap

    # --- ROW 5: Bottom Row (aligned flush with right edge = 1469) ---
    ry += kh + 10
    draw_key(d, sx, ry, int(u * 1.40), kh, "CTRL", "Ctrl+TAB\nDOS Warp", "", "dim")
    x_alt1 = sx + int(u * 1.40) + gap
    draw_key(d, x_alt1, ry, int(u * 1.40), kh, "ALT", "Alt+F\nFlight Mdl", "", "flight")
    sp_x = x_alt1 + int(u * 1.40) + gap
    sp_w = int(u * 8.68)
    draw_key(d, sp_x, ry, sp_w, kh, "SPACE",
             "FIRE SELECTED WEAPON (Tap = 1 Missile / Bomb   |   Hold = M61A1 Vulcan Cannon)",
             "Auto-drops Time Compression to 1x when weapons fire  |  Zero missile ripple salvo", "weap")
    x_alt2 = sp_x + sp_w + gap
    draw_key(d, x_alt2, ry, int(u * 1.65), kh, "ALT", "Modifier", "", "dim")
    x_ctrl2 = x_alt2 + int(u * 1.65) + gap
    ctrl2_w = (sx + int(u * 14.70) + 13 * gap) - x_ctrl2
    draw_key(d, x_ctrl2, ry, ctrl2_w, kh, "CTRL", "Ctrl+E\nEject!", "", "weap")

    # --- Right Column (x = 1498 .. 1866, width = 368px) ---
    rx = kb_x + kb_w - 390  # 1496
    rw = 368

    # Top of right column: INS (Chaff) & DEL (Flare) aligned with F-row
    cm_w = (rw - 10) // 2
    draw_key(d, rx,              kb_y + 18, cm_w, kh, "INS", "Dispense\nChaff", "Radar CM", "weap")
    draw_key(d, rx + cm_w + 10,  kb_y + 18, cm_w, kh, "DEL", "Dispense\nFlare", "IR CM",    "weap")

    # Middle of right column: 3D Cockpit, Mouse & OpenTrack panel
    ay = ry - kh - 10
    my = kb_y + 18 + kh + 14
    mh = (ay - 12) - my
    d.rounded_rectangle([rx, my, rx + rw, my + mh], radius=10, fill=(22, 34, 48), outline=(78, 175, 128), width=2)
    d.text((rx + 14, my + 12), "3D COCKPIT, MOUSE & HOTAS", font=F_SEC, fill=(100, 245, 155))
    d.line([(rx + 14, my + 40), (rx + rw - 14, my + 40)], fill=(50, 85, 70), width=1)
    mouse_lines = [
        ("Right-Mouse Drag:", "360-deg Virtual Cockpit Free-Look"),
        ("Middle-Click / '3':", "Recenter Head & Exit Padlock"),
        ("8 / Shift+8:",       "Unclamped 3D Padlock + X-Ray Box"),
        ("OpenTrack UDP:",     "Port 4242 TrackIR (F9 to Recenter)"),
        ("Seamless HOTAS:",    "Multi-USB Stick/Thr/Pedals + Kbd"),
    ]
    mly = my + 50
    for k, v in mouse_lines:
        d.text((rx + 14, mly), k, font=F_BODYB, fill=(235, 225, 150))
        mly += 21
        d.text((rx + 22, mly), v, font=F_SHIFT, fill=(205, 220, 232))
        mly += 22

    # Bottom of right column: Arrow Flight Stick cluster
    aw = 116
    ag = 10
    draw_key(d, rx + aw + ag,       ay, aw, kh, "UP",    "Nose\nDown",   "", "flight")
    draw_key(d, rx,                 ry, aw, kh, "LEFT",  "Roll\nLeft",   "", "flight")
    draw_key(d, rx + aw + ag,       ry, aw, kh, "DOWN",  "Pull Up\n(+G)","", "flight")
    draw_key(d, rx + 2 * (aw + ag), ry, aw, kh, "RIGHT", "Roll\nRight",  "", "flight")

    # --- Bottom 3-Column Quickstart Combat Cards ---
    by = kb_y + kb_h + 18
    bh = H - by - 22
    cw = (W - 68 - 32) // 3
    cards = [
        ("1. AIR-TO-AIR COMBAT LOOP (BVR & WVR)", (239, 132, 68), [
            "Enter cycles A-A weapons (AIM-120 / AIM-9 / M61 Gun).",
            "F5 NAM / F6 ACM Radar; T cycles targets, Z locks STT.",
            "Press 8 for Unclamped 3D Padlock (Shift+8 prev, 3 fwd).",
            "Space fires (tap = single missile, hold = M61 Vulcan).",
            "Press Ins Chaff / Del Flare / E ECM against SAMs.",
        ]),
        ("2. FLIGHT, THROTTLE & AIR-TO-GROUND", (66, 165, 245), [
            "Arrows fly stick, [ / ] rudder; HOTAS engages live.",
            "+ / - Throttle; / toggles Afterburner; , / . AB1-AB5.",
            "B Airbrake, G Gear, W Brakes, A Autopilot, \\ ILS.",
            "Backspace cycles A-G bombs/AGMs; F7 Ground Radar.",
            "Hi-Fidelity 80387 FPU flight model selectable in Launcher.",
        ]),
        ("3. NATIVE HOST HOTKEYS & TRILOGY TIPS", (60, 240, 145), [
            "F12 toggles Native Overlay (3D/VR, HOTAS, Audio, MP).",
            "F11 cycles Classic VGA / Modern 3D / Hybrid / VR Stereo.",
            "TAB Smooth Time Comp (1x-8x, auto-drops on missiles).",
            "F9 recenters OpenTrack UDP (4242) & 3D free-look.",
            "Switch F-16C / F/A-18C / MiG-29A via F12 -> Title.",
        ]),
    ]
    for idx, (title, col, lines) in enumerate(cards):
        cx0 = 34 + idx * (cw + 16)
        d.rounded_rectangle([cx0, by, cx0 + cw, by + bh], radius=10, fill=PANEL_BG, outline=col, width=2)
        d.text((cx0 + 16, by + 14), title, font=F_SEC, fill=col)
        d.line([(cx0 + 16, by + 44), (cx0 + cw - 16, by + 44)], fill=PANEL_BORDER, width=1)
        ty = by + 56
        for ln in lines:
            d.text((cx0 + 16, ty), "- " + ln, font=F_BODY, fill=(212, 224, 236))
            ty += 37

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    img.save(out_path, "PNG", optimize=True)
    print(f"Generated {out_path} ({W}x{H}, {os.path.getsize(out_path):,} bytes)")


if __name__ == "__main__":
    # Place output in <repo_root>/docs/Falcon30_Quickstart_Keys.png
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(os.path.dirname(script_dir))
    build_image(os.path.join(repo_root, "docs", "Falcon30_Quickstart_Keys.png"))
