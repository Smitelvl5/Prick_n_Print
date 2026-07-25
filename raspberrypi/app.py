#!/usr/bin/env python3
"""
Prick_n_Print Raspberry Pi server.
HTTP API (ESP32-compatible) + web UI for commands, messages, images, and audio.
Everything sent to ESP32 is stored here; ESP32 can request anything at any time.
"""

import json
import os
import uuid
from pathlib import Path
from datetime import datetime

try:
    import fcntl
    HAS_FCNTL = True
except ImportError:
    HAS_FCNTL = False  # Windows has no fcntl

from flask import Flask, request, jsonify, send_from_directory, render_template

app = Flask(__name__)

BASE_DIR = Path(__file__).resolve().parent
UPLOADS_DIR = BASE_DIR / "uploads"
IMAGES_DIR = UPLOADS_DIR / "images"
AUDIO_DIR = UPLOADS_DIR / "audio"
DATA_FILE = BASE_DIR / "data.json"
LOCK_FILE = BASE_DIR / "data.json.lock"

for d in (IMAGES_DIR, AUDIO_DIR):
    d.mkdir(parents=True, exist_ok=True)


def load_data():
    if not DATA_FILE.exists():
        return {"commands": {}, "audio": {}, "images": {}, "reminders": {}, "config": {}, "messages": []}
    try:
        LOCK_FILE.touch(exist_ok=True)
        with open(LOCK_FILE, "r") as lf:
            if HAS_FCNTL:
                fcntl.flock(lf.fileno(), fcntl.LOCK_SH)
            try:
                with open(DATA_FILE, "r", encoding="utf-8") as f:
                    return json.load(f)
            finally:
                if HAS_FCNTL:
                    fcntl.flock(lf.fileno(), fcntl.LOCK_UN)
    except (json.JSONDecodeError, IOError):
        pass
    return {"commands": {}, "audio": {}, "images": {}, "reminders": {}, "config": {}, "messages": []}


def save_data(data):
    LOCK_FILE.touch(exist_ok=True)
    with open(LOCK_FILE, "r") as lf:
        if HAS_FCNTL:
            fcntl.flock(lf.fileno(), fcntl.LOCK_EX)
        try:
            tmp = DATA_FILE.with_suffix(".json.tmp")
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
                f.flush()
                os.fsync(f.fileno())
            tmp.rename(DATA_FILE)  # Atomic on POSIX
        finally:
            if HAS_FCNTL:
                fcntl.flock(lf.fileno(), fcntl.LOCK_UN)


def get_data():
    data = load_data()
    for key in ("commands", "audio", "images", "reminders", "config", "messages", "devices"):
        if key not in data:
            data[key] = {} if key != "messages" else []
    return data


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
def _sort_by_date(items, key="at", newest_first=True):
    """Sort list of dicts by date key; items without key go last."""
    def sort_key(x):
        v = x.get(key) or ""
        return (0 if v else 1, v)
    return sorted(items, key=sort_key, reverse=newest_first)


@app.route("/api/history")
def get_history():
    data = get_data()
    # Messages: pending commands (type=print) - what ESP32 will receive
    commands = data.get("commands", {})
    msgs = [
        {"id": k, "text": v.get("data", ""), "at": v.get("at", ""), "source": v.get("source", "")}
        for k, v in commands.items()
        if v.get("type") == "print"
    ]
    msgs = _sort_by_date(msgs, key="at")[:50]
    images = [
        {"name": v["name"], "path": f"images/{v['name']}", "id": k, "at": v.get("at", "")}
        for k, v in data.get("images", {}).items()
    ]
    audio = [
        {"name": v["name"], "path": f"audio/{v['name']}", "id": k, "at": v.get("at", "")}
        for k, v in data.get("audio", {}).items()
    ]
    images = _sort_by_date(images)[:50]
    audio = _sort_by_date(audio)[:50]
    devices = data.get("devices", {})
    return jsonify({"messages": msgs, "images": images, "audio": audio, "devices": devices})


@app.route("/api/history/<hist_type>", methods=["DELETE"])
def delete_history(hist_type):
    """Clear messages, images, or audio. hist_type: messages | commands | images | audio | all"""
    data = get_data()
    changed = False
    if hist_type in ("messages", "all"):
        if data.get("messages"):
            data["messages"] = []
            changed = True
        # Also clear pending commands (what ESP32 polls)
        if data.get("commands"):
            data["commands"] = {}
            changed = True
    if hist_type in ("commands", "all") and hist_type != "messages":
        if data.get("commands"):
            data["commands"] = {}
            changed = True
    if hist_type in ("images", "all"):
        for v in list(data.get("images", {}).values()):
            fpath = IMAGES_DIR / v.get("name", "")
            if fpath.exists():
                try:
                    fpath.unlink()
                except OSError:
                    pass
        if data.get("images"):
            data["images"] = {}
            changed = True
    if hist_type in ("audio", "all"):
        for v in list(data.get("audio", {}).values()):
            fpath = AUDIO_DIR / v.get("name", "")
            if fpath.exists():
                try:
                    fpath.unlink()
                except OSError:
                    pass
        if data.get("audio"):
            data["audio"] = {}
            changed = True
    if changed:
        save_data(data)
    return jsonify({"ok": True, "cleared": hist_type}), 200


# ---------- API: Commands (ESP32) ----------
@app.route("/api/commands", methods=["GET"])
def get_commands():
    data = get_data()
    return jsonify(data["commands"])


@app.route("/api/commands", methods=["POST"])
def post_command():
    data = get_data()
    body = request.get_json(force=True, silent=True)
    # iOS Shortcuts sometimes sends JSON as a string; parse it
    if isinstance(body, str):
        try:
            body = json.loads(body)
        except json.JSONDecodeError:
            return jsonify({"error": "Invalid JSON"}), 400
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    items = body if isinstance(body, list) else [body]
    ids = []
    now = datetime.utcnow().isoformat() + "Z"
    for item in items:
        if isinstance(item, str):
            try:
                item = json.loads(item)
            except json.JSONDecodeError:
                continue
        if not isinstance(item, dict):
            continue
        # iOS Shortcuts sometimes sends the command as a single key with JSON string value
        if "type" not in item and len(item) >= 1:
            keys = [k for k in item.keys() if isinstance(k, str) and k.strip().startswith("{")]
            if len(keys) == 1:
                try:
                    item = json.loads(keys[0])
                    if not isinstance(item, dict):
                        continue
                except json.JSONDecodeError:
                    pass
        cid = str(uuid.uuid4())[:8]
        if "at" not in item:
            item = dict(item)
            item["at"] = now
        data["commands"][cid] = item
        ids.append(cid)
    if not ids:
        return jsonify({"error": "No valid commands"}), 400
    save_data(data)
    return jsonify({"id": ids[0]} if len(ids) == 1 else {"ids": ids}), 201


@app.route("/api/commands/<cid>", methods=["DELETE"], strict_slashes=False)
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
        if f and f.filename:
            name = request.form.get("name") or f.filename or "audio.mp3"
        else:
            # iOS Shortcuts "File" body: raw bytes with Content-Type audio/*
            ct = (request.content_type or "").lower()
            if "audio/" in ct or request.get_data():
                ext = "m4a" if "m4a" in ct or "aac" in ct else "mp3"
                name = f"voice_{datetime.utcnow().strftime('%Y%m%d_%H%M%S')}.{ext}"
                raw = request.get_data()
                if not raw:
                    return jsonify({"error": "No audio data"}), 400
                safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "audio.mp3"
                path = AUDIO_DIR / safe
                path.write_bytes(raw)
                url = f"{_base_url()}/uploads/audio/{safe}"
                data["audio"][str(uuid.uuid4())[:8]] = {
                    "url": url, "name": safe, "downloaded": False,
                    "at": datetime.utcnow().isoformat() + "Z"
                }
                save_data(data)
                return jsonify({"id": "ok"}), 201
            return jsonify({"error": "file or JSON url/name required"}), 400
        safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "audio.mp3"
        path = AUDIO_DIR / safe
        f.save(path)
        url = f"{_base_url()}/uploads/audio/{safe}"
        name = safe
    cid = str(uuid.uuid4())[:8]
    data["audio"][cid] = {
        "url": url, "name": name, "downloaded": False,
        "at": datetime.utcnow().isoformat() + "Z"
    }
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
        if f and f.filename:
            name = request.form.get("name") or f.filename or "image.jpg"
        else:
            # iOS Shortcuts "File" body: raw bytes with Content-Type image/*
            ct = (request.content_type or "").lower()
            if "image/" in ct or request.get_data():
                ext = "png" if "png" in ct else "jpg"
                name = f"photo_{datetime.utcnow().strftime('%Y%m%d_%H%M%S')}.{ext}"
                raw = request.get_data()
                if not raw:
                    return jsonify({"error": "No image data"}), 400
                safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "image.jpg"
                path = IMAGES_DIR / safe
                path.write_bytes(raw)
                url = f"{_base_url()}/uploads/images/{safe}"
                data["images"][str(uuid.uuid4())[:8]] = {
                    "url": url, "name": safe, "downloaded": False,
                    "at": datetime.utcnow().isoformat() + "Z"
                }
                save_data(data)
                return jsonify({"id": "ok"}), 201
            return jsonify({"error": "file or JSON url/name required"}), 400
        safe = "".join(c for c in name if c.isalnum() or c in ".-_") or "image.jpg"
        path = IMAGES_DIR / safe
        f.save(path)
        url = f"{_base_url()}/uploads/images/{safe}"
        name = safe
    cid = str(uuid.uuid4())[:8]
    data["images"][cid] = {
        "url": url, "name": name, "downloaded": False,
        "at": datetime.utcnow().isoformat() + "Z"
    }
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


@app.route("/api/images/<cid>", methods=["DELETE"])
def delete_image(cid):
    data = get_data()
    if cid not in data["images"]:
        return "", 404
    entry = data["images"][cid]
    fpath = IMAGES_DIR / entry.get("name", "")
    if fpath.exists():
        try:
            fpath.unlink()
        except OSError:
            pass
    del data["images"][cid]
    save_data(data)
    return "", 204


@app.route("/api/audio/<cid>", methods=["DELETE"])
def delete_audio(cid):
    data = get_data()
    if cid not in data["audio"]:
        return "", 404
    entry = data["audio"][cid]
    fpath = AUDIO_DIR / entry.get("name", "")
    if fpath.exists():
        try:
            fpath.unlink()
        except OSError:
            pass
    del data["audio"][cid]
    save_data(data)
    return "", 204


# ---------- API: Devices (heartbeat for visibility) ----------
@app.route("/api/devices", methods=["GET"])
def get_devices():
    return jsonify(get_data()["devices"])


@app.route("/api/devices/heartbeat", methods=["POST"])
def device_heartbeat():
    """ESP32 can POST {"device_id": "tzt-display", "name": "TZT Display"} to show in web UI."""
    body = request.get_json(force=True, silent=True) or {}
    device_id = body.get("device_id") or request.headers.get("X-Device-Id", "").strip()
    name = body.get("name") or device_id or "Unknown"
    if not device_id:
        return jsonify({"error": "device_id required"}), 400
    data = get_data()
    data["devices"][device_id] = {
        "name": name,
        "last_seen": datetime.utcnow().isoformat() + "Z",
    }
    save_data(data)
    return jsonify({"ok": True}), 200


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
    rec["commands"][cid] = {"type": "print", "data": msg, "source": "shortcut", "at": datetime.utcnow().isoformat() + "Z"}
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
    data["images"][cid] = {
        "url": f"{_base_url()}/uploads/images/{safe}",
        "name": safe,
        "downloaded": False,
        "at": datetime.utcnow().isoformat() + "Z",
    }
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
    data["audio"][cid] = {
        "url": f"{_base_url()}/uploads/audio/{safe}",
        "name": safe,
        "downloaded": False,
        "at": datetime.utcnow().isoformat() + "Z",
    }
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


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
