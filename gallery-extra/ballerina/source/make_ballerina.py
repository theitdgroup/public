"""Draw the Ballerina face's pirouette frames.

    python gallery-extra/ballerina/source/make_ballerina.py

The dancer is posed as a 3D skeleton (en pointe, working leg in passe, arms
rounded overhead), turned a little each frame about the vertical axis and drawn
back to front, so the frames read as one continuous spin. Output: LVGL RGB565A8
.bin frames in ../assets plus PNG previews here.
"""
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
FRAMES = 8
W, H = 150, 226          # frame size on the watch
S = 4                    # supersampling
TILT = math.radians(10)  # we look at her slightly from above

SKIN = (242, 201, 168)
LEOTARD = (226, 92, 140)
TIGHTS = (246, 190, 202)
SHOE = (240, 143, 176)
HAIR = (59, 36, 22)
TUTU = (248, 186, 210)
TUTU_EDGE = (255, 236, 244)

# Skeleton in figure units: x right, y down, z toward the viewer. Origin = spin axis.
J = {
    "head": (0, 24, 0), "bun": (0, 14, -11), "neck": (0, 42, 0),
    "shL": (-17, 50, 0), "shR": (17, 50, 0),
    "elL": (-31, 23, 4), "elR": (31, 23, 4),
    "haL": (-7, 0, 9), "haR": (7, 0, 9),
    "waist": (0, 100, 0), "hipL": (-7, 112, 0), "hipR": (7, 112, 0),
    "knL": (-3, 156, 0), "anL": (-1, 199, 0), "toL": (0, 214, 1),
    "knR": (31, 140, 9), "anR": (2, 150, 4), "toR": (-3, 156, 3),
}


def project(p, th):
    x, y, z = p
    xr = x * math.cos(th) + z * math.sin(th)
    zr = -x * math.sin(th) + z * math.cos(th)
    yp = y * math.cos(TILT) - zr * math.sin(TILT)
    return xr, yp, zr


def shade(c, z):
    k = 1.0 + max(-0.18, min(0.08, z / 120))
    return tuple(max(0, min(255, int(v * k))) for v in c)


def frame(th):
    img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ox, oy = W / 2, 6  # spin axis x, top margin
    P = {k: project(v, th) for k, v in J.items()}

    def pt(p):
        return ((ox + p[0]) * S, (oy + p[1]) * S)

    parts = []  # (depth, draw function)

    def limb(a, b, w0, w1, col):
        pa, pb = P[a], P[b]
        z = (pa[2] + pb[2]) / 2

        def draw():
            c = shade(col, z)
            (x0, y0), (x1, y1) = pt(pa), pt(pb)
            n = 12
            for i in range(n + 1):
                t = i / n
                r = (w0 + (w1 - w0) * t) * S / 2
                x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
                d.ellipse([x - r, y - r, x + r, y + r], fill=c)
        parts.append((z, draw))

    # Legs (pink tights) and pointe shoes
    limb("hipL", "knL", 13, 10, TIGHTS); limb("knL", "anL", 10, 6, TIGHTS); limb("anL", "toL", 6, 5, SHOE)
    limb("hipR", "knR", 13, 10, TIGHTS); limb("knR", "anR", 10, 6, TIGHTS); limb("anR", "toR", 6, 5, SHOE)
    # Arms
    for s in "LR":
        limb("sh" + s, "el" + s, 7, 6, SKIN); limb("el" + s, "ha" + s, 6, 4, SKIN)

    # Torso (leotard): a tapered body from shoulders to waist
    def torso():
        # seen side-on the body still has depth, so keep a minimum width
        def span(y, half):
            cx = (pt(project((-half, y, 0), th))[0] + pt(project((half, y, 0), th))[0]) / 2
            w = max(abs(pt(project((half, y, 0), th))[0] - cx), half * 0.55 * S)
            yy = pt(project((0, y, 0), th))[1]
            return cx - w, cx + w, yy
        l0, r0, y0_ = span(50, 17)
        l1, r1, y1_ = span(100, 8)
        d.polygon([(l0, y0_), (r0, y0_), (r1, y1_), (l1, y1_)], fill=LEOTARD)
        x0, y0 = pt(P["neck"]); r = 4.5 * S
        d.line([pt(P["neck"]), pt(project((0, 52, 0), th))], fill=SKIN, width=int(9 * S))
        d.ellipse([x0 - r, y0 - r, x0 + r, y0 + r], fill=SKIN)
        # rounded shoulders
        for s in ("shL", "shR"):
            x, y = pt(P[s]); r = 5.5 * S
            d.ellipse([x - r, y - r, x + r, y + r], fill=LEOTARD)
    parts.append((0, torso))

    # Head: face toward the viewer when facing front, hair otherwise; bun behind
    def head():
        hx, hy = pt(P["head"]); r = 12.5 * S
        bx, by = pt(P["bun"])
        facing = math.cos(th)  # 1 = facing us
        if P["bun"][2] < 0:  # bun behind the head
            d.ellipse([bx - 7 * S, by - 7 * S, bx + 7 * S, by + 7 * S], fill=HAIR)
        d.ellipse([hx - r, hy - r, hx + r, hy + r], fill=HAIR)
        if facing > -0.2:
            # face oval shifts toward where she faces
            fx = hx + math.sin(th) * 4 * S
            fw = r * (0.55 + 0.35 * max(0, facing))
            d.ellipse([fx - fw, hy - r * 0.55, fx + fw, hy + r * 0.95], fill=SKIN)
            if facing > 0.5:  # little smile and eyes when facing front
                for ex in (-4, 4):
                    d.ellipse([fx + (ex - 1) * S, hy + 1 * S, fx + (ex + 1) * S, hy + 3 * S], fill=(70, 40, 40))
                d.arc([fx - 3 * S, hy + 3 * S, fx + 3 * S, hy + 8 * S], 20, 160, fill=(200, 80, 100), width=S)
        if P["bun"][2] >= 0:
            d.ellipse([bx - 7 * S, by - 7 * S, bx + 7 * S, by + 7 * S], fill=HAIR)
    parts.append((P["head"][2], head))

    # Tutu: a stiff disc at the hips, ruffled edge that turns with her
    def tutu(front):
        cx, cy = pt(project((0, 112, 0), th))
        rx, ry = 54 * S, 54 * math.sin(TILT) * S + 9 * S
        pts = []
        n = 48
        for i in range(n + 1):
            a = (math.pi if front else 0) + math.pi * i / n
            ruffle = 1 + 0.07 * math.sin(14 * (a - th))
            pts.append((cx + math.cos(a) * rx * ruffle, cy + math.sin(a) * ry * ruffle))
        pts.append((cx, cy))
        d.polygon(pts, fill=TUTU)
        # layered edge highlights
        for i in range(0, n, 2):
            a = (math.pi if front else 0) + math.pi * i / n
            x, y = cx + math.cos(a) * rx * 0.95, cy + math.sin(a) * ry * 0.95
            rr = 3.2 * S
            d.ellipse([x - rr, y - rr * 0.7, x + rr, y + rr * 0.7], fill=TUTU_EDGE)
    parts.append((-30, lambda: tutu(False)))   # back half behind the body
    parts.append((40, lambda: tutu(True)))     # front half over the thighs

    for _, fn in sorted(parts, key=lambda p: p[0]):
        fn()
    return img.resize((W, H), Image.LANCZOS)


def to_rgb565a8(im):
    px = im.load()
    color, alpha = bytearray(), bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            color += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
            alpha.append(a)
    return struct.pack("<BBHHHHH", 0x19, 0x14, 0, W, H, W * 2, 0) + bytes(color) + bytes(alpha)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    sheet = Image.new("RGBA", (W * FRAMES, H), (40, 16, 50, 255))
    for i in range(FRAMES):
        im = frame(2 * math.pi * i / FRAMES)
        open(os.path.join(ASSETS, f"ballerina_spin{i}.bin"), "wb").write(to_rgb565a8(im))
        sheet.alpha_composite(im, (W * i, 0))
    sheet.save(os.path.join(HERE, "frames.png"))
    print("wrote", FRAMES, "frames")


if __name__ == "__main__":
    main()
