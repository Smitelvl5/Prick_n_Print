#!/usr/bin/env python3
"""
Replicate the iOS Shortcut image flow: upload image to Firebase Storage,
then add an entry to Realtime Database so the TZT downloads and shows it.

Usage:
  pip install requests
  python scripts/upload_image_to_firebase.py
  python scripts/upload_image_to_firebase.py path/to/image.jpg
  python scripts/upload_image_to_firebase.py path/to/image.jpg --name my-sketch.jpg

  With no arguments, uses scripts/test.jpg and name test.jpg.

Uses the same project as config_tzt.h (printerpot-d96f8).
"""

import argparse
import os
import sys
import urllib.parse

# Default: scripts/test.jpg relative to project root (parent of scripts/)
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = os.path.dirname(_SCRIPT_DIR)
DEFAULT_IMAGE = os.path.join(_PROJECT_ROOT, "scripts", "test.jpg")

try:
    import requests
except ImportError:
    print("Install requests: pip install requests", file=sys.stderr)
    sys.exit(1)

# Match include/config_tzt.h
PROJECT_ID = "printerpot-d96f8"
BUCKET = f"{PROJECT_ID}.appspot.com"
REALTIME_DB_URL = f"https://{PROJECT_ID}-default-rtdb.firebaseio.com"
# Path in Storage (folder/filename). Use %2F for /
STORAGE_PATH = "sketches/test.jpg"


def upload_to_storage(image_path: str, storage_name: str) -> str:
    """Upload image to Firebase Storage. Returns public download URL."""
    with open(image_path, "rb") as f:
        data = f.read()

    # Infer content type
    lower = image_path.lower()
    if lower.endswith(".png"):
        content_type = "image/png"
    else:
        content_type = "image/jpeg"

    # Use Google Cloud Storage JSON API (Firebase Storage uses this for uploads).
    # firebasestorage.googleapis.com/v0/b/... returns 404 for upload; GCS endpoint is correct.
    encoded_name = urllib.parse.quote(storage_name, safe="")
    url = f"https://storage.googleapis.com/upload/storage/v1/b/{BUCKET}/o?uploadType=media&name={encoded_name}"

    resp = requests.post(url, data=data, headers={"Content-Type": content_type}, timeout=30)
    if resp.status_code == 404:
        print("Error: Firebase Storage returned 404 (bucket not found).", file=sys.stderr)
        print("Enable Storage: Firebase Console → Build → Storage → Get started", file=sys.stderr)
        print("Create a bucket (same region as Realtime Database), then run this script again.", file=sys.stderr)
        sys.exit(1)
    resp.raise_for_status()
    obj = resp.json()

    # GCS returns mediaLink (may require auth to download). Prefer Firebase public URL if we have a token.
    token = obj.get("downloadTokens") or (obj.get("metadata") or {}).get("firebaseStorageDownloadTokens")
    if token:
        name_encoded = urllib.parse.quote(storage_name, safe="")
        return f"https://firebasestorage.googleapis.com/v0/b/{BUCKET}/o/{name_encoded}?alt=media&token={token}"
    media = obj.get("mediaLink")
    if media:
        return media
    raise RuntimeError("No download URL in response. Keys: " + ", ".join(obj.keys()))


def add_to_realtime_db(download_url: str, name: str) -> None:
    """Add image entry to Realtime Database so TZT polls and downloads it."""
    payload = {"url": download_url, "name": name}
    url = f"{REALTIME_DB_URL}/images.json"
    resp = requests.post(url, json=payload, timeout=10)
    resp.raise_for_status()
    key = resp.json()
    print(f"Added to Realtime Database: /images/{key}")


def main():
    parser = argparse.ArgumentParser(description="Upload image to Firebase (Storage + Realtime DB) like the Shortcut.")
    parser.add_argument("image", nargs="?", default=DEFAULT_IMAGE,
                        help=f"Path to image file (.jpg or .png). Default: scripts/test.jpg")
    parser.add_argument("--name", "-n", help="Filename for TZT (e.g. test.jpg). Default: from image path.")
    parser.add_argument("--storage-path", default=STORAGE_PATH,
                        help=f"Path in Storage (folder/file). Default: {STORAGE_PATH}")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        print(f"Error: image file not found: {args.image}", file=sys.stderr)
        print("Put a JPEG named test.jpg in the scripts/ folder, or pass a path.", file=sys.stderr)
        return 1

    name = args.name
    if not name:
        name = os.path.basename(args.image)
    storage_name = args.storage_path

    print(f"Uploading {args.image} to Storage as {storage_name} ...")
    download_url = upload_to_storage(args.image, storage_name)
    print(f"Download URL: {download_url[:80]}...")

    print(f"Adding to Realtime DB with name={name} ...")
    add_to_realtime_db(download_url, name)
    print("Done. TZT will pick it up on next poll and save to /data/images/media/")

    return 0


if __name__ == "__main__":
    sys.exit(main())
