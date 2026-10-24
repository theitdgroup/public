"""Writes the bundled clock faces and the gallery catalog.

Installed on the watch:  data/faces/*.json
Gallery (device + site): data/gallery/ and gallery-site/
Run from the project root: python tools/make_faces.py
"""
import json
import os
import shutil

INSTALLED = ["classic", "digital", "newsdesk"]

FACES = {
    "classic": {
        "name": "Classic", "author": "ClockHub",
        "description": "Analog dial with a sweeping second hand and date window.",
        "background": {"color": "#0B1220", "color2": "#000000", "gradient": "vertical"},
        "elements": [
            {"type": "ticks", "count": 60, "radius": 226, "length": 10, "width": 2, "color": "#4A5568",
             "majorEvery": 5, "majorLength": 26, "majorWidth": 6, "majorColor": "#F5F5F7", "round": True},
            {"type": "numerals", "style": "arabic", "radius": 170, "size": 32, "color": "#E2E8F0"},
            {"type": "rect", "x": 0, "y": 82, "w": 112, "h": 40, "radius": 20, "color": "#1A2333"},
            {"type": "text", "text": "{ddd} {d}", "x": 0, "y": 82, "size": 20, "color": "#F6AD55", "upper": True},
            {"type": "hand", "hand": "hour", "length": 110, "tail": 18, "width": 12, "color": "#F5F5F7"},
            {"type": "hand", "hand": "minute", "length": 170, "tail": 18, "width": 8, "color": "#F5F5F7"},
            {"type": "hand", "hand": "second", "length": 190, "tail": 36, "width": 3, "color": "#F6AD55", "smooth": True},
            {"type": "circle", "radius": 10, "color": "#F6AD55", "borderColor": "#0B1220", "borderWidth": 3},
        ],
    },
    "digital": {
        "name": "Big Digital", "author": "ClockHub",
        "description": "Large time, full date and a seconds ring.",
        "background": {"color": "#000000"},
        "elements": [
            {"type": "arc", "value": "seconds", "radius": 229, "width": 6, "color": "#0A84FF", "bgColor": "#1C1C1E"},
            {"type": "text", "text": "{dddd}", "x": 0, "y": -92, "size": 24, "color": "#0A84FF", "upper": True, "letterSpacing": 2},
            {"type": "text", "text": "{time}", "x": 0, "y": -12, "size": 96, "color": "#FFFFFF"},
            {"type": "text", "text": "{ampm?}", "x": 0, "y": 52, "size": 20, "color": "#8E8E93"},
            {"type": "text", "text": "{MMMM} {d}, {yyyy}", "x": 0, "y": 84, "size": 24, "color": "#C7C7CC"},
            {"type": "text", "text": "{charging}{battery}", "x": 0, "y": 160, "size": 16, "color": "#8E8E93"},
        ],
    },
    "newsdesk": {
        "name": "News Desk", "author": "ClockHub",
        "description": "Time, date, rotating headlines and the current song.",
        "background": {"color": "#000000", "color2": "#0D1B2A", "gradient": "vertical"},
        "elements": [
            {"type": "text", "text": "{time}", "x": 0, "y": -88, "size": 72, "color": "#FFFFFF"},
            {"type": "text", "text": "{dddd} {d} {MMMM}", "x": 0, "y": -30, "size": 20, "color": "#8E8E93"},
            {"type": "rect", "x": 0, "y": 6, "w": 300, "h": 2, "color": "#2C2C2E"},
            {"type": "text", "text": "{headlineSource}", "x": 0, "y": 34, "size": 14, "color": "#0A84FF", "upper": True, "letterSpacing": 1},
            {"type": "text", "text": "{headline}", "x": 0, "y": 92, "size": 20, "color": "#F2F2F7", "width": 330, "wrap": True},
            {"type": "text", "text": "{np}", "x": 0, "y": 176, "size": 16, "color": "#1DB954", "width": 220, "marquee": True},
        ],
    },
    "neon-rings": {
        "name": "Neon Rings", "author": "ClockHub",
        "description": "Hours, minutes and seconds as glowing rings.",
        "background": {"color": "#000000"},
        "elements": [
            {"type": "arc", "value": "hours", "radius": 224, "width": 16, "color": "#FF2D55", "bgColor": "#2A0A12"},
            {"type": "arc", "value": "minutes", "radius": 200, "width": 16, "color": "#30D158", "bgColor": "#0A2A12"},
            {"type": "arc", "value": "seconds", "radius": 176, "width": 16, "color": "#0A84FF", "bgColor": "#0A1A2A"},
            {"type": "text", "text": "{time}", "x": 0, "y": -10, "size": 64, "color": "#FFFFFF"},
            {"type": "text", "text": "{ddd} {d} {MMM}", "x": 0, "y": 48, "size": 20, "color": "#8E8E93", "upper": True},
        ],
    },
    "roman-gold": {
        "name": "Roman Gold", "author": "ClockHub",
        "description": "Dress watch with gold Roman numerals.",
        "background": {"color": "#1A1612", "color2": "#000000", "gradient": "vertical"},
        "elements": [
            {"type": "circle", "radius": 228, "color": "none", "borderColor": "#3A2F1E", "borderWidth": 2},
            {"type": "ticks", "count": 60, "radius": 220, "length": 6, "width": 1, "color": "#8B6F3E",
             "majorEvery": 5, "majorLength": 14, "majorWidth": 3, "majorColor": "#D4AF37"},
            {"type": "numerals", "style": "roman", "radius": 178, "size": 28, "color": "#D4AF37"},
            {"type": "text", "text": "CLOCKHUB", "x": 0, "y": -80, "size": 14, "color": "#8B6F3E", "letterSpacing": 3},
            {"type": "hand", "hand": "hour", "length": 108, "tail": 0, "width": 7, "color": "#D4AF37"},
            {"type": "hand", "hand": "minute", "length": 160, "tail": 0, "width": 5, "color": "#D4AF37"},
            {"type": "hand", "hand": "second", "length": 182, "tail": 30, "width": 2, "color": "#E5C76B", "smooth": True},
            {"type": "circle", "radius": 7, "color": "#D4AF37"},
        ],
    },
    "chrono": {
        "name": "Chronograph", "author": "ClockHub",
        "description": "Sporty dial with date, weekday and battery sub-dials.",
        "background": {"color": "#1C1C1E", "color2": "#0A0A0A", "gradient": "vertical"},
        "elements": [
            {"type": "ticks", "count": 60, "radius": 228, "length": 8, "width": 2, "color": "#636366",
             "majorEvery": 5, "majorLength": 20, "majorWidth": 5, "majorColor": "#FFFFFF"},
            {"type": "numerals", "style": "arabic", "radius": 186, "size": 20, "color": "#AEAEB2"},
            {"type": "circle", "x": -82, "y": 0, "radius": 46, "color": "#111111", "borderColor": "#48484A", "borderWidth": 2},
            {"type": "text", "text": "{battery}", "x": -82, "y": 0, "size": 20, "color": "#30D158"},
            {"type": "circle", "x": 82, "y": 0, "radius": 46, "color": "#111111", "borderColor": "#48484A", "borderWidth": 2},
            {"type": "text", "text": "{dd}", "x": 82, "y": 0, "size": 28, "color": "#FFFFFF"},
            {"type": "circle", "x": 0, "y": 86, "radius": 40, "color": "#111111", "borderColor": "#48484A", "borderWidth": 2},
            {"type": "text", "text": "{ddd}", "x": 0, "y": 86, "size": 16, "color": "#FF9F0A", "upper": True},
            {"type": "hand", "hand": "hour", "length": 112, "tail": 20, "width": 10, "color": "#FFFFFF"},
            {"type": "hand", "hand": "minute", "length": 176, "tail": 20, "width": 7, "color": "#FFFFFF"},
            {"type": "hand", "hand": "second", "length": 200, "tail": 40, "width": 3, "color": "#FF9F0A"},
            {"type": "circle", "radius": 8, "color": "#FF9F0A"},
        ],
    },
    "sunset": {
        "name": "Sunset", "author": "ClockHub",
        "description": "Warm gradient with a clean digital time.",
        "background": {"color": "#FF7E5F", "color2": "#2B1055", "gradient": "vertical"},
        "elements": [
            {"type": "text", "text": "{time}", "x": 0, "y": -20, "size": 96, "color": "#FFFFFF"},
            {"type": "text", "text": "{dddd}", "x": 0, "y": 62, "size": 24, "color": "#FFE5D9"},
            {"type": "text", "text": "{d} {MMMM}", "x": 0, "y": 96, "size": 20, "color": "#FFD1C1"},
            {"type": "arc", "value": "battery", "radius": 226, "width": 4, "startAngle": 150, "sweep": 60,
             "color": "#FFFFFF", "bgColor": "#FFFFFF40"},
        ],
    },
    "terminal": {
        "name": "Terminal", "author": "ClockHub",
        "description": "Green-screen console look.",
        "background": {"color": "#020702"},
        "elements": [
            {"type": "text", "text": "clockhub:~$ date", "x": -150, "y": -110, "size": 16, "color": "#1F9E3D", "align": "left"},
            {"type": "text", "text": "{ddd} {yyyy}-{MM}-{dd}", "x": -150, "y": -78, "size": 20, "color": "#33FF66", "align": "left"},
            {"type": "text", "text": "{HH}:{mm}:{ss}", "x": -150, "y": -20, "size": 48, "color": "#33FF66", "align": "left"},
            {"type": "text", "text": "clockhub:~$ now-playing", "x": -150, "y": 40, "size": 16, "color": "#1F9E3D", "align": "left"},
            {"type": "text", "text": "{np}", "x": -150, "y": 70, "size": 20, "color": "#33FF66", "align": "left", "width": 300, "marquee": True},
            {"type": "text", "text": "batt {battery}  _", "x": -150, "y": 110, "size": 16, "color": "#1F9E3D", "align": "left"},
        ],
    },
}

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


catalog = {"name": "ClockHub Faces", "version": 1, "faces": []}
for fid, face in FACES.items():
    face = {"id": fid, "version": 1, **face}
    if fid in INSTALLED:
        write(os.path.join(ROOT, "data", "faces", fid + ".json"), face)
    for base in ("data/gallery", "gallery-site"):
        write(os.path.join(ROOT, base, "faces", fid + ".json"), face)
    catalog["faces"].append({"id": fid, "name": face["name"], "author": face["author"],
                             "description": face["description"], "file": "faces/" + fid + ".json", "assets": []})
# Image-based faces live in <folder>/<id>/face.json with their .bin images in
# <folder>/<id>/assets/. gallery-extra/ (e.g. imported .orb themes) goes into both
# galleries; private-faces/ (e.g. trademarked emblems) only into the watch's own gallery.
site_catalog = {**catalog, "faces": list(catalog["faces"])}
for folder, bases in (("gallery-extra", ("data/gallery", "gallery-site")), ("private-faces", ("data/gallery",))):
    src = os.path.join(ROOT, folder)
    for fid in sorted(os.listdir(src)) if os.path.isdir(src) else []:
        if not os.path.isfile(os.path.join(src, fid, "face.json")):
            continue
        with open(os.path.join(src, fid, "face.json")) as fh:
            face = json.load(fh)
        assets_dir = os.path.join(src, fid, "assets")
        assets = sorted(os.listdir(assets_dir)) if os.path.isdir(assets_dir) else []
        face["assets"] = assets
        for base in bases:
            write(os.path.join(ROOT, base, "faces", fid + ".json"), face)
            os.makedirs(os.path.join(ROOT, base, "faces", "assets"), exist_ok=True)
            for a in assets:
                shutil.copy(os.path.join(assets_dir, a), os.path.join(ROOT, base, "faces", "assets", a))
        entry = {"id": fid, "name": face["name"], "author": face.get("author", ""),
                 "description": face.get("description", ""), "file": "faces/" + fid + ".json", "assets": assets}
        catalog["faces"].append(entry)
        if "gallery-site" in bases:
            site_catalog["faces"].append(entry)

write(os.path.join(ROOT, "data/gallery", "index.json"), catalog)
write(os.path.join(ROOT, "gallery-site", "index.json"), site_catalog)
# The gallery site renders previews with the same code as the device
shutil.copy(os.path.join(ROOT, "data", "www", "face-render.js"), os.path.join(ROOT, "gallery-site", "face-render.js"))
print("wrote", len(FACES), "faces")
