#!/usr/bin/env python3
"""Draws the bundled example skin ("Example - Midnight Rose") with Pillow.

    python gen_example_skin.py            # writes ../../../skins/Example - Midnight Rose/
    python gen_skin_inc.py                # then embed it into the DLL (src/ui/skin_example.inc)

The images are plain PNG/JPG files, so the skin doubles as a template: open them in any
image editor, change them, and the window reloads the skin as soon as a file is saved.
"""
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "..", "skins", "Example - Midnight Rose"))
SS = 4  # supersampling for smooth edges

ROSE = (232, 112, 168)
ROSE_LIGHT = (255, 170, 205)
ROSE_DEEP = (150, 60, 110)
NAVY = (14, 14, 28)
INK = (24, 22, 40)


def font(size, bold=True):
    names = ["DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"]
    for folder in ["/usr/share/fonts/truetype/dejavu", "C:/Windows/Fonts"]:
        for n in names + (["segoeuib.ttf"] if bold else ["segoeui.ttf"]):
            p = os.path.join(folder, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def big(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def small(img, w, h):
    return img.resize((w, h), Image.LANCZOS)


def vgrad(w, h, top, bottom):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
        for x in range(w):
            px[x, y] = c
    return img


def rounded(w, h, r, fill_top, fill_bottom, border=None, border_w=1, inner_glow=None):
    """A rounded rectangle drawn at SSx and scaled down."""
    W, H, R = w * SS, h * SS, r * SS
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, H - 1], R, fill=255)
    body = vgrad(W, H, fill_top, fill_bottom)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(body, (0, 0), mask)
    if inner_glow:
        glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle([SS * 2, SS * 1, W - 1 - SS * 2, H // 2], max(1, R - SS), fill=inner_glow)
        glow = glow.filter(ImageFilter.GaussianBlur(SS * 1.5))
        out = Image.alpha_composite(out, Image.composite(glow, Image.new("RGBA", (W, H), (0, 0, 0, 0)), mask))
    if border:
        ImageDraw.Draw(out).rounded_rectangle([0, 0, W - 1, H - 1], R, outline=border, width=border_w * SS)
    return small(out, w, h)


def save(img, name):
    path = os.path.join(OUT, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if name.endswith(".jpg"):
        img.convert("RGB").save(path, quality=84, optimize=True, progressive=True)
    else:
        img.save(path, optimize=True)


def background():
    w, h = 640, 800
    img = vgrad(w, h, (26, 18, 44, 255), (8, 8, 18, 255))
    # soft bokeh
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    import random
    rnd = random.Random(7)
    for _ in range(26):
        r = rnd.randint(18, 90)
        x, y = rnd.randint(-40, w + 40), rnd.randint(-40, h + 40)
        col = rnd.choice([ROSE, ROSE_DEEP, (110, 90, 200), (70, 60, 150)])
        d.ellipse([x - r, y - r, x + r, y + r], fill=col + (rnd.randint(18, 46),))
    glow = glow.filter(ImageFilter.GaussianBlur(22))
    img = Image.alpha_composite(img, glow)
    # a faint diagonal sheen
    sheen = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).polygon([(w * 0.55, 0), (w * 0.85, 0), (w * 0.25, h), (-w * 0.05, h)], fill=(255, 255, 255, 10))
    img = Image.alpha_composite(img, sheen.filter(ImageFilter.GaussianBlur(30)))
    save(img, "background.jpg")


def panels():
    save(rounded(32, 32, 9, (20, 18, 34, 205), (14, 13, 26, 215), border=(255, 255, 255, 34)), "panel.png")
    save(rounded(48, 26, 7, (58, 50, 84, 255), (40, 34, 60, 255), border=(255, 255, 255, 40), inner_glow=(255, 255, 255, 28)), "button.png")
    save(rounded(48, 26, 7, (150, 70, 118, 255), (104, 46, 84, 255), border=(255, 190, 220, 90), inner_glow=(255, 255, 255, 40)), "button_hover.png")
    save(rounded(48, 26, 7, (232, 112, 168, 255), (176, 70, 128, 255), border=(255, 210, 230, 140)), "button_active.png")
    save(rounded(40, 24, 11, (10, 9, 20, 235), (26, 24, 42, 235), border=(255, 255, 255, 26)), "track.png")
    save(rounded(40, 24, 11, (255, 140, 190, 170), (170, 60, 120, 170)), "fill.png")
    save(rounded(40, 30, 8, (30, 28, 46, 170), (22, 20, 36, 120), border=(255, 255, 255, 24)), "tab.png")
    save(rounded(40, 30, 8, (100, 50, 86, 220), (60, 34, 58, 170), border=(255, 180, 215, 70)), "tab_hover.png")
    save(rounded(40, 30, 8, (176, 76, 132, 255), (92, 40, 74, 235), border=(255, 200, 225, 120), inner_glow=(255, 255, 255, 36)), "tab_active.png")

    # header strip: gradient with a rose line along the bottom
    w, h = 64, 48
    img = vgrad(w, h, (40, 26, 58, 230), (20, 16, 34, 170))
    d = ImageDraw.Draw(img)
    d.line([(0, h - 2), (w, h - 2)], fill=ROSE + (200,), width=1)
    d.line([(0, h - 1), (w, h - 1)], fill=ROSE_DEEP + (120,), width=1)
    save(img, "header.png")


def grab():
    s = 40
    img = big(s, s)
    d = ImageDraw.Draw(img)
    c, r = s * SS / 2, s * SS * 0.30
    halo = big(s, s)
    ImageDraw.Draw(halo).ellipse([c - r * 1.5, c - r * 1.5, c + r * 1.5, c + r * 1.5], fill=ROSE + (110,))
    img = Image.alpha_composite(img, halo.filter(ImageFilter.GaussianBlur(SS * 3)))
    d = ImageDraw.Draw(img)
    d.ellipse([c - r, c - r, c + r, c + r], fill=(255, 236, 244, 255), outline=ROSE + (255,), width=SS * 2)
    save(small(img, s, s), "grab.png")
    img2 = Image.alpha_composite(big(s, s), halo.filter(ImageFilter.GaussianBlur(SS * 4)))
    img2 = Image.alpha_composite(img2, halo.filter(ImageFilter.GaussianBlur(SS * 2)))
    d = ImageDraw.Draw(img2)
    d.ellipse([c - r, c - r, c + r, c + r], fill=ROSE_LIGHT + (255,), outline=(255, 255, 255, 255), width=SS * 2)
    save(small(img2, s, s), "grab_active.png")


def checkboxes():
    s = 32
    save(rounded(s, s, 8, (14, 13, 26, 240), (30, 28, 46, 240), border=(255, 255, 255, 60)), "check_off.png")
    on = rounded(s, s, 8, (240, 130, 180, 255), (170, 62, 120, 255), border=(255, 210, 230, 160))
    big_on = big(s, s)
    ImageDraw.Draw(big_on).line([(9 * SS, 16.5 * SS), (14 * SS, 21.5 * SS), (23.5 * SS, 10.5 * SS)], fill=(255, 255, 255, 255), width=int(3.2 * SS), joint="curve")
    save(Image.alpha_composite(on, small(big_on, s, s)), "check_on.png")


def logo():
    text = "UUEPBS"
    f = font(64 * SS // 2)
    probe = ImageDraw.Draw(Image.new("RGBA", (10, 10)))
    x0, y0, x1, y1 = probe.textbbox((0, 0), text, font=f)
    tw, th = x1 - x0, y1 - y0
    pad = 6 * SS
    W, H = tw + pad * 2 + 20 * SS, th + pad * 2
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).text((pad + 20 * SS - x0, pad - y0), text, font=f, fill=255)
    grad = Image.new("RGBA", (W, H))
    gp = grad.load()
    for x in range(W):
        t = x / (W - 1)
        c = tuple(int(ROSE_LIGHT[i] + (ROSE[i] - ROSE_LIGHT[i]) * t) for i in range(3))
        for y in range(H):
            gp[x, y] = c + (255,)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    glow = Image.new("RGBA", (W, H), ROSE + (0,))
    glow.putalpha(mask.filter(ImageFilter.GaussianBlur(SS * 4)).point(lambda v: int(v * 0.55)))
    out = Image.alpha_composite(out, glow)
    out.paste(grad, (0, 0), mask)
    # small four-point spark in front of the text
    d = ImageDraw.Draw(out)
    cx, cy, r = pad + 8 * SS, H / 2, 9 * SS
    d.polygon([(cx, cy - r), (cx + r * 0.28, cy - r * 0.28), (cx + r, cy), (cx + r * 0.28, cy + r * 0.28), (cx, cy + r),
               (cx - r * 0.28, cy + r * 0.28), (cx - r, cy), (cx - r * 0.28, cy - r * 0.28)], fill=ROSE_LIGHT + (255,))
    h = 48
    w = int(W * h / H)
    save(out.resize((w, h), Image.LANCZOS), "logo.png")


def icon(name, draw_fn):
    s = 48
    img = big(s, s)
    d = ImageDraw.Draw(img)
    draw_fn(d, s * SS)
    save(small(img, s, s), "icons/" + name + ".png")


COL = (255, 214, 232, 255)
W8 = 4 * SS  # stroke width


def i_body(d, S):
    c = S / 2
    d.ellipse([c - S * .1, S * .08, c + S * .1, S * .28], fill=COL)
    d.rounded_rectangle([c - S * .17, S * .31, c + S * .17, S * .62], S * .08, fill=COL)
    d.rounded_rectangle([c - S * .15, S * .58, c - S * .03, S * .92], S * .05, fill=COL)
    d.rounded_rectangle([c + S * .03, S * .58, c + S * .15, S * .92], S * .05, fill=COL)
    d.rounded_rectangle([c - S * .3, S * .32, c - S * .2, S * .6], S * .05, fill=COL)
    d.rounded_rectangle([c + S * .2, S * .32, c + S * .3, S * .6], S * .05, fill=COL)


def i_bones(d, S):
    # a bone: a thick shaft with two knobs at each end
    a, b = (S * .3, S * .7), (S * .7, S * .3)
    d.line([a, b], fill=COL, width=int(S * .12))
    r = S * .085
    n = (S * .06, S * .06)  # perpendicular offset of the knobs
    for (x, y) in (a, b):
        for sgn in (-1, 1):
            cx, cy = x + n[0] * sgn, y + n[1] * sgn * -1
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=COL)


def i_morphs(d, S):
    pts = []
    for k in range(64):
        t = k / 64 * math.tau
        r = S * (.3 + .06 * math.sin(3 * t) + .03 * math.cos(5 * t))
        pts.append((S / 2 + r * math.cos(t), S / 2 + r * math.sin(t)))
    d.polygon(pts, outline=COL, width=W8)
    d.ellipse([S * .42, S * .42, S * .58, S * .58], fill=COL)


def i_presets(d, S):
    for k, alpha in ((2, 110), (1, 170), (0, 255)):
        o = k * S * .07
        d.rounded_rectangle([S * .22 + o, S * .18 - o * 0 + o, S * .7 + o, S * .8 + o], S * .06, outline=COL[:3] + (alpha,), width=W8)
    d.polygon([(S * .34, S * .18), (S * .5, S * .18), (S * .5, S * .44), (S * .42, S * .37), (S * .34, S * .44)], fill=COL)


def i_status(d, S):
    pts = [(S * .1, S * .55), (S * .3, S * .55), (S * .4, S * .3), (S * .52, S * .78), (S * .62, S * .45), (S * .7, S * .55), (S * .9, S * .55)]
    d.line(pts, fill=COL, width=W8, joint="curve")


def arc_arrow(d, S, start, end, head_at_end=True):
    box = [S * .2, S * .2, S * .8, S * .8]
    d.arc(box, start, end, fill=COL, width=W8)
    a = math.radians(end if head_at_end else start)
    cx, cy, r = S / 2, S / 2, S * .3
    x, y = cx + r * math.cos(a), cy + r * math.sin(a)
    tx, ty = -math.sin(a), math.cos(a)
    if not head_at_end:
        tx, ty = -tx, -ty
    h = S * .13
    d.polygon([(x + tx * h, y + ty * h), (x - ty * h * .8 - tx * h * .2, y + tx * h * .8 - ty * h * .2), (x + ty * h * .8 - tx * h * .2, y - tx * h * .8 - ty * h * .2)], fill=COL)


def i_reset(d, S):
    arc_arrow(d, S, 200, 500 - 40)


def i_refresh(d, S):
    arc_arrow(d, S, 20, 160)
    arc_arrow(d, S, 200, 340)


def i_xyz(d, S):
    o = (S * .32, S * .68)
    d.line([o, (S * .86, S * .68)], fill=COL, width=W8)
    d.line([o, (S * .32, S * .14)], fill=COL, width=W8)
    d.line([o, (S * .12, S * .88)], fill=COL, width=W8)
    for p in ((S * .86, S * .68), (S * .32, S * .14), (S * .12, S * .88)):
        d.ellipse([p[0] - S * .06, p[1] - S * .06, p[0] + S * .06, p[1] + S * .06], fill=COL)


def i_save(d, S):
    d.line([(S * .5, S * .15), (S * .5, S * .58)], fill=COL, width=W8)
    d.polygon([(S * .32, S * .44), (S * .68, S * .44), (S * .5, S * .64)], fill=COL)
    d.line([(S * .18, S * .6), (S * .18, S * .82), (S * .82, S * .82), (S * .82, S * .6)], fill=COL, width=W8, joint="curve")


def i_load(d, S):
    d.line([(S * .5, S * .66), (S * .5, S * .24)], fill=COL, width=W8)
    d.polygon([(S * .32, S * .36), (S * .68, S * .36), (S * .5, S * .15)], fill=COL)
    d.line([(S * .18, S * .6), (S * .18, S * .82), (S * .82, S * .82), (S * .82, S * .6)], fill=COL, width=W8, joint="curve")


def i_delete(d, S):
    d.line([(S * .2, S * .27), (S * .8, S * .27)], fill=COL, width=W8)
    d.line([(S * .4, S * .27), (S * .42, S * .16), (S * .58, S * .16), (S * .6, S * .27)], fill=COL, width=W8)
    d.polygon([(S * .27, S * .33), (S * .73, S * .33), (S * .68, S * .86), (S * .32, S * .86)], outline=COL, width=W8)
    for x in (.43, .57):
        d.line([(S * x, S * .42), (S * x, S * .76)], fill=COL, width=int(W8 * .7))


def i_folder(d, S):
    d.polygon([(S * .12, S * .26), (S * .4, S * .26), (S * .47, S * .34), (S * .88, S * .34), (S * .88, S * .8), (S * .12, S * .8)], outline=COL, width=W8)
    d.line([(S * .12, S * .44), (S * .88, S * .44)], fill=COL, width=W8)


def i_group(shape):
    def draw(d, S):
        if shape == "breasts":
            d.ellipse([S * .12, S * .3, S * .5, S * .72], outline=COL, width=W8)
            d.ellipse([S * .5, S * .3, S * .88, S * .72], outline=COL, width=W8)
        elif shape == "hips":
            d.line([(S * .14, S * .3), (S * .86, S * .3)], fill=COL, width=W8)
            d.arc([S * .14, S * .02, S * .86, S * .78], 0, 180, fill=COL, width=W8)
            d.line([(S * .3, S * .62), (S * .3, S * .9)], fill=COL, width=W8)
            d.line([(S * .7, S * .62), (S * .7, S * .9)], fill=COL, width=W8)
        elif shape == "thighs":
            d.rounded_rectangle([S * .22, S * .12, S * .44, S * .88], S * .1, outline=COL, width=W8)
            d.rounded_rectangle([S * .56, S * .12, S * .78, S * .88], S * .1, outline=COL, width=W8)
        elif shape == "waist":
            d.line([(S * .25, S * .15), (S * .4, S * .5), (S * .25, S * .85)], fill=COL, width=W8, joint="curve")
            d.line([(S * .75, S * .15), (S * .6, S * .5), (S * .75, S * .85)], fill=COL, width=W8, joint="curve")
        elif shape == "head":
            d.ellipse([S * .26, S * .14, S * .74, S * .66], outline=COL, width=W8)
            d.line([(S * .5, S * .66), (S * .5, S * .88)], fill=COL, width=W8)
    return draw


def main():
    os.makedirs(OUT, exist_ok=True)
    background()
    panels()
    grab()
    checkboxes()
    logo()
    for name, fn in (("body", i_body), ("bones", i_bones), ("morphs", i_morphs), ("presets", i_presets), ("status", i_status),
                     ("reset", i_reset), ("refresh", i_refresh), ("xyz", i_xyz), ("save", i_save), ("load", i_load),
                     ("delete", i_delete), ("folder", i_folder)):
        icon(name, fn)
    for g in ("breasts", "hips", "thighs", "waist", "head"):
        icon("group_" + g, i_group(g))

    skin = {
        "format": "UUEPBS skin",
        "version": 1,
        "name": "Midnight Rose",
        "author": "XTGMods",
        "description": "Example skin: glass panels over a night-sky background. Copy the folder, rename it and edit freely.",
        "colors": {
            "accent": "#E870A8", "accent_dim": "#8E3E6C", "title": "#FFB0D2",
            "good": "#7FE0A0", "warn": "#FFC060", "muted": "#A8A2C0", "text": "#F4EEF8",
            "window": "#0E0E1C", "panel": "#15132680", "popup": "#1A1830F5",
            "border": "#FFFFFF22", "separator": "#FFFFFF1E",
            "frame": "#0C0B18D0", "frame_hover": "#221C38E0", "frame_active": "#2E2248F0",
            "header": "#E870A83A", "header_hover": "#E870A866", "header_active": "#E870A899",
            "text_selected": "#E870A860",
            "scrollbar_bg": "#00000030", "scrollbar_grab": "#FFFFFF30", "scrollbar_grab_hover": "#E870A880", "scrollbar_grab_active": "#E870A8C0",
        },
        "style": {"frame_rounding": 7, "child_rounding": 9, "grab_rounding": 9, "tab_rounding": 8, "child_border": 0,
                  "frame_padding": [10, 6], "item_spacing": [9, 7], "logo_height": 1.5},
        "images": {
            "background": {"file": "background.jpg", "mode": "cover", "tint": "#FFFFFFE6"},
            "header": {"file": "header.png", "mode": "stretch"},
            "logo": "logo.png",
            "panel": {"file": "panel.png", "slice": 10},
            "button": {"file": "button.png", "slice": 8},
            "button_hover": {"file": "button_hover.png", "slice": 8},
            "button_active": {"file": "button_active.png", "slice": 8},
            "slider_track": {"file": "track.png", "slice": [11, 11, 11, 11]},
            "slider_fill": {"file": "fill.png", "slice": [11, 11, 11, 11]},
            "slider_grab": "grab.png",
            "slider_grab_active": "grab_active.png",
            "checkbox": "check_off.png",
            "checkbox_checked": "check_on.png",
            "tab": {"file": "tab.png", "slice": [9, 9, 9, 4]},
            "tab_hover": {"file": "tab_hover.png", "slice": [9, 9, 9, 4]},
            "tab_active": {"file": "tab_active.png", "slice": [9, 9, 9, 4]},
        },
        "icons": {
            "simplified": "icons/body.png", "detailed": "icons/bones.png", "presets": "icons/presets.png", "status": "icons/status.png",
            "bones": "icons/bones.png", "morphs": "icons/morphs.png",
            "reset": "icons/reset.png", "refresh": "icons/refresh.png", "xyz": "icons/xyz.png",
            "save": "icons/save.png", "load": "icons/load.png", "delete": "icons/delete.png", "folder": "icons/folder.png",
            "group.Breasts": "icons/group_breasts.png", "group.Hips": "icons/group_hips.png", "group.Thighs": "icons/group_thighs.png",
            "group.Waist": "icons/group_waist.png", "group.Head": "icons/group_head.png",
        },
    }
    with open(os.path.join(OUT, "skin.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(skin, f, indent=2)
        f.write("\n")
    total = sum(os.path.getsize(os.path.join(dp, n)) for dp, _, ns in os.walk(OUT) for n in ns)
    print("wrote", OUT, f"({total // 1024} KB)")


if __name__ == "__main__":
    sys.exit(main())
