"""Build the browser installers (ESP Web Tools) for ClockHub.

    python tools/build_installer.py              (version = today's date)
    python tools/build_installer.py --version 1.2.0

Produces a ready-to-host folder (two with --public):
  dist/installer-public/  only with --public: everything except private-faces/
  dist/installer-full/    includes private-faces/ (keep private; serve with
                          tools/installer_server.py)
Each holds index.html, two manifests (new install / update app only), the
merged firmware image and the LittleFS image (web page + face gallery).
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, ".pio", "build", "clockhub")
FS_OFFSET = 0x810000  # "spiffs" partition in partitions.csv


def pio(*args, data_dir=None):
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    if data_dir:
        env["PLATFORMIO_DATA_DIR"] = data_dir
    r = subprocess.run([sys.executable, "-m", "platformio", "run", *args], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print(r.stdout[-3000:])
        sys.exit("PlatformIO failed: " + " ".join(args))


# Faces bigger than this stay out of the installer's file system image (8 MB in all); the
# watches keep their gallery on the SD card, and tools/sync_watch.py adds them over Wi-Fi.
INSTALLER_FACE_LIMIT = 1024 * 1024


def drop_faces(data_dir, keep):
    """Remove gallery faces that keep(face_entry, asset_bytes) rejects from a copy of data/."""
    gallery = os.path.join(data_dir, "gallery")
    idx_path = os.path.join(gallery, "index.json")
    idx = json.load(open(idx_path, encoding="utf-8"))
    kept, dropped = [], []
    for f in idx["faces"]:
        size = sum(os.path.getsize(os.path.join(gallery, "faces", "assets", a)) for a in f["assets"]
                   if os.path.exists(os.path.join(gallery, "faces", "assets", a)))
        if keep(f, size):
            kept.append(f)
            continue
        dropped.append(f["name"])
        for p in [os.path.join(gallery, f["file"])] + [os.path.join(gallery, "faces", "assets", a) for a in f["assets"]]:
            if os.path.exists(p):
                os.remove(p)
    idx["faces"] = kept
    json.dump(idx, open(idx_path, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    return dropped


def installer_data_dir():
    """Copy of data/ without the largest gallery faces, so the image fits the 8 MB partition."""
    out = os.path.join(ROOT, ".pio", "data-installer")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "data"), out)
    dropped = drop_faces(out, lambda f, size: size <= INSTALLER_FACE_LIMIT)
    if dropped:
        print("  left out of the installer (added over Wi-Fi by tools/sync_watch.py):", ", ".join(dropped))
    return out


def public_data_dir():
    """Copy of data/ whose gallery leaves out private-faces/."""
    out = os.path.join(ROOT, ".pio", "data-public")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "data"), out)
    private = os.path.join(ROOT, "private-faces")
    gallery = os.path.join(out, "gallery")
    for fid in os.listdir(private) if os.path.isdir(private) else []:
        p = os.path.join(gallery, "faces", fid + ".json")
        if os.path.exists(p):
            os.remove(p)
        for a in os.listdir(os.path.join(private, fid, "assets")) if os.path.isdir(os.path.join(private, fid, "assets")) else []:
            ap = os.path.join(gallery, "faces", "assets", a)
            if os.path.exists(ap):
                os.remove(ap)
    # gallery-site/index.json is the public catalog
    shutil.copy(os.path.join(ROOT, "gallery-site", "index.json"), os.path.join(gallery, "index.json"))
    return out


def write_installer(dest, edition, version, fs_image, faces_note):
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    shutil.copy(os.path.join(BUILD, "firmware.factory.bin"), os.path.join(dest, "clockhub.factory.bin"))
    shutil.copy(fs_image, os.path.join(dest, "clockhub-littlefs.bin"))
    # ?v= makes browsers and Cloudflare fetch each new version instead of a cached copy
    app = {"path": f"clockhub.factory.bin?v={version}", "offset": 0}
    fs = {"path": f"clockhub-littlefs.bin?v={version}", "offset": FS_OFFSET}
    for name, parts, erase in (("manifest-new.json", [app, fs], True), ("manifest-update.json", [app], False)):
        manifest = {"name": "ClockHub", "version": version, "new_install_prompt_erase": erase,
                    "builds": [{"chipFamily": "ESP32-S3", "parts": parts}]}
        with open(os.path.join(dest, name), "w", newline="\n") as fh:
            json.dump(manifest, fh, indent=2)
    with open(os.path.join(ROOT, "installer", "index.html"), encoding="utf-8") as fh:
        html = fh.read()
    html = html.replace("__EDITION__", edition).replace("__VERSION__", version).replace("__FACES_NOTE__", faces_note)
    for name in ("manifest-new.json", "manifest-update.json"):
        html = html.replace(f'manifest="{name}"', f'manifest="{name}?v={version}"')
    with open(os.path.join(dest, "index.html"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)
    # GitHub Pages: serve files as-is
    open(os.path.join(dest, ".nojekyll"), "w").close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=datetime.date.today().strftime("%Y.%m.%d"))
    ap.add_argument("--public", action="store_true", help="also build installer-public (gallery without private faces)")
    args = ap.parse_args()

    with open(os.path.join(ROOT, "src", "version.h"), "w", newline="\n") as fh:
        fh.write("// Written by tools/build_installer.py; shown on the web page and to the browser installer\n"
                 f'#pragma once\n#define FW_VERSION "{args.version}"\n')

    print("Updating face galleries...")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_faces.py")], check=True, stdout=subprocess.DEVNULL)
    print("Building firmware (a few minutes)...")
    pio()
    print("Building full file system image...")
    pio("-t", "buildfs", data_dir=installer_data_dir())
    full_fs = os.path.join(ROOT, ".pio", "littlefs-full.bin")
    shutil.copy(os.path.join(BUILD, "littlefs.bin"), full_fs)
    dist = os.path.join(ROOT, "dist")
    # Only the full edition by default: both editions use the same file names, so
    # having two folders around made it easy to upload a mix of them
    shutil.rmtree(os.path.join(dist, "installer-public"), ignore_errors=True)
    if args.public:
        print("Building public file system image...")
        pio("-t", "buildfs", data_dir=public_data_dir())
        public_fs = os.path.join(ROOT, ".pio", "littlefs-public.bin")
        shutil.copy(os.path.join(BUILD, "littlefs.bin"), public_fs)
        write_installer(os.path.join(dist, "installer-public"), "Public edition", args.version, public_fs, "")
    write_installer(os.path.join(dist, "installer-full"), "Full edition (private)", args.version, full_fs,
                    ", including the private faces")
    print(f"\nDone (version {args.version}):\n  {os.path.join(dist, 'installer-full')}")
    print("Try the full one with: python tools/installer_server.py")


if __name__ == "__main__":
    main()
