"""Draw the Soccer face's ball: a classic truncated-icosahedron ball (12 black
pentagons, 20 white hexagons) seen with one pentagon facing us.

    python gallery-extra/soccer/source/make_soccer.py

Writes ../assets/soccer_ball.bin (rotated by the second hand on the watch) and
../assets/soccer_shade.bin (fixed light and shadow on top, so the spinning ball
still looks round). Both are 240x240 LVGL ARGB8888, like the Volleyball face.
"""
import itertools
import math
import os
import struct

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
SIZE, R = 240, 112          # image size, ball radius in pixels
SS = 3                      # supersampling

PHI = (1 + 5 ** 0.5) / 2


def norm(v):
    l = math.sqrt(sum(c * c for c in v))
    return tuple(c / l for c in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def add(a, b, k=1.0):
    return tuple(x + y * k for x, y in zip(a, b))


# Icosahedron: its 12 vertices become the pentagons, its 20 faces the hexagons
ico = []
for a, b in itertools.product((-1, 1), repeat=2):
    ico += [(0, a, b * PHI), (a, b * PHI, 0), (b * PHI, 0, a)]
edge = min(math.dist(ico[0], v) for v in ico[1:])
faces = [f for f in itertools.combinations(range(12), 3)
         if all(abs(math.dist(ico[i], ico[j]) - edge) < 1e-6 for i, j in itertools.combinations(f, 2))]

# Truncation at 1/3 of each edge gives the ball's corners; from them, each face's plane distance
pent_d = dot(add(ico[0], sub(ico[next(j for j in range(12) if abs(math.dist(ico[0], ico[j]) - edge) < 1e-6)], ico[0]), 1 / 3), norm(ico[0]))
f0 = faces[0]
c0 = norm(tuple(sum(ico[i][k] for i in f0) for k in range(3)))
hex_d = dot(add(ico[f0[0]], sub(ico[f0[1]], ico[f0[0]]), 1 / 3), c0)
planes = [(norm(v), pent_d, True) for v in ico] + \
         [(norm(tuple(sum(ico[i][k] for i in f) for k in range(3))), hex_d, False) for f in faces]

# Turn the ball so a pentagon faces the viewer (+z)
front = norm(ico[0])
zx = math.atan2(front[0], front[2])


def rot(v):
    # rotate so `front` maps to +z: first about y, then about x
    x, y, z = v
    c, s = math.cos(-zx), math.sin(-zx)
    x, z = x * c + z * s, -x * s + z * c
    fy = front[1]
    fz = math.sqrt(1 - fy * fy)
    ang = math.atan2(fy, fz)
    c, s = math.cos(ang), math.sin(ang)
    y, z = y * c - z * s, y * s + z * c
    return (x, y, z)


planes = [(rot(n), d, p) for n, d, p in planes]


def ball():
    N = SIZE * SS
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = img.load()
    rr = R * SS
    seam = 0.022
    for j in range(N):
        for i in range(N):
            x, y = (i + 0.5 - N / 2) / rr, (N / 2 - j - 0.5) / rr
            q = x * x + y * y
            if q > 1:
                continue
            u = (x, y, math.sqrt(1 - q))
            # the face the ray leaves through has the largest (u.n)/d
            vals = sorted(((dot(u, n) / d, p) for n, d, p in planes), reverse=True)
            if vals[0][0] - vals[1][0] < seam * (vals[0][0]):
                c = (70, 74, 80)
            elif vals[0][1]:
                c = (22, 24, 28)
            else:
                c = (246, 247, 249)
            px[i, j] = c + (255,)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def shade():
    N = SIZE * SS
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = img.load()
    rr = R * SS
    L = norm((-0.45, 0.55, 0.7))  # light from the upper left
    for j in range(N):
        for i in range(N):
            x, y = (i + 0.5 - N / 2) / rr, (N / 2 - j - 0.5) / rr
            q = x * x + y * y
            if q > 1:
                continue
            z = math.sqrt(1 - q)
            lam = max(0.0, dot((x, y, z), L))
            dark = (1 - lam) ** 1.6 * 0.62 + (q ** 6) * 0.25          # shadow side + rim
            spec = max(0.0, lam) ** 28 * 0.85                          # glossy highlight
            if spec > dark:
                px[i, j] = (255, 255, 255, int(min(1, spec) * 255))
            else:
                px[i, j] = (6, 20, 10, int(min(1, dark) * 255))
    return img.resize((SIZE, SIZE), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.6))


def to_bin(im):
    out = bytearray(struct.pack("<BBHHHHH", 0x19, 0x10, 0, SIZE, SIZE, SIZE * 4, 0))
    for r, g, b, a in im.getdata():
        out += bytes((b, g, r, a))
    return bytes(out)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    b, s = ball(), shade()
    open(os.path.join(ASSETS, "soccer_ball.bin"), "wb").write(to_bin(b))
    open(os.path.join(ASSETS, "soccer_shade.bin"), "wb").write(to_bin(s))
    b.save(os.path.join(HERE, "ball.png"))
    s.save(os.path.join(HERE, "shade.png"))
    Image.alpha_composite(b, s).save(os.path.join(HERE, "combined.png"))
    print("ok")


if __name__ == "__main__":
    main()
