"""Serve the full (private) browser installer on this computer.

    python tools/installer_server.py        -> opens http://localhost:8123

Browsers allow USB access from http://localhost, so no https is needed here.
Build it first with tools/build_installer.py.
"""
import functools
import http.server
import os
import webbrowser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "dist", "installer-full")
PORT = 8123

if not os.path.isfile(os.path.join(SITE, "index.html")):
    raise SystemExit("Build the installer first: python tools/build_installer.py")

handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=SITE)
url = f"http://localhost:{PORT}/"
print(f"ClockHub full installer at {url}  (Ctrl+C to stop)")
webbrowser.open(url)
http.server.ThreadingHTTPServer(("localhost", PORT), handler).serve_forever()
