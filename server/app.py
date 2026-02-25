#!/usr/bin/env python3
"""
TZT Backend: HTTP API for commands, audio, images, and reminders.
Replace Firebase: app/shortcuts POST here; ESP32 polls via GET.

Run: flask --app app run --host 0.0.0.0 --port 5000
Or: python app.py
"""

import json
import os
import uuid
from pathlib import Path

from flask import Flask, request, jsonify, send_from_directory

app = Flask(__name__)


@app.after_request
def cors_headers(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET, PUT, POST, DELETE, OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response


@app.route("/<path:path>", methods=["OPTIONS"])
def options_cors(path):
    return "", 204

# In-memory store (optionally persist to data.json)
DATA_FILE = Path(__file__).parent / "data.json"
UPLOADS_DIR = Path(__file__).parent / "uploads"
UPLOADS_AUDIO = UPLOADS_DIR / "audio"
UPLOADS_IMAGES = UPLOADS_DIR / "images"

for d in (UPLOADS_AUDIO, UPLOADS_IMAGES):
    d.mkdir(parents=True, exist_ok=True)


def load_data():
    if DATA_FILE.exists():
        try:
            with open(DATA_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            pass
    return {"commands": {}, "audio": {}, "images": {}, "reminders": {}, "config": {}}


def save_data(data):
    with open(DATA_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def get_data():
    data = load_data()
    for key in ("commands", "audio", "images", "reminders", "config"):
        if key not in data:
            data[key] = {}
    return data


# ---------- Commands ----------
@app.route("/api/commands", methods=["GET"])
def get_commands():
    data = get_data()
    return jsonify(data["commands"])


@app.route("/api/commands", methods=["POST"])
def post_command():
    data = get_data()
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    # Single object or array
    items = body if isinstance(body, list) else [body]
    ids = []
    for item in items:
        cid = str(uuid.uuid4())[:8]
        data["commands"][cid] = item
        ids.append(cid)
    save_data(data)
    return jsonify({"id": ids[0]} if len(ids) == 1 else {"ids": ids}), 201


@app.route("/api/commands/<cid>", methods=["DELETE"])
def delete_command(cid):
    data = get_data()
    if cid in data["commands"]:
        del data["commands"][cid]
        save_data(data)
    return "", 204


# ---------- Audio ----------
@app.route("/api/audio", methods=["GET"])
def get_audio():
    data = get_data()
    return jsonify(data["audio"])


@app.route("/api/audio", methods=["POST"])
def post_audio():
    data = get_data()
    if request.content_type and "application/json" in request.content_type:
        body = request.get_json(force=True, silent=True) or {}
        url = body.get("url", "")
        name = body.get("name", "")
        if not name and url:
            name = url.split("/")[-1].split("?")[0] or "audio.mp3"
        if not url or not name:
            return jsonify({"error": "url and name required"}), 400
    else:
        # Multipart: file upload
        f = request.files.get("file")
        if not f:
            return jsonify({"error": "file or JSON url/name required"}), 400
        name = request.form.get("name") or f.filename or "audio.mp3"
        ext = Path(name).suffix or ".mp3"
        if not name.endswith(ext):
            name = name + ext
        path = UPLOADS_AUDIO / name
        f.save(path)
        url = f"{request.url_root.rstrip('/')}/uploads/audio/{name}"
    cid = str(uuid.uuid4())[:8]
    data["audio"][cid] = {"url": url, "name": name, "downloaded": False}
    save_data(data)
    return jsonify({"id": cid}), 201


@app.route("/api/audio/<cid>", methods=["PUT"])
def put_audio(cid):
    data = get_data()
    if cid not in data["audio"]:
        return "", 404
    body = request.get_json(force=True, silent=True) or {}
    if body.get("downloaded") is True:
        data["audio"][cid]["downloaded"] = True
        save_data(data)
    return jsonify(data["audio"][cid])


# ---------- Images ----------
@app.route("/api/images", methods=["GET"])
def get_images():
    data = get_data()
    return jsonify(data["images"])


@app.route("/api/images", methods=["POST"])
def post_image():
    data = get_data()
    if request.content_type and "application/json" in request.content_type:
        body = request.get_json(force=True, silent=True) or {}
        url = body.get("url", "")
        name = body.get("name", "")
        if not name and url:
            name = url.split("/")[-1].split("?")[0] or "image.jpg"
        if not url or not name:
            return jsonify({"error": "url and name required"}), 400
    else:
        f = request.files.get("file")
        if not f:
            return jsonify({"error": "file or JSON url/name required"}), 400
        name = request.form.get("name") or f.filename or "image.jpg"
        ext = Path(name).suffix or ".jpg"
        if not name.endswith(ext):
            name = name + ext
        path = UPLOADS_IMAGES / name
        f.save(path)
        url = f"{request.url_root.rstrip('/')}/uploads/images/{name}"
    cid = str(uuid.uuid4())[:8]
    data["images"][cid] = {"url": url, "name": name, "downloaded": False}
    save_data(data)
    return jsonify({"id": cid}), 201


@app.route("/api/images/<cid>", methods=["PUT"])
def put_image(cid):
    data = get_data()
    if cid not in data["images"]:
        return "", 404
    body = request.get_json(force=True, silent=True) or {}
    if body.get("downloaded") is True:
        data["images"][cid]["downloaded"] = True
        save_data(data)
    return jsonify(data["images"][cid])


# ---------- Reminders ----------
@app.route("/api/reminders", methods=["GET"])
def get_reminders():
    data = get_data()
    return jsonify(data["reminders"])


@app.route("/api/reminders", methods=["PUT"])
def put_reminders():
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    data = get_data()
    data["reminders"] = body
    save_data(data)
    return jsonify({"ok": True})


# ---------- Config (optional) ----------
@app.route("/api/config", methods=["GET"])
def get_config():
    data = get_data()
    return jsonify(data["config"])


@app.route("/api/config", methods=["PUT"])
def put_config():
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    data = get_data()
    data["config"] = body
    save_data(data)
    return jsonify({"ok": True})


# ---------- Uploaded files (for ESP32 to download by URL) ----------
@app.route("/uploads/audio/<path:name>")
def uploads_audio(name):
    return send_from_directory(UPLOADS_AUDIO, name)


@app.route("/uploads/images/<path:name>")
def uploads_images(name):
    return send_from_directory(UPLOADS_IMAGES, name)


# ---------- Health ----------
@app.route("/api/health")
def health():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
