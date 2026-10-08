# Reference render for the plan's Phase 6 dry-box unit, drawn in one projection: spools
# (ui_spool_canvas geometry), box (front, left wall, floor, back wall, right side) and glass lid.
# usage: python3 unit_render.py <dark|light> <out.png> [mode: 0 no lid, 1 unit lid, 2 per-lane lids]
import sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

theme, out = sys.argv[1], sys.argv[2]
MODE = int(sys.argv[3]) if len(sys.argv) > 3 else 1   # 0 no lid, 1 one lid per unit, 2 one lid per lane
LID = MODE != 0
SS = 4
W, H = 520, 150

# --- projection: depth z maps to (K*z, -RISE*z/DZ) on screen --------------------------------
K = 0.45                      # ui_spool_canvas ELLIPSE_RATIO: flange rx = 0.45 * ry
SPOOL = 61                    # spool canvas size at 800x480
FRY = 0.42 * SPOOL            # flange vertical radius (FLANGE_RADIUS)
DZ = 2 * FRY * 1.1            # box depth: a spool's diameter plus 10%
RISE = 5.0                    # back edges sit this much higher on screen
S = K * DZ                    # projected depth, sideways
def proj(x, y, z):            # x along the row, y screen-down at the front plane, z depth 0..DZ
    return (x + K * z, y - RISE * z / DZ)

# --- layout (unscaled px) ------------------------------------------------------------------
SLOTS = [58, 155, 251, 348]   # spool row x centers (front plane), from the 800x480 AFC view
FL, FR = 10, 392              # box front face left/right
FT, FB = 101, 126             # front wall top / bottom
EB = 10                       # back wall is this much taller than the front
FLOOR_MID = FB - RISE / 2
CY = FB - FRY - 2            # spool rests on the floor (front-plane y; projected to mid-depth below)
LID_H = FT - EB / 2 - CY + FRY + 1   # crest (mid-depth) sits 1px above the spool tops

if theme == "dark":
    bg = (32, 32, 35); text = (215, 218, 224); face = (50, 50, 52); wall = (58, 58, 59)
    floor_c = (66, 66, 67); back_c = (42, 42, 45); side_c = (44, 44, 47); edge = (120, 122, 128)
    body, shade = (0x66,) * 3, (0x44,) * 3; hub_t, hub_b = (12, 12, 14), (70, 70, 74)
    shell = (58, 66, 80); glass = (200, 225, 255); hl = (255, 255, 255); gedge = (235, 242, 255)
    shell_a, glass_a, gedge_a, band_a, cap_a = 0.80, 0.06, 0.60, 0.26, 0.14
    face_a = 0.55; chip_bg = (28, 28, 31)
else:
    bg = (255, 255, 255); text = (60, 62, 68); face = (186, 186, 186); wall = (141, 141, 141)
    floor_c = (165, 165, 165); back_c = (205, 205, 207); side_c = (160, 160, 162); edge = (120, 120, 124)
    body, shade = (0x66,) * 3, (0x44,) * 3; hub_t, hub_b = (12, 12, 14), (90, 90, 94)
    shell = (226, 232, 241); glass = (70, 105, 150); hl = (255, 255, 255); gedge = (60, 90, 130)
    shell_a, glass_a, gedge_a, band_a, cap_a = 0.80, 0.05, 0.55, 0.60, 0.10
    face_a = 0.55; chip_bg = (246, 246, 248)
FIL = [(26, 26, 34), (43, 211, 209), (47, 179, 233), (222, 89, 35)]
MAT = ["PLA", "Silk PLA", "ASA", "PETG"]

img = Image.new("RGBA", (W * SS, H * SS), bg + (255,))
def P(pt): return (pt[0] * SS, pt[1] * SS)
def layer(): return Image.new("RGBA", img.size, (0, 0, 0, 0))
def comp(l):
    global img
    img = Image.alpha_composite(img, l)
def poly(pts, col, a=1.0, l=None):
    t = l or layer(); ImageDraw.Draw(t).polygon([P(p) for p in pts], fill=tuple(col) + (int(255 * a),))
    if l is None: comp(t)
def tinted(alpha_img, col, a):
    al = np.asarray(alpha_img).astype(np.float32) * a
    rgba = np.empty(al.shape + (4,), np.uint8); rgba[..., :3] = col; rgba[..., 3] = np.clip(al, 0, 255).astype(np.uint8)
    return Image.fromarray(rgba, "RGBA")
def line(pts, col, a=1.0, w=1.0, blur=0.0):
    m = Image.new("L", img.size, 0); ImageDraw.Draw(m).line([P(p) for p in pts], fill=255, width=max(1, int(w * SS)), joint="curve")
    if blur: m = m.filter(ImageFilter.GaussianBlur(blur * SS))
    comp(tinted(m, col, a))
def lighten(c, amt): return tuple(min(255, int(v + (255 - v) * amt / 255)) for v in c)
def darken(c, amt): return tuple(max(0, int(v * (255 - amt) / 255)) for v in c)
def mix(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def grad_ellipse(cx, cy, rx, ry, top, bot):
    t = layer(); d = ImageDraw.Draw(t)
    for i in range(int(2 * ry * SS) + 1):
        yy = cy - ry + i / SS
        u = (yy - cy) / ry
        if abs(u) > 1: continue
        hw = rx * math.sqrt(1 - u * u)
        c = mix(top, bot, (u + 1) / 2)
        d.line([P((cx - hw, yy)), P((cx + hw, yy))], fill=c + (255,))
    comp(t)

def spool(x, y, fil):
    ry, rx = FRY, FRY * K
    w = 0.35 * SPOOL
    lx, rxp = x - w / 2, x + w / 2
    grad_ellipse(lx, y, rx, ry, lighten(shade, 40), darken(shade, 25))          # back flange
    fry = ry * 0.85; frx = fry * K; side = darken(fil, 30)
    lt, dk = lighten(side, 70), darken(side, 35)
    grad_ellipse(lx, y, frx, fry, lt, dk)                                        # filament cylinder
    t = layer(); d = ImageDraw.Draw(t)
    for i in range(int(2 * fry * SS) + 1):
        yy = y - fry + i / SS; c = mix(lt, dk, i / (2 * fry * SS))
        d.line([P((lx, yy)), P((rxp, yy))], fill=c + (255,))
    comp(t)
    grad_ellipse(rxp, y, frx, fry, lighten(fil, 70), darken(fil, 35))
    grad_ellipse(rxp, y, rx, ry, lighten(body, 40), darken(body, 25))            # front flange
    grad_ellipse(rxp, y, 0.10 * SPOOL * K, 0.10 * SPOOL, hub_t, hub_b)           # hub hole

# Box corners. Front at z=0, back at z=DZ.
fl_t, fr_t, fl_b, fr_b = (FL, FT), (FR, FT), (FL, FB), (FR, FB)
bl_t, br_t = proj(FL, FT - EB, DZ), proj(FR, FT - EB, DZ)
bl_b, br_b = proj(FL, FB, DZ), proj(FR, FB, DZ)

# Drum lid cross-section: half-ellipse on the sloped chord from the front-wall top to the back-wall top.
def profile(x, n=72, thetas=None):
    pts = []
    for th in (thetas if thetas is not None else np.linspace(math.pi, 0, n)):
        z = DZ / 2 * (1 + math.cos(th))
        h = EB * z / DZ + LID_H * math.sin(th)
        pts.append(proj(x, FT - h, z))
    return pts
def hull(points):
    pts = sorted(set(points))
    def cr(o, a, b): return (a[0]-o[0])*(b[1]-o[1]) - (a[1]-o[1])*(b[0]-o[0])
    lo, up = [], []
    for p in pts:
        while len(lo) >= 2 and cr(lo[-2], lo[-1], p) <= 0: lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(up) >= 2 and cr(up[-2], up[-1], p) <= 0: up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]
HALF = (SLOTS[1] - SLOTS[0]) / 2 - S / 4 - 1   # per-lane lid half-width: each end cap tucks halfway behind the next lid
SEGS = [(FL, FR)] if MODE != 2 else [(x - HALF, x + HALF) for x in SLOTS]
LIDS = []
for x0, x1 in SEGS:
    cl, cr = profile(x0), profile(x1)
    LIDS.append((x0, x1, cl, cr, hull(cl + cr)))
sil = LIDS[0][4]

# 1. Back layer: back wall, floor, left wall (inside faces), lid interior shell.
poly([bl_t, br_t, br_b, bl_b], back_c)
poly([fl_b, fr_b, br_b, bl_b], floor_c)
poly([fl_t, bl_t, bl_b, fl_b], wall)
line([bl_t, br_t], edge, 0.6)
if LID:
    t = layer()
    for *_, sl in LIDS:
        ImageDraw.Draw(t).polygon([P(p) for p in sl], fill=tuple(shell) + (int(255 * shell_a),))
    comp(t)
    poly([bl_t, br_t, br_b, bl_b], back_c)
    line([bl_t, br_t], edge, 0.6)

# 2. Spools, at mid-depth.
for x, fil in zip(SLOTS, FIL):
    sx, sy = proj(x, CY, DZ / 2)
    spool(sx, sy, fil)

# 3. Front face (translucent, as the tray draws it today) and the right side face.
poly([fl_t, fr_t, fr_b, fl_b], face, face_a)
poly([fr_t, br_t, br_b, fr_b], side_c)
for a, b in [(fl_t, fr_t), (fl_b, fr_b), (fl_t, fl_b), (fr_t, fr_b), (fr_t, br_t), (fr_b, br_b), (br_t, br_b), (fl_t, bl_t), (fl_b, bl_b)]:
    line([a, b], edge, 0.55)

# 4. Badges and tool labels.
fnt = ImageFont.truetype("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", 11 * SS)
fsm = ImageFont.truetype("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", 9 * SS)
d = ImageDraw.Draw(img)
for i, x in enumerate(SLOTS):
    sx, sy = proj(x, CY, DZ / 2)
    bx, by = sx + 10, sy + FRY - 8
    d.ellipse([P((bx - 8, by - 8)), P((bx + 8, by + 8))], fill=(56, 142, 60, 255), outline=(255, 255, 255, 255), width=SS * 1)
    d.text(P((bx, by)), str(i + 1), font=fsm, fill=(20, 20, 20, 255) if theme == "light" else (235, 235, 235, 255), anchor="mm")
    tx, ty = sx - 22, sy - FRY + 8
    d.rounded_rectangle([P((tx - 9, ty - 6)), P((tx + 9, ty + 6))], radius=3 * SS, fill=(110, 110, 114, 220))
    d.text(P((tx, ty)), f"T{i}", font=fsm, fill=(235, 235, 235, 255), anchor="mm")

# 5. Glass lid front: clear skin, denser right cap, highlight band, silhouette edge, rims.
def glass_lid(x0, x1, capL, capR, sl):
    poly(sl, glass, glass_a)
    poly(capR, glass, cap_a)
    th = math.radians(112)
    pl, pr = profile(x0, thetas=[th])[0], profile(x1, thetas=[th])[0]
    span = pr[0] - pl[0]
    fin, fout = min(70, span * 0.3), min(110, span * 0.4)
    xs_ = np.arange(img.size[0]) / SS
    fade = (np.clip((xs_ - (pl[0] + min(20, span * 0.1))) / fin, 0, 1) * np.clip(((pr[0] - min(30, span * 0.1)) - xs_) / fout, 0, 1))[None, :]
    m = Image.new("L", img.size, 0)
    ImageDraw.Draw(m).line([P(pl), P(pr)], fill=255, width=int(7 * SS))
    m = m.filter(ImageFilter.GaussianBlur(5 * SS))
    comp(tinted(Image.fromarray((np.asarray(m) * fade).astype(np.uint8)), hl, band_a))
    line(sl + [sl[0]], gedge, gedge_a, 1.2, blur=0.4)
    line(capR, gedge, gedge_a * 0.7, 1.0)
    line(capL, gedge, gedge_a * 0.3, 1.0)

# 5. Glass lid(s): clear skin, denser right cap, diffuse sheen, silhouette edge, rims.
if LID:
    for lid in LIDS:
        glass_lid(*lid)

# 6. Material labels above the lid, and the readout beside the drum.
d = ImageDraw.Draw(img)
TOP = min(p[1] for p in sil) if LID else FT - EB - RISE
for x, m in zip(SLOTS, MAT):
    d.text(P((proj(x, 0, DZ / 2)[0], TOP - 10)), m, font=fnt, fill=text + (255,), anchor="mm")
LANE_RH = ["38%", "41%", "22%", "35%"]
if MODE == 2:
    for x, rh in zip(SLOTS, LANE_RH):
        d.text(P((proj(x, 0, DZ / 2)[0], TOP - 24)), rh, font=fsm, fill=(224, 160, 48, 255), anchor="mm")
else:
    rx0 = br_t[0] + 10
    d.rounded_rectangle([P((rx0, TOP - 4)), P((rx0 + 62, TOP + 54))], radius=6 * SS, fill=chip_bg + (255,))
    d.text(P((rx0 + 36, TOP + 13)), "24°C", font=fnt, fill=text + (255,), anchor="mm")
    d.text(P((rx0 + 36, TOP + 38)), "42%", font=fnt, fill=(224, 160, 48, 255), anchor="mm")
    d.ellipse([P((rx0 + 10, TOP + 10)), P((rx0 + 16, TOP + 18))], fill=(80, 140, 210, 255))
    d.ellipse([P((rx0 + 9, TOP + 33)), P((rx0 + 17, TOP + 42))], fill=(80, 140, 210, 255))


img.convert("RGB").resize((W * 2, H * 2), Image.LANCZOS).save(out)
