#!/usr/bin/env python3
"""
Test posting message, image, and audio to the TZT backend (Raspberry Pi).
Run with: python scripts/test_backend.py --url http://192.168.1.50:5000
"""

import argparse
import os
import sys

try:
    import requests
except ImportError:
    print("Install requests: pip install requests", file=sys.stderr)
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
SAMPLES = os.path.join(PROJECT_ROOT, "raspberrypi", "samples")


def main():
    ap = argparse.ArgumentParser(description="Test TZT backend (message, image, audio)")
    ap.add_argument("--url", "-u", required=True, help="Backend URL (e.g. http://192.168.1.50:5000)")
    ap.add_argument("--image", "-i", help="Image path (default: raspberrypi/samples/sample.jpg)")
    ap.add_argument("--audio", "-a", help="Audio path (default: raspberrypi/samples/sample.mp3)")
    args = ap.parse_args()

    base = args.url.rstrip("/")
    image_path = args.image or os.path.join(SAMPLES, "sample.jpg")
    audio_path = args.audio or os.path.join(SAMPLES, "sample.mp3")

    ok_count = 0

    # 1. Test health
    print("1. Health check...")
    try:
        r = requests.get(f"{base}/api/health", timeout=5)
        if r.status_code == 200:
            print("   OK")
            ok_count += 1
        else:
            print(f"   Failed: {r.status_code}")
    except Exception as e:
        print(f"   Error: {e}")
        print("\nIs the server running? Check URL and network.")
        return 1

    # 2. Test message
    print("2. Posting message...")
    try:
        r = requests.post(
            f"{base}/api/commands",
            json={"type": "print", "data": "Test from script!", "source": "test"},
            timeout=10,
        )
        if r.status_code in (200, 201):
            print("   OK - message sent")
            ok_count += 1
        else:
            print(f"   Failed: {r.status_code} {r.text[:200]}")
    except Exception as e:
        print(f"   Error: {e}")

    # 3. Test image
    print("3. Posting image...")
    if os.path.isfile(image_path):
        try:
            with open(image_path, "rb") as f:
                r = requests.post(
                    f"{base}/api/images",
                    files={"file": (os.path.basename(image_path), f)},
                    data={"name": os.path.basename(image_path)},
                    timeout=30,
                )
            if r.status_code in (200, 201):
                print(f"   OK - image queued: {os.path.basename(image_path)}")
                ok_count += 1
            else:
                print(f"   Failed: {r.status_code} {r.text[:200]}")
        except Exception as e:
            print(f"   Error: {e}")
    else:
        print(f"   Skip - file not found: {image_path}")

    # 4. Test audio
    print("4. Posting audio...")
    if os.path.isfile(audio_path):
        try:
            with open(audio_path, "rb") as f:
                r = requests.post(
                    f"{base}/api/audio",
                    files={"file": (os.path.basename(audio_path), f)},
                    data={"name": os.path.basename(audio_path)},
                    timeout=30,
                )
            if r.status_code in (200, 201):
                print(f"   OK - audio queued: {os.path.basename(audio_path)}")
                ok_count += 1
            else:
                print(f"   Failed: {r.status_code} {r.text[:200]}")
        except Exception as e:
            print(f"   Error: {e}")
    else:
        print(f"   Skip - file not found: {audio_path}")

    print(f"\nDone. {ok_count} test(s) passed.")
    return 0 if ok_count > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
