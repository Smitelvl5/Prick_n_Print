#!/usr/bin/env python3
"""
Prick_n_Print Raspberry Pi server.
HTTP API (ESP32-compatible) + web UI for commands, messages, images, and audio.
Everything sent to ESP32 is stored here; ESP32 can request anything at any time.
"""

import json
import shutil
import uuid
from pathlib import Path
from datetime import datetime

from flask import Flask, request, jsonify, send_from_directory, render_template

app = Flask(__name__)

BASE_DIR = Path(__file__).resolve().parent
UPLOADS_DIR = BASE_DIR / "uploads"
IMAGES_DIR = UPLOADS_DIR / "images"
AUDIO_DIR = UPLOADS_DIR / "audio"
DATA_FILE = BASE_DIR / "data.json"
SAMPLES_DIR = BASE_DIR / "samples"

for d in (IMAGES_DIR, AUDIO_DIR):
    d.mkdir(parents=True, exist_ok=True)


def load_data():
    if DATA_FILE.exists():
        try:
            with open(DATA_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            pass
    return {"commands": {}, "audio": {}, "images": {}, "reminders": {}, "config": {}, "messages": []}


def save_data(data):
    with open(DATA_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def get_data():
    data = load_data()
    for key in ("commands", "audio", "images", "reminders", "config", "messages"):
        if key not in data:
            data[key] = {} if key != "messages" else []
    return data


def seed_samples(data):
    """Ensure sample.jpg and sample.mp3 exist and are in the database."""
    base_url = ""  # Filled at request time
    changed = False

    # Copy sample files if missing
    for name, dst_dir in [("sample.jpg", IMAGES_DIR), ("sample.mp3", AUDIO_DIR)]:
        for src in [SAMPLES_DIR / name, BASE_DIR / name]:
            if src.exists():
                dst = dst_dir / name
                if not dst.exists():
                    shutil.copy2(src, dst)
                    changed = True
                break

    # Seed images if empty
    if not data["images"]:
        for f in IMAGES_DIR.iterdir():
            if f.is_file():
                cid = str(uuid.uuid4())[:8]
                data["images"][cid] = {
                    "url": f"/uploads/images/{f.name}",
                    "name": f.name,
                    "downloaded": False,
                }
                changed = True

    # Seed audio if empty
    if not data["audio"]:
        for f in AUDIO_DIR.iterdir():
            if f.is_file():
                cid = str(uuid.uuid4())[:8]
                data["audio"][cid] = {
                    "url": f"/uploads/audio/{f.name}",
                    "name": f.name,
                    "downloaded": False,
                }
                changed = True

    if changed:
        save_data(data)


def _base_url():
    return request.url_root.rstrip("/")


@app.after_request
def cors_headers(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET, PUT, POST, DELETE, OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response


@app.route("/<path:path>", methods=["OPTIONS"])
def options_cors(path):
    return "", 204


# ---------- Web UI ----------
@app.route("/")
def index():
    return render_template("index.html")


# ---------- API: History (web UI) ----------
@app.route("/api/history")
def get_history():
    data = get_data()
    msgs = data.get("messages", [])[-20:][::-1]
    images = [
        {"name": v["name"], "path": f"images/{v['name']}", "id": k}
        for k, v in data.get("images", {}).items()
    ][:20]
    audio = [
        {"name": v["name"], "path": f"audio/{v['name']}", "id": k}
        for k, v in data.get("audio", {}).items()
    ][:20]
    return jsonify({"messages": msgs, "images": images, "audio": audio})


# ---------- API: Commands (ESP32) ----------
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


# ---------- API: Audio (ESP32) ----------
@app.route("/api/audio", methods=["GET"])
def get_audio():
    data = get_data()
    # Fill full URLs for ESP32
    base = _base_url()
    result = {}
    for k, v in data["audio"].items():
        r = dict(v)
        if r["url"].startswith("/"):
            r["url"] = base + r["url"]
        result[k] = r
    return jsonify(result)


@app.route("/api/audio", methods=["POST"])
def post_audio():
    data = get_data()
    if request.content_type and "application/json" in (request.content_type or ""):
        body = request.get_json(force=True, silent=True) or {}
        url = body.get("url", "")
        name = body.get("name", "") or (url.split("/")[-1].split("?")[0] if url else "") or "audio.mp3"
        if not url or not name:
            return jsonify({"error": "url and name required"}), 400
        url = url if url.startswith("http") else _base_url() + (url if url.startswith("/") else "/uploads/audio/" + name)
    else:
        f = request.files.get("file") or request.files.get("audio")
        if not f or f.filename == "":
            return jsonify({"error": "file or JSON url/name required"}), 400
        name = request.form.get("name") or f.filename or "audio.mp3"
        safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "audio.mp3"
        path = AUDIO_DIR / safe
        f.save(path)
        url = f"{_base_url()}/uploads/audio/{safe}"
        name = safe
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


# ---------- API: Images (ESP32) ----------
@app.route("/api/images", methods=["GET"])
def get_images():
    data = get_data()
    base = _base_url()
    result = {}
    for k, v in data["images"].items():
        r = dict(v)
        if r["url"].startswith("/"):
            r["url"] = base + r["url"]
        result[k] = r
    return jsonify(result)


@app.route("/api/images", methods=["POST"])
def post_image():
    data = get_data()
    if request.content_type and "application/json" in (request.content_type or ""):
        body = request.get_json(force=True, silent=True) or {}
        url = body.get("url", "")
        name = body.get("name", "") or (url.split("/")[-1].split("?")[0] if url else "") or "image.jpg"
        if not url or not name:
            return jsonify({"error": "url and name required"}), 400
        url = url if url.startswith("http") else _base_url() + (url if url.startswith("/") else "/uploads/images/" + name)
    else:
        f = request.files.get("file") or request.files.get("image")
        if not f or f.filename == "":
            return jsonify({"error": "file or JSON url/name required"}), 400
        name = request.form.get("name") or f.filename or "image.jpg"
        safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "image.jpg"
        path = IMAGES_DIR / safe
        f.save(path)
        url = f"{_base_url()}/uploads/images/{safe}"
        name = safe
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


# ---------- API: Reminders & Config (ESP32) ----------
@app.route("/api/reminders", methods=["GET"])
def get_reminders():
    return jsonify(get_data()["reminders"])


@app.route("/api/reminders", methods=["PUT"])
def put_reminders():
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    data = get_data()
    data["reminders"] = body
    save_data(data)
    return jsonify({"ok": True})


@app.route("/api/config", methods=["GET"])
def get_config():
    return jsonify(get_data()["config"])


@app.route("/api/config", methods=["PUT"])
def put_config():
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    data = get_data()
    data["config"] = body
    save_data(data)
    return jsonify({"ok": True})


# ---------- Legacy /message, /image, /audio (web UI, shortcuts) ----------
@app.route("/message", methods=["POST"])
def receive_message():
    data = request.get_json(silent=True) or {}
    msg = data.get("message", request.form.get("message", ""))
    if not msg:
        return jsonify({"error": "No message provided"}), 400
    rec = get_data()
    rec["messages"].append({"text": msg, "at": datetime.utcnow().isoformat() + "Z"})
    rec["messages"] = rec["messages"][-500:]
    # Also add as command for ESP32 (print)
    cid = str(uuid.uuid4())[:8]
    rec["commands"][cid] = {"type": "print", "data": msg, "source": "shortcut"}
    save_data(rec)
    return jsonify({"status": "received", "message": msg}), 201


@app.route("/image", methods=["POST"])
def receive_image():
    f = request.files.get("image") or request.files.get("file")
    if not f or f.filename == "":
        return jsonify({"error": "No image provided"}), 400
    name = request.form.get("name") or f.filename or "image.jpg"
    safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "image.jpg"
    path = IMAGES_DIR / safe
    f.save(path)
    data = get_data()
    cid = str(uuid.uuid4())[:8]
    data["images"][cid] = {"url": f"{_base_url()}/uploads/images/{safe}", "name": safe, "downloaded": False}
    save_data(data)
    return jsonify({"status": "saved", "filename": safe}), 201


@app.route("/audio", methods=["POST"])
def receive_audio():
    f = request.files.get("audio") or request.files.get("file")
    if not f or f.filename == "":
        return jsonify({"error": "No audio provided"}), 400
    name = request.form.get("name") or f.filename or "audio.mp3"
    safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "audio.mp3"
    path = AUDIO_DIR / safe
    f.save(path)
    data = get_data()
    cid = str(uuid.uuid4())[:8]
    data["audio"][cid] = {"url": f"{_base_url()}/uploads/audio/{safe}", "name": safe, "downloaded": False}
    save_data(data)
    return jsonify({"status": "saved", "filename": safe}), 201


# ---------- File serving (ESP32 download) ----------
@app.route("/uploads/images/<path:name>")
def serve_image(name):
    return send_from_directory(IMAGES_DIR, name, as_attachment=request.args.get("dl") == "1")


@app.route("/uploads/audio/<path:name>")
def serve_audio(name):
    return send_from_directory(AUDIO_DIR, name, as_attachment=True)


# ---------- Health ----------
@app.route("/api/health")
def health():
    return jsonify({"status": "ok"})


# Seed samples on startup
_seed_data = get_data()
seed_samples(_seed_data)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
