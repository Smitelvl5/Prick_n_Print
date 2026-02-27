#!/usr/bin/env python3
"""
Send messages, images, or audio to the TZT via the HTTP backend (no Firebase).

Usage:
  # Message (print + show on display)
  python scripts/post_to_backend.py --url http://192.168.1.100:5000 --message "Hello from shortcut"
  python scripts/post_to_backend.py --url http://192.168.1.100:5000 --message "Hi" --source shortcut

  # Image (ESP32 will download and show)
  python scripts/post_to_backend.py --url http://192.168.1.100:5000 --image path/to/photo.jpg
  python scripts/post_to_backend.py --url http://192.168.1.100:5000 --image photo.jpg --name my-photo.jpg

  # Audio / voice memo (ESP32 will download and can play)
  python scripts/post_to_backend.py --url http://192.168.1.100:5000 --audio path/to/voice.mp3
  python scripts/post_to_backend.py --url http://192.168.1.100:5000 --audio voice.mp3 --name memo.mp3

Set BACKEND_URL in config_tzt.h to the same base URL (e.g. http://YOUR_PC_IP:5000).

For public URL with HTTP Basic Auth:
  python post_to_backend.py --url http://YOUR_PUBLIC_IP --user admin --password YOUR_PASS --message "Hi"
"""

import argparse
import os
import sys

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = os.path.dirname(_SCRIPT_DIR)

try:
    import requests
except ImportError:
    print("Install requests: pip install requests", file=sys.stderr)
    sys.exit(1)


def post_command(base_url: str, payload: dict, auth: tuple | None = None) -> bool:
    r = requests.post(f"{base_url.rstrip('/')}/api/commands", json=payload, auth=auth, timeout=10)
    if r.status_code not in (200, 201):
        print(f"Error: {r.status_code} {r.text}", file=sys.stderr)
        return False
    print("Command sent.")
    return True


def post_image(base_url: str, path: str, name: str | None, auth: tuple | None = None) -> bool:
    base_url = base_url.rstrip("/")
    if not os.path.isfile(path):
        print(f"File not found: {path}", file=sys.stderr)
        return False
    name = name or os.path.basename(path)
    with open(path, "rb") as f:
        r = requests.post(
            f"{base_url}/api/images",
            files={"file": (name, f)},
            data={"name": name},
            auth=auth,
            timeout=30,
        )
    if r.status_code not in (200, 201):
        print(f"Error: {r.status_code} {r.text}", file=sys.stderr)
        return False
    print(f"Image queued. TZT will download and show: {name}")
    return True


def post_audio(base_url: str, path: str, name: str | None, auth: tuple | None = None) -> bool:
    base_url = base_url.rstrip("/")
    if not os.path.isfile(path):
        print(f"File not found: {path}", file=sys.stderr)
        return False
    name = name or os.path.basename(path)
    with open(path, "rb") as f:
        r = requests.post(
            f"{base_url}/api/audio",
            files={"file": (name, f)},
            data={"name": name},
            auth=auth,
            timeout=30,
        )
    if r.status_code not in (200, 201):
        print(f"Error: {r.status_code} {r.text}", file=sys.stderr)
        return False
    print(f"Audio queued. TZT will download: {name}")
    return True


def main():
    ap = argparse.ArgumentParser(description="POST message/image/audio to TZT HTTP backend")
    ap.add_argument("--url", "-u", required=True, help="Backend base URL (e.g. http://192.168.1.100:5000)")
    ap.add_argument("--user", "-U", help="HTTP Basic Auth username (for public URL with password)")
    ap.add_argument("--password", "-P", help="HTTP Basic Auth password (for public URL with password)")
    ap.add_argument("--message", "-m", help="Print message (sends type=print command)")
    ap.add_argument("--source", "-s", default="shortcut", help="Source label for print (default: shortcut)")
    ap.add_argument("--image", "-i", help="Path to image file to send")
    ap.add_argument("--audio", "-a", help="Path to audio file to send")
    ap.add_argument("--name", "-n", help="Filename for image/audio (default: from path)")
    args = ap.parse_args()

    auth = (args.user, args.password) if (args.user and args.password) else None
    if args.message:
        ok = post_command(args.url, {"type": "print", "data": args.message, "source": args.source}, auth=auth)
    elif args.image:
        ok = post_image(args.url, args.image, args.name, auth=auth)
    elif args.audio:
        ok = post_audio(args.url, args.audio, args.name, auth=auth)
    else:
        ap.print_help()
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
