#!/usr/bin/env python3
"""Publish firmware to the Pi; the board installs it by itself (TZT within ~20 s, Main within a minute).

    pio run -e tzt-esp32-devkit-ota && python3 scripts/publish_firmware.py              # TZT
    pio run -e esp32-wroom-32-ota && python3 scripts/publish_firmware.py --main         # Main board
    python3 scripts/publish_firmware.py --both                                          # both (built)

Uploads .pio/build/<env>/firmware.bin with the build id from fw_version.txt (scripts/fw_version.py).
Works from anywhere: it uses the Pi's public address (BACKEND_URL in include/config_tzt.h) unless
--pi is given, e.g. --pi http://192.168.1.202 on the home network.
"""
import argparse
import json
import os
import re
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def tls_context():
    """python.org's macOS Python ships without root certificates; use certifi or the system bundle."""
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    for bundle in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"):
        if os.path.exists(bundle):
            return ssl.create_default_context(cafile=bundle)
    return ssl.create_default_context()


def define(header, name):
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, (ROOT / header).read_text())
    if not m:
        sys.exit(f"{name} not found in {header}")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--main", action="store_true", help="the main board (env esp32-wroom-32-ota)")
    ap.add_argument("--both", action="store_true", help="TZT and main board")
    ap.add_argument("--pi", help="Pi base URL (default: BACKEND_URL from include/config_tzt.h)")
    args = ap.parse_args()
    targets = [TZT, MAIN] if args.both else [MAIN] if args.main else [TZT]
    for env, device in targets:
        publish(env, device, args.pi)


TZT = ("tzt-esp32-devkit-ota", "tzt-display")
MAIN = ("esp32-wroom-32-ota", "main-board")


def publish(env, device, pi):
    build = ROOT / ".pio" / "build" / env
    image, version_file = build / "firmware.bin", build / "fw_version.txt"
    if not image.exists() or not version_file.exists():
        sys.exit(f"No build in {build} - run: pio run -e {env}")
    version = version_file.read_text().strip()
    blob = image.read_bytes()
    base = (pi or define("include/config_tzt.h", "BACKEND_URL")).rstrip("/")
    key = define("include/secrets.h", "BACKEND_API_KEY")

    url = f"{base}/api/firmware/{device}?" + urllib.parse.urlencode({"version": version})
    req = urllib.request.Request(url, data=blob, method="POST", headers={
        "X-API-Key": key, "Content-Type": "application/octet-stream", "User-Agent": "prick-n-print-publish"})
    print(f"Publishing {device} {version} ({len(blob):,} bytes) to {base} ...")
    try:
        with urllib.request.urlopen(req, timeout=120, context=tls_context()) as r:
            meta = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"Pi refused it: HTTP {e.code} {e.read().decode(errors='replace')}")
    except urllib.error.URLError as e:
        sys.exit(f"Could not reach the Pi: {e.reason}")
    if device == "main-board":
        print(f"Published. The TZT tells the main board, which installs it and restarts (it also checks")
        print("every 6 h by itself). Its build id shows on its /system page once it's done.")
    else:
        print(f"Published. The TZT installs it on its next poll: 'Updating' on its screen, then it")
        print("restarts. If it fails, the Pi offers it again in 30 min.")


if __name__ == "__main__":
    main()
