"""Make the public, open-source copy of ClockHub in ../clockhub-firmware.

    python tools/export_public.py

Copies only what's safe to publish: the firmware source, the vendored audio library,
the settings web page, the Bible verses, the Reveille sound (CC BY) and the faces made
for ClockHub. Never copied: private-faces/, private-audio/, archive/, dist/, the store,
build logs, and third-party artwork (the imported Steam Punk theme).
Re-run after changes; it replaces the export (keeping its .git folder).
"""
import json
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.dirname(ROOT), "clockhub-firmware")
SKIP_FACES = {"steam-punk-t2is"}          # third-party art: not ours to publish
TOOLS = ["stamp_build.py", "make_faces.py", "make_icons.py", "make_bible.py", "build_installer.py",
         "installer_server.py", "set_wifi.py", "export_public.py"]


def copytree(src, dst):
    shutil.copytree(os.path.join(ROOT, src), os.path.join(OUT, dst or src),
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".bible-cache.json"))


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name == ".git":
            continue
        p = os.path.join(OUT, name)
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)

    for d in ("src", "lib", "include", "installer"):
        copytree(d, None)
    for f in ("platformio.ini", "partitions.csv"):
        shutil.copy(os.path.join(ROOT, f), OUT)
    os.makedirs(os.path.join(OUT, "tools"))
    for t in TOOLS:
        shutil.copy(os.path.join(ROOT, "tools", t), os.path.join(OUT, "tools", t))

    # data/: web page, verses, sounds, and the public gallery (not the private one in data/gallery)
    for d in ("www", "bible", "sounds"):
        copytree(os.path.join("data", d), os.path.join("data", d))
    gal = os.path.join(OUT, "data", "gallery")
    os.makedirs(os.path.join(gal, "faces", "assets"))
    idx = json.load(open(os.path.join(ROOT, "gallery-site", "index.json"), encoding="utf-8"))
    idx["faces"] = [f for f in idx["faces"] if f["id"] not in SKIP_FACES]
    for f in idx["faces"]:
        shutil.copy(os.path.join(ROOT, "gallery-site", f["file"]), os.path.join(gal, f["file"]))
        for a in f["assets"]:
            shutil.copy(os.path.join(ROOT, "gallery-site", "faces", "assets", a), os.path.join(gal, "faces", "assets", a))
    json.dump(idx, open(os.path.join(gal, "index.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)

    # sources of the faces made for ClockHub
    for fid in sorted(os.listdir(os.path.join(ROOT, "gallery-extra"))):
        if fid not in SKIP_FACES:
            copytree(os.path.join("gallery-extra", fid), None)
    shutil.copy(os.path.join(ROOT, "tools", "public_README.md"), os.path.join(OUT, "README.md"))
    shutil.copy(os.path.join(ROOT, "tools", "GPL-3.0.txt"), os.path.join(OUT, "LICENSE"))
    open(os.path.join(OUT, ".gitignore"), "w").write(".pio/\ndist/\n__pycache__/\n*.log\n")
    print("exported to", OUT, "-", len(idx["faces"]), "faces")


if __name__ == "__main__":
    main()
