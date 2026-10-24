"""Draw the Astrology face's dial: a zodiac wheel on a purple starry sky.

    python gallery-extra/astrology/source/make_astrology.py

The 12 zodiac signs sit at the hour positions (Aries at 12) in a gold band; the
watch's fonts have no zodiac symbols, so the whole dial is one image, rendered from
SVG with Chrome (Segoe UI Symbol supplies the glyphs; U+FE0E keeps them as plain
symbols rather than colour emoji). Output: ../assets/astrology_dial.bin (RGB565).
"""
import math
import os
import random
import struct
import subprocess

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
D, C = 466, 233
GOLD, GOLD_D = "#E8C97A", "#B8964A"
SIGNS = "\u2648\u2649\u264A\u264B\u264C\u264D\u264E\u264F\u2650\u2651\u2652\u2653"   # Aries .. Pisces


def pt(r, deg):
    a = math.radians(deg)
    return C + r * math.sin(a), C - r * math.cos(a)


def svg():
    rnd = random.Random(5)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {D} {D}" width="{D}" height="{D}">',
         '<defs>'
         '<radialGradient id="sky" cx="50%" cy="45%" r="60%"><stop offset="0" stop-color="#5E2B8C"/>'
         '<stop offset="0.55" stop-color="#2E1250"/><stop offset="1" stop-color="#10051C"/></radialGradient>'
         '<radialGradient id="neb1" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="#C45CC8" stop-opacity="0.45"/><stop offset="1" stop-color="#C45CC8" stop-opacity="0"/></radialGradient>'
         '<radialGradient id="neb2" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="#5B7CFF" stop-opacity="0.35"/><stop offset="1" stop-color="#5B7CFF" stop-opacity="0"/></radialGradient>'
         '<linearGradient id="band" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3B1863"/><stop offset="1" stop-color="#22093D"/></linearGradient>'
         '</defs>',
         f'<rect width="{D}" height="{D}" fill="url(#sky)"/>',
         '<ellipse cx="160" cy="170" rx="150" ry="90" fill="url(#neb1)" transform="rotate(-25 160 170)"/>',
         '<ellipse cx="310" cy="300" rx="140" ry="80" fill="url(#neb2)" transform="rotate(30 310 300)"/>']
    for _ in range(170):  # stars
        x, y = rnd.uniform(0, D), rnd.uniform(0, D)
        if math.hypot(x - C, y - C) > 186:
            continue
        r = rnd.choice([0.6, 0.7, 0.9, 1.1, 1.5])
        o.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="#FFFFFF" opacity="{rnd.uniform(0.35, 0.95):.2f}"/>')
    # a few constellation-style figures, faint
    # small dipper-like star patterns, one in each quarter of the sky, clear of the centre
    shape = [(-30, 8), (-14, 2), (0, 6), (12, -4), (28, -2), (30, 14), (14, 16)]
    for q, rot in ((45, 20), (135, -35), (225, 60), (315, -10)):
        cx, cy = pt(132, q)
        a = math.radians(rot)
        pts = [(cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a)) for x, y in shape]
        o.append('<polyline points="%s" fill="none" stroke="#D9C8FF" stroke-opacity="0.35" stroke-width="1"/>' % " ".join(f"{x:.0f},{y:.0f}" for x, y in pts))
        o += [f'<circle cx="{x:.0f}" cy="{y:.0f}" r="1.8" fill="#F3EAFF"/>' for x, y in pts]
    # eight-pointed star in the middle, faint gold
    star = []
    for k in range(16):
        r = 64 if k % 2 == 0 else 16
        star.append("%.1f,%.1f" % pt(r, k * 22.5))
    o.append(f'<polygon points="{" ".join(star)}" fill="{GOLD}" fill-opacity="0.16" stroke="{GOLD}" stroke-opacity="0.45" stroke-width="1"/>')
    o.append(f'<circle cx="{C}" cy="{C}" r="120" fill="none" stroke="{GOLD}" stroke-opacity="0.35" stroke-width="1" stroke-dasharray="2 5"/>')
    # zodiac band
    o.append(f'<circle cx="{C}" cy="{C}" r="210" fill="none" stroke="url(#band)" stroke-width="38"/>')
    for r, w in ((229, 2.5), (191, 2)):
        o.append(f'<circle cx="{C}" cy="{C}" r="{r}" fill="none" stroke="{GOLD}" stroke-width="{w}"/>')
    for i in range(12):
        (x1, y1), (x2, y2) = pt(191, i * 30 + 15), pt(229, i * 30 + 15)
        o.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{GOLD_D}" stroke-width="1.5"/>')
        gx, gy = pt(210, i * 30)
        o.append(f'<text x="{gx:.1f}" y="{gy:.1f}" font-family="Segoe UI Symbol" font-size="24" fill="{GOLD}" '
                 f'text-anchor="middle" dominant-baseline="central">{SIGNS[i]}&#xFE0E;</text>')
        dx, dy = pt(178, i * 30)  # hour dot just inside the band
        o.append(f'<circle cx="{dx:.1f}" cy="{dy:.1f}" r="{3.2 if i % 3 == 0 else 2}" fill="{GOLD}"/>')
    o.append("</svg>")
    return "".join(o)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    open(os.path.join(HERE, "dial.svg"), "w", encoding="utf-8").write(svg())
    html = os.path.join(HERE, "render.html")
    open(html, "w").write('<!doctype html><html><body style="margin:0"><img src="dial.svg" style="display:block;width:932px;height:932px"></body></html>')
    png = os.path.join(HERE, "dial_big.png")
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--window-size=932,932",
                    f"--screenshot={png}", "file:///" + html.replace("\\", "/")], check=True, capture_output=True)
    im = Image.open(png).convert("RGB").resize((D, D), Image.LANCZOS)
    im.save(os.path.join(HERE, "dial.png"))
    data = bytearray()
    for r, g, b in im.get_flattened_data():
        data += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    open(os.path.join(ASSETS, "astrology_dial.bin"), "wb").write(struct.pack("<BBHHHHH", 0x19, 0x12, 0, D, D, D * 2, 0) + bytes(data))
    print("ok")


if __name__ == "__main__":
    main()
