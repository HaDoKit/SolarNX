"""Generates all SolarNX artwork in one style (the "eclipse"): a dark disc with a thin gold rim and a
soft corona, thin tilted orbits fading red -> blue, and the SOLARNX wordmark drawn as dark letters
with a glowing rim.

    python tools/branding/make_branding.py

Writes (paths relative to the repository root):
    SolarNX/Assets/icon.png, icon.ico      app / installer / tray icon, forwarder tile
    Client/icon.jpg                        .nro icon (Homebrew Menu)
    SolarNX/Assets/background.png          PC main window background (2560x1440)
    SolarNX/Assets/wordmark.png            PC home screen logo (transparent)
    Client/romfs/branding/background.png   Switch menu background (1280x720)
    Client/romfs/branding/wordmark.png     Switch menu top bar logo (transparent)

The backgrounds hold only the static part of the scene. The planets moving along the orbits are
drawn by the apps on top, using the same geometry as SCENE below - keep it identical to
SolarNX/UI/Controls/AnimatedBackgroundControl.xaml.cs and Client/menu/starsystem.c.

Needs Pillow and the Windows Segoe UI Semibold font (seguisb.ttf).
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FONT = "C:/Windows/Fonts/seguisb.ttf"

BG = (8, 9, 13)
RED = (255, 60, 40)
BLUE = (10, 185, 230)
GOLD = (255, 190, 80)
RIM_GOLD = (255, 214, 140)

# Scene geometry for the backgrounds, in fractions of the image height; the centre is the middle of
# the image. Mirrored in the apps' planet animation (see the module docstring).
SCENE = {
    "sun_radius": 0.13,
    "orbits": [(0.46, 0.105), (0.74, 0.17)],  # (rx, ry)
    "tilt_degrees": -12.0,
}


def lerp(a, b, k):
    return tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(len(a)))


def radial(size, center, radius, stops):
    """RGBA radial gradient; stops = [(t, (r, g, b, a)), ...], t in 0..1."""
    w, h = size
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    px = img.load()
    cx, cy = center
    for y in range(max(0, int(cy - radius)), min(h, int(cy + radius) + 1)):
        for x in range(max(0, int(cx - radius)), min(w, int(cx + radius) + 1)):
            t = math.hypot(x - cx, y - cy) / radius
            if t > 1:
                continue
            for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
                if t0 <= t <= t1:
                    px[x, y] = lerp(c0, c1, (t - t0) / (t1 - t0) if t1 > t0 else 0)
                    break
    return img


def circle_mask(size, cx, cy, r):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    return m


def ellipse_point(center, rx, ry, tilt, t):
    x, y = math.cos(t) * rx, math.sin(t) * ry
    return (center[0] + x * math.cos(tilt) - y * math.sin(tilt), center[1] + x * math.sin(tilt) + y * math.cos(tilt))


def draw_orbit_arc(canvas, center, rx, ry, tilt, t_from, t_to, width, alpha):
    """Arc of a tilted ellipse, coloured red (left) -> blue (right)."""
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    steps = max(200, int(rx * 0.8))
    for k in range(steps):
        p0 = ellipse_point(center, rx, ry, tilt, t_from + (t_to - t_from) * k / steps)
        p1 = ellipse_point(center, rx, ry, tilt, t_from + (t_to - t_from) * (k + 1) / steps)
        kx = max(0.0, min(1.0, (p0[0] - (center[0] - rx)) / (2 * rx)))
        d.line([p0, p1], fill=lerp(RED, BLUE, kx) + (alpha,), width=max(1, int(width)))
    canvas.alpha_composite(layer)


def draw_eclipse(canvas, center, r, rim_width, front_arcs):
    """Corona, rim glow, dark disc, rim; front_arcs() is drawn over the disc."""
    size = canvas.size
    canvas.alpha_composite(radial(size, center, r * 2.1, [(0, GOLD + (0,)), (0.47, GOLD + (0,)), (0.50, GOLD + (120,)),
                                                         (0.62, (255, 140, 40, 45)), (1, (255, 120, 30, 0))]))
    rim = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(rim).ellipse((center[0] - r, center[1] - r, center[0] + r, center[1] + r), outline=RIM_GOLD + (255,), width=max(1, int(rim_width)))
    canvas.alpha_composite(rim.filter(ImageFilter.GaussianBlur(rim_width)))
    canvas.paste(Image.new("RGBA", size, BG + (255,)), (0, 0), circle_mask(size, center[0], center[1], r))
    canvas.alpha_composite(rim)
    front_arcs()


def draw_dot(canvas, p, color, r):
    canvas.alpha_composite(radial(canvas.size, p, r * 2.2, [(0, color + (90,)), (1, color + (0,))]))
    canvas.paste(Image.new("RGBA", canvas.size, color + (255,)), (0, 0), circle_mask(canvas.size, p[0], p[1], r))


def draw_stars(canvas, count, seed, keep_out):
    """Sparse, faint, static stars; keep_out = (cx, cy, r) left empty around the eclipse."""
    w, h = canvas.size
    rnd = random.Random(seed)
    d = ImageDraw.Draw(canvas)
    for _ in range(count):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        if math.hypot(x - keep_out[0], y - keep_out[1]) < keep_out[2]:
            continue
        r = rnd.uniform(0.5, 1.3) * h / 720
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, rnd.randint(35, 100)))


def wordmark(height_px, transparent):
    """SOLARNX: dark letters with a glowing rim - gold for SOLAR, red N, blue X."""
    s = 4
    size = int(height_px * s)  # font size ~ cap height / 0.7
    font = ImageFont.truetype(FONT, size)
    tracking = size * 0.084
    stroke = max(1, int(size * 0.048))
    letters = [(c, RIM_GOLD) for c in "SOLAR"] + [("N", RED), ("X", BLUE)]
    width = sum(font.getlength(c) for c, _ in letters) + tracking * (len(letters) - 1)
    pad = int(size * 0.35)
    left, top, right, bottom = font.getbbox("SOLARNX")
    W, H = int(width + pad * 2), int(bottom - top + pad * 2)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0) if transparent else BG + (255,))
    rim_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    fill_mask = Image.new("L", (W, H), 0)
    rd, fd = ImageDraw.Draw(rim_layer), ImageDraw.Draw(fill_mask)
    x, y = pad, pad - top
    for c, col in letters:
        rd.text((x, y), c, font=font, fill=col + (255,), stroke_width=stroke, stroke_fill=col + (255,))
        fd.text((x, y), c, font=font, fill=255)
        x += font.getlength(c) + tracking
    halo = rim_layer.filter(ImageFilter.GaussianBlur(size * 0.10))
    halo.putalpha(halo.getchannel("A").point(lambda v: int(v * 0.55)))
    canvas.alpha_composite(halo)
    canvas.alpha_composite(rim_layer)
    canvas.paste(Image.new("RGBA", (W, H), BG + (255,)), (0, 0), fill_mask)
    return canvas.resize((W // s, H // s), Image.LANCZOS)


def background(w, h, seed):
    s = 2
    W, H = w * s, h * s
    canvas = Image.new("RGBA", (W, H), BG + (255,))
    center = (W / 2, H / 2)
    r = SCENE["sun_radius"] * H
    tilt = math.radians(SCENE["tilt_degrees"])
    draw_stars(canvas, int(100 * (w / h) / (16 / 9)), seed, (center[0], center[1], r * 2.4))  # same density at any size
    line = 1.4 * H / 720
    for rx, ry in SCENE["orbits"]:
        draw_orbit_arc(canvas, center, rx * H, ry * H, tilt, math.pi, math.tau, line, 90)  # back arcs

    def front():
        for rx, ry in SCENE["orbits"]:
            draw_orbit_arc(canvas, center, rx * H, ry * H, tilt, 0, math.pi, line, 200)

    draw_eclipse(canvas, center, r, 2.0 * H / 720, front)
    return canvas.resize((w, h), Image.LANCZOS).convert("RGB")


def icon():
    s = 4
    N = 256 * s
    canvas = Image.new("RGBA", (N, N), BG + (255,))
    center = (N / 2, N * 0.385)
    r = N * 0.135
    rx, ry, tilt = N * 0.36, N * 0.085, math.radians(-12)
    draw_orbit_arc(canvas, center, rx, ry, tilt, math.pi, math.tau, 1.6 * s, 110)
    draw_eclipse(canvas, center, r, 2.2 * s, lambda: draw_orbit_arc(canvas, center, rx, ry, tilt, 0, math.pi, 1.6 * s, 255))
    draw_dot(canvas, ellipse_point(center, rx, ry, tilt, math.radians(160)), RED, N * 0.020)
    draw_dot(canvas, ellipse_point(center, rx, ry, tilt, math.radians(338)), BLUE, N * 0.017)
    icon_img = canvas.resize((256, 256), Image.LANCZOS)
    mark = wordmark(31, transparent=True)
    icon_img.alpha_composite(mark, ((256 - mark.width) // 2, int(256 * 0.70) - mark.height // 2 + 8))
    return icon_img


def out(path):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full


def main():
    ic = icon()
    ic.save(out("SolarNX/Assets/icon.png"))
    ic.save(out("SolarNX/Assets/icon.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    ic.convert("RGB").save(out("Client/icon.jpg"), quality=95)

    background(2560, 1440, seed=11).save(out("SolarNX/Assets/background.png"), optimize=True)
    wordmark(56, transparent=True).save(out("SolarNX/Assets/wordmark.png"), optimize=True)

    background(1280, 720, seed=7).save(out("Client/romfs/branding/background.png"), optimize=True)
    wordmark(34, transparent=True).save(out("Client/romfs/branding/wordmark.png"), optimize=True)
    print("ok")


if __name__ == "__main__":
    main()
