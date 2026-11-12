#!/usr/bin/env python3
"""
Prick_n_Print Raspberry Pi server.
HTTP API (ESP32-compatible) + web UI for commands, messages, images, and audio.
Everything sent to ESP32 is stored here; ESP32 can request anything at any time.

Run (dev):  python app.py
Run (prod): gunicorn -w 2 -k gthread --threads 8 -b 0.0.0.0:5000 app:app   (see setup.sh)
"""

import contextlib
import functools
import hashlib
import hmac
import json
import os
import shutil
import subprocess
import threading
import time
import uuid
from datetime import datetime
from glob import escape as glob_escape
from pathlib import Path

try:
    import fcntl
    HAS_FCNTL = True
except ImportError:
    HAS_FCNTL = False  # Windows has no fcntl

from flask import Flask, jsonify, render_template, request, send_from_directory
from werkzeug.utils import secure_filename

app = Flask(__name__)
# UTF-8 in JSON instead of \uXXXX escapes: a non-English message would otherwise reach the TZT up to
# 6x its printed size, and the TZT has to hold each command response in RAM
app.json.ensure_ascii = False

BASE_DIR = Path(__file__).resolve().parent
UPLOADS_DIR = BASE_DIR / "uploads"
IMAGES_DIR = UPLOADS_DIR / "images"
AUDIO_DIR = UPLOADS_DIR / "audio"
FIRMWARE_DIR = UPLOADS_DIR / "firmware"
DATA_FILE = BASE_DIR / "data.json"
LOCK_FILE = BASE_DIR / "data.json.lock"
COMMANDS_MAX_WAIT_S = 25     # GET /api/commands?wait= cap
COMMANDS_WAIT_CHECK_S = 0.2  # how often a waiting poll looks for new commands
# Longest message the printer can take: the main board reassembles at most 64 ESP-NOW pieces of
# 199 bytes (PRINT_CHUNK_MAX in src/main.cpp) and drops anything longer without printing it.
PRINT_MAX_BYTES = 64 * 199
COMMAND_MAX_BYTES = 16 * 1024  # whole command as sent to the TZT (it holds one in RAM at a time)
# Firmware the Pi hands out (scripts/publish_firmware.py uploads it). Key = the device's X-Device-Id.
# Value = the device's app partition size: TZT min_spiffs.csv (1.875 MB), Main default.csv (1.25 MB).
# Main never polls /api/commands: it checks GET /api/firmware/main-board itself, and the TZT
# relays the queued update command to it over ESP-NOW so it checks straight away.
FIRMWARE_DEVICES = {"tzt-display": 0x1E0000, "main-board": 0x140000}
FIRMWARE_RETRY_S = 30 * 60     # a device still on old firmware gets the update offered again after this

# Shared secret required (as header X-API-Key) on every state-changing request.
# Set in the environment (see setup.sh / systemd unit); the server refuses to
# start without one so a fresh deploy can't accidentally run wide open.
API_KEY = os.environ.get("PRICK_N_PRINT_API_KEY", "")
if not API_KEY:
    raise RuntimeError(
        "PRICK_N_PRINT_API_KEY is not set. Generate one (e.g. `python -c "
        "\"import secrets; print(secrets.token_hex(32))\"`) and set it in the "
        "environment before starting the server."
    )

ALLOWED_ORIGINS = {
    o.strip() for o in os.environ.get("PRICK_N_PRINT_CORS_ORIGINS", "").split(",") if o.strip()
}

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".heic", ".heif"}  # all converted to JPEG
AUDIO_EXTENSIONS = {".mp3", ".wav", ".m4a", ".aac", ".ogg"}

for d in (IMAGES_DIR, AUDIO_DIR):
    d.mkdir(parents=True, exist_ok=True)

# Device "last seen" times change on every TZT poll, so they live in a small file on tmpfs (RAM)
# instead of data.json, which is fsync'd to the SD card. Lost on reboot, which is fine for last-seen.
RUNTIME_DIR = Path(
    os.environ.get("PRICK_N_PRINT_RUNTIME_DIR")
    or ("/dev/shm" if os.path.isdir("/dev/shm") else BASE_DIR)
)
DEVICES_FILE = RUNTIME_DIR / "prick_n_print_devices.json"


def require_api_key(fn):
    """Guards state-changing routes. ESP32 devices and the web UI send X-API-Key."""
    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        provided = request.headers.get("X-API-Key", "")
        if not provided or not hmac.compare_digest(provided, API_KEY):
            return jsonify({"error": "Unauthorized"}), 401
        return fn(*args, **kwargs)
    return wrapper


TZT_SCREEN = (320, 240)


def _to_tzt_jpeg(path):
    """Fit an image (any Pillow-readable format, HEIC with pillow-heif) onto the TZT's 320x240
    screen as a baseline JPEG - its decoder can't do progressive, PNG or full-size photos."""
    from PIL import Image, ImageOps
    try:
        from pillow_heif import register_heif_opener
        register_heif_opener()
    except ImportError:
        pass
    with Image.open(path) as im:
        im = ImageOps.exif_transpose(im).convert("RGB")
        im = ImageOps.contain(im, TZT_SCREEN)
        canvas = Image.new("RGB", TZT_SCREEN, (0, 0, 0))
        canvas.paste(im, ((TZT_SCREEN[0] - im.width) // 2, (TZT_SCREEN[1] - im.height) // 2))
    out = path.with_suffix(".jpg")
    canvas.save(out, "JPEG", quality=85, progressive=False)
    if out != path:
        path.unlink(missing_ok=True)
    return out


def _to_mp3(path):
    """The TZT plays MP3/WAV only; iPhone voice memos are m4a/aac."""
    if path.suffix.lower() in (".mp3", ".wav") or not shutil.which("ffmpeg"):
        return path
    out = path.with_suffix(".mp3")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(path),
                    "-ac", "1", "-ar", "22050", "-b:a", "64k", str(out)],
                   check=True, timeout=120)
    path.unlink(missing_ok=True)
    return out


def prepare_for_tzt(kind, path):
    """Convert a saved upload into something the TZT can show/play; returns the file to serve.
    Keeps the original if the converter isn't installed or the file can't be read."""
    try:
        return _to_tzt_jpeg(path) if kind == "images" else _to_mp3(path)
    except Exception as e:  # noqa: BLE001 - a bad upload must not fail the request
        app.logger.warning("Could not convert %s for the TZT: %s", path.name, e)
        return path


def safe_upload_name(name, allowed_extensions, default_name):
    """Sanitize a client-supplied filename: strip path components, whitelist extension."""
    candidate = secure_filename(name or "") or default_name
    ext = Path(candidate).suffix.lower()
    if ext not in allowed_extensions:
        candidate = Path(default_name).stem + Path(default_name).suffix
    return candidate


def _read_data_file():
    """data.json with every top-level key present. Caller holds the lock."""
    data = {}
    try:
        with open(DATA_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        pass
    for key in ("commands", "audio", "images", "reminders", "config", "messages"):
        if key not in data:
            data[key] = {} if key != "messages" else []
    return data


def _write_data_file(data):
    """Caller holds the exclusive lock."""
    tmp = DATA_FILE.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, DATA_FILE)  # Atomic; unlike Path.rename(), also overwrites on Windows


@contextlib.contextmanager
def _data_lock(mode):
    LOCK_FILE.touch(exist_ok=True)
    with open(LOCK_FILE, "r") as lf:  # own open file per call, so flock also excludes other threads
        if HAS_FCNTL:
            fcntl.flock(lf.fileno(), mode)
        try:
            yield
        finally:
            if HAS_FCNTL:
                fcntl.flock(lf.fileno(), fcntl.LOCK_UN)


def get_data():
    """Read-only snapshot. To change data, use update_data(): saving a snapshot would overwrite
    whatever another request saved in between."""
    with _data_lock(fcntl.LOCK_SH if HAS_FCNTL else None):
        return _read_data_file()


@contextlib.contextmanager
def update_data():
    """Read, change and save data.json under one exclusive lock. gunicorn runs 2 workers x 8
    threads, so without it two requests (say the TZT's ack and a new message) each load the file,
    change their own copy, and the second save silently undoes the first: a lost message, or a
    command that comes back and prints twice. Saves only if the block changed something; nothing is
    saved if it raises. Keep slow work (image/audio conversion) outside the block."""
    with _data_lock(fcntl.LOCK_EX if HAS_FCNTL else None):
        data = _read_data_file()
        before = json.dumps(data, sort_keys=True)
        yield data
        if json.dumps(data, sort_keys=True) != before:
            _write_data_file(data)


def _data_mtime():
    try:
        return DATA_FILE.stat().st_mtime_ns
    except OSError:
        return 0


def _base_url():
    return request.url_root.rstrip("/")


def _now():
    return datetime.utcnow().isoformat() + "Z"


def load_devices():
    try:
        with open(DEVICES_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def touch_device(device_id, name):
    """Best effort: a failed last-seen update must not fail the poll that carries it."""
    devices = load_devices()
    devices[device_id] = {"name": name, "last_seen": _now()}
    # Per process and thread: gunicorn runs 2 workers x 8 threads, all of which may write at once
    tmp = DEVICES_FILE.with_suffix(f".{os.getpid()}.{threading.get_ident()}.tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(devices, f)
        os.replace(tmp, DEVICES_FILE)
    except OSError as e:
        app.logger.warning("Could not update device last-seen: %s", e)


def _add_media(data, kind, url, name):
    """Record an upload (kind: "audio" | "images") and queue a download command for it, so the TZT
    fetches it on its next commands poll instead of polling /api/audio and /api/images. Not silent:
    the TZT plays a voice message / shows a picture as soon as it arrives.
    The command shares the media id, so deleting the media can drop its pending download too."""
    mid = str(uuid.uuid4())[:8]
    now = _now()
    data[kind][mid] = {"url": url, "name": name, "downloaded": False, "at": now}
    data["commands"][mid] = {
        "type": "download_audio" if kind == "audio" else "download_image",
        "url": url, "name": name, "media_id": mid, "at": now,
    }
    return mid


def _drop_download_commands(data, command_type):
    data["commands"] = {
        k: v for k, v in data["commands"].items()
        if not (v.get("type") == command_type and v.get("media_id"))
    }


@app.before_request
def reject_wrong_api_key():
    """A request that carries X-API-Key must carry the right one, on every route. nginx lets
    requests that have the header skip its password prompt (so the ESP32s and Shortcuts work over
    Tailscale Funnel); this check is what stops a made-up key from getting past it."""
    provided = request.headers.get("X-API-Key")
    if provided is not None and not hmac.compare_digest(provided, API_KEY):
        return jsonify({"error": "Unauthorized"}), 401
    return None


@app.after_request
def cors_headers(response):
    origin = request.headers.get("Origin", "")
    if origin and (origin in ALLOWED_ORIGINS or "*" in ALLOWED_ORIGINS):
        response.headers["Access-Control-Allow-Origin"] = origin
        response.headers["Vary"] = "Origin"
    response.headers["Access-Control-Allow-Methods"] = "GET, PUT, POST, DELETE, OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type, X-API-Key"
    return response


@app.route("/<path:path>", methods=["OPTIONS"])
def options_cors(path):
    return "", 204


# ---------- Web UI ----------
@app.route("/")
def index():
    # The web UI is served from this same origin and needs the key to call the
    # state-changing API routes below; access to the page itself is gated by
    # the nginx Basic Auth / IP allowlist configured in setup.sh.
    return render_template("index.html", api_key=API_KEY)


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
    # Messages: permanent log of everything sent (survives the TZT deleting the pending command after printing)
    msgs = _sort_by_date(list(data.get("messages", [])), key="at")[:50]
    # Delivery state for the page: "delivered_at" (the TZT took it) and "failed" (the TZT skipped it)
    # are stored on the message; still queued means the TZT hasn't picked it up yet.
    msgs = [dict(m, pending=m.get("id") in data["commands"]) for m in msgs]
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
    return jsonify({"messages": msgs, "images": images, "audio": audio, "devices": load_devices()})


def _unlink_quietly(path):
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


@app.route("/api/history/<hist_type>", methods=["DELETE"])
@require_api_key
def delete_history(hist_type):
    """Clear messages, images, or audio. hist_type: messages | commands | images | audio | all"""
    files = []
    with update_data() as data:
        if hist_type in ("messages", "all"):
            data["messages"] = []
            # and the messages still waiting to print - not pending downloads, alarms or controls
            data["commands"] = {k: v for k, v in data["commands"].items() if v.get("type") != "print"}
        if hist_type == "commands":
            data["commands"] = {}
        if hist_type in ("images", "all"):
            files += [IMAGES_DIR / v.get("name", "") for v in data["images"].values()]
            data["images"] = {}
            _drop_download_commands(data, "download_image")
        if hist_type in ("audio", "all"):
            files += [AUDIO_DIR / v.get("name", "") for v in data["audio"].values()]
            data["audio"] = {}
            _drop_download_commands(data, "download_audio")
    for f in files:
        if f.name:
            _unlink_quietly(f)
    return jsonify({"ok": True, "cleared": hist_type}), 200


# ---------- API: Commands (ESP32) ----------
def _id_list(arg):
    return [cid for cid in request.args.get(arg, "").split(",") if cid]


def _mark_messages(data, ids, **fields):
    """Record delivery state on the sent-messages log entries that share these command ids."""
    ids = set(ids)
    for m in data["messages"]:
        if m.get("id") in ids:
            m.update(fields)


@app.route("/api/commands", methods=["GET"])
@require_api_key
def get_commands():
    """Hands out the oldest pending command (one per poll), as {id: command}, with its id also in
    the X-Command-Id header: if the TZT can't parse the body, it can still skip that command by id
    instead of failing the same poll forever with everything else stuck behind it.
    ?ack=id1,id2: commands the device handled since its last poll (deleted here, saving it a DELETE
    request and TLS handshake per command). ?skip=id: ones it received but couldn't handle."""
    # Devices send X-Device-Id on every poll, which doubles as their heartbeat
    device_id = request.headers.get("X-Device-Id", "").strip()
    if device_id:
        touch_device(device_id, request.headers.get("X-Device-Name", "").strip() or device_id)
        _offer_firmware_again(device_id, request.headers.get("X-Firmware", "").strip())
    acked, skipped = _id_list("ack"), _id_list("skip")
    if acked or skipped:
        now = _now()
        with update_data() as data:
            done = [cid for cid in dict.fromkeys(acked + skipped) if cid in data["commands"]]
            for cid in done:
                del data["commands"][cid]
            _mark_messages(data, [c for c in acked if c in done and c not in skipped], delivered_at=now)
            _mark_messages(data, [c for c in skipped if c in done], failed="The TZT could not read this message")
    else:
        data = get_data()
    # Long poll: ?wait=N holds an empty answer up to N s (max 25, under nginx's 60 s read timeout)
    # and returns as soon as a command is queued, so controls reach the TZT in about a second
    # instead of on its next 30 s poll. Needs threaded gunicorn workers (see setup.sh).
    wait = min(max(request.args.get("wait", 0, type=float), 0), COMMANDS_MAX_WAIT_S)
    deadline = time.monotonic() + wait
    mtime = _data_mtime()
    while not data["commands"] and time.monotonic() < deadline:
        time.sleep(COMMANDS_WAIT_CHECK_S)
        if _data_mtime() != mtime:  # a save from any worker replaces the file
            mtime = _data_mtime()
            data = get_data()
    if not data["commands"]:
        return jsonify({})
    cid, cmd = next(iter(data["commands"].items()))  # dicts keep insertion order: oldest first
    resp = jsonify({cid: cmd})
    resp.headers["X-Command-Id"] = cid
    return resp


def _command_too_big(item):
    """Error text if the TZT/printer can't take this command, else None."""
    if item.get("type") == "print":
        size = len(str(item.get("data", "")).encode("utf-8"))
        if size > PRINT_MAX_BYTES:
            return (f"Message too long to print: {size} bytes, the limit is {PRINT_MAX_BYTES} "
                    f"(about {PRINT_MAX_BYTES // 1000} thousand English letters).")
    if len(json.dumps(item, ensure_ascii=False).encode("utf-8")) > COMMAND_MAX_BYTES:
        return f"Command too large (limit {COMMAND_MAX_BYTES} bytes)."
    return None


@app.route("/api/commands", methods=["POST"])
@require_api_key
def post_command():
    body = request.get_json(force=True, silent=True)
    # iOS Shortcuts sometimes sends JSON as a string; parse it
    if isinstance(body, str):
        try:
            body = json.loads(body)
        except json.JSONDecodeError:
            return jsonify({"error": "Invalid JSON"}), 400
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    items = []
    for item in body if isinstance(body, list) else [body]:
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
        # Reject up front, so the sender sees why, rather than queueing something that won't print
        err = _command_too_big(item)
        if err:
            return jsonify({"error": err}), 413
        items.append(item)
    if not items:
        return jsonify({"error": "No valid commands"}), 400
    ids = []
    now = _now()
    with update_data() as data:
        for item in items:
            cid = str(uuid.uuid4())[:8]
            if "at" not in item:
                item = dict(item)
                item["at"] = now
            data["commands"][cid] = item
            ids.append(cid)
            # Permanent record, independent of the pending queue above (which gets
            # deleted once the TZT prints it) so sent messages aren't lost once delivered.
            if item.get("type") == "print":
                data["messages"].append({
                    "id": cid,
                    "text": item.get("data", ""),
                    "source": item.get("source", ""),
                    "at": item["at"],
                })
        data["messages"] = data["messages"][-500:]
    return jsonify({"id": ids[0]} if len(ids) == 1 else {"ids": ids}), 201


@app.route("/api/commands/<cid>", methods=["DELETE"], strict_slashes=False)
@require_api_key
def delete_command(cid):
    with update_data() as data:
        data["commands"].pop(cid, None)
    return "", 204


@app.route("/api/messages/<mid>", methods=["DELETE"])
@require_api_key
def delete_message(mid):
    """Delete one entry from the permanent sent-messages log (web UI)."""
    with update_data() as data:
        data["messages"] = [m for m in data["messages"] if m.get("id") != mid]
    return "", 204


# ---------- API: Audio & images (ESP32) ----------
MEDIA = {
    # kind: (folder, allowed extensions, default name, multipart field, raw-upload name prefix)
    "audio": (lambda: AUDIO_DIR, AUDIO_EXTENSIONS, "audio.mp3", "audio", "voice"),
    "images": (lambda: IMAGES_DIR, IMAGE_EXTENSIONS, "image.jpg", "image", "photo"),
}


def _list_media(kind):
    """?pending=1: only uploads the TZT hasn't downloaded yet (all it needs for its backlog sync,
    and it has to hold the whole answer in RAM)."""
    base = _base_url()
    pending_only = request.args.get("pending") == "1"
    result = {}
    for k, v in get_data()[kind].items():
        if pending_only and v.get("downloaded"):
            continue
        r = dict(v)
        if r["url"].startswith("/"):
            r["url"] = base + r["url"]  # full URLs for the ESP32
        result[k] = r
    return jsonify(result)


def _unique_upload_path(folder, name):
    """A path for a new upload that no other file shares, even after conversion changes its
    extension (photo.png -> photo.jpg). iPhones often send every photo as "image.jpg": overwriting
    would swap the picture under older entries, and the TZT, which skips files it already has, would
    never fetch the new one. Reserves the name by creating the file, so parallel uploads can't pick it."""
    stem, ext = Path(name).stem, Path(name).suffix
    for n in range(1, 1000):
        candidate = f"{stem}{ext}" if n == 1 else f"{stem}_{n}{ext}"
        if any(folder.glob(glob_escape(Path(candidate).stem) + ".*")):
            continue
        try:
            os.close(os.open(folder / candidate, os.O_CREAT | os.O_EXCL | os.O_WRONLY))
            return folder / candidate
        except FileExistsError:
            continue
    return folder / f"{stem}_{uuid.uuid4().hex[:8]}{ext}"


def _save_upload(kind, name, write):
    """Save an uploaded file (write(path) stores it), convert it for the TZT, return (url, name).
    Runs outside the data lock: the conversion (ffmpeg, Pillow) can take seconds."""
    folder, exts, default, _, _ = MEDIA[kind]
    path = _unique_upload_path(folder(), safe_upload_name(name, exts, default))
    write(path)
    safe = prepare_for_tzt(kind, path).name
    return f"{_base_url()}/uploads/{kind}/{safe}", safe


def _post_media(kind):
    _, _, default, field, prefix = MEDIA[kind]
    if "application/json" in (request.content_type or ""):
        body = request.get_json(force=True, silent=True) or {}
        url = body.get("url", "")
        name = body.get("name", "") or (url.split("/")[-1].split("?")[0] if url else "") or default
        if not url or not name:
            return jsonify({"error": "url and name required"}), 400
        url = url if url.startswith("http") else _base_url() + (
            url if url.startswith("/") else f"/uploads/{kind}/" + name)
        raw_upload = False
    else:
        # Any file field: iOS Shortcuts' "Form" body names it whatever the key was set to
        # ("Photo", "photo", ...) and may leave out the filename
        f = (request.files.get("file") or request.files.get(field)
             or next(iter(request.files.values()), None))
        if f:
            url, name = _save_upload(kind, request.form.get("name") or f.filename or default, f.save)
            raw_upload = False
        else:
            # iOS Shortcuts "File" body: raw bytes with Content-Type audio/* or image/*
            raw = request.get_data()
            if not raw:
                app.logger.warning("Rejected %s upload: no file (Content-Type %r, form keys %s)",
                                   kind, request.content_type, list(request.form.keys()))
                return jsonify({"error": "file or JSON url/name required"}), 400
            ct = (request.content_type or "").lower()
            if kind == "audio":
                ext = "m4a" if "m4a" in ct or "aac" in ct else "mp3"
            else:
                ext = "png" if "png" in ct else "heic" if "heic" in ct or "heif" in ct else "jpg"
            name = f"{prefix}_{datetime.utcnow().strftime('%Y%m%d_%H%M%S')}.{ext}"
            url, name = _save_upload(kind, name, lambda path: path.write_bytes(raw))
            raw_upload = True
    with update_data() as data:
        cid = _add_media(data, kind, url, name)
    return jsonify({"id": "ok" if raw_upload else cid}), 201


def _mark_media_downloaded(kind, cid):
    with update_data() as data:
        if cid not in data[kind]:
            return "", 404
        body = request.get_json(force=True, silent=True) or {}
        if body.get("downloaded") is True:
            data[kind][cid]["downloaded"] = True
        return jsonify(data[kind][cid])


def _delete_media(kind, cid):
    with update_data() as data:
        if cid not in data[kind]:
            return "", 404
        entry = data[kind].pop(cid)
        data["commands"].pop(cid, None)  # not downloaded yet: drop its pending download command
    if entry.get("name"):
        _unlink_quietly(MEDIA[kind][0]() / entry["name"])
    return "", 204


@app.route("/api/audio", methods=["GET"])
@require_api_key
def get_audio():
    return _list_media("audio")


@app.route("/api/audio", methods=["POST"])
@require_api_key
def post_audio():
    return _post_media("audio")


@app.route("/api/audio/<cid>", methods=["PUT"])
@require_api_key
def put_audio(cid):
    return _mark_media_downloaded("audio", cid)


@app.route("/api/audio/<cid>", methods=["DELETE"])
@require_api_key
def delete_audio(cid):
    return _delete_media("audio", cid)


@app.route("/api/images", methods=["GET"])
@require_api_key
def get_images():
    return _list_media("images")


@app.route("/api/images", methods=["POST"])
@require_api_key
def post_image():
    return _post_media("images")


@app.route("/api/images/<cid>", methods=["PUT"])
@require_api_key
def put_image(cid):
    return _mark_media_downloaded("images", cid)


@app.route("/api/images/<cid>", methods=["DELETE"])
@require_api_key
def delete_image(cid):
    return _delete_media("images", cid)


# ---------- API: Devices (heartbeat for visibility) ----------
@app.route("/api/devices", methods=["GET"])
@require_api_key
def get_devices():
    return jsonify(load_devices())


@app.route("/api/devices/heartbeat", methods=["POST"])
@require_api_key
def device_heartbeat():
    """ESP32 can POST {"device_id": "tzt-display", "name": "TZT Display"} to show in web UI.
    (The TZT doesn't need to: it sends X-Device-Id on every /api/commands poll.)"""
    body = request.get_json(force=True, silent=True) or {}
    device_id = body.get("device_id") or request.headers.get("X-Device-Id", "").strip()
    name = body.get("name") or device_id or "Unknown"
    if not device_id:
        return jsonify({"error": "device_id required"}), 400
    touch_device(device_id, name)
    return jsonify({"ok": True}), 200


# ---------- API: Reminders & Config (ESP32) ----------
@app.route("/api/reminders", methods=["GET"])
@require_api_key
def get_reminders():
    return jsonify(get_data()["reminders"])


@app.route("/api/reminders", methods=["PUT"])
@require_api_key
def put_reminders():
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    with update_data() as data:
        data["reminders"] = body
    return jsonify({"ok": True})


@app.route("/api/config", methods=["GET"])
@require_api_key
def get_config():
    return jsonify(get_data()["config"])


@app.route("/api/config", methods=["PUT"])
@require_api_key
def put_config():
    body = request.get_json(force=True, silent=True)
    if body is None:
        return jsonify({"error": "JSON body required"}), 400
    with update_data() as data:
        data["config"] = body
    return jsonify({"ok": True})


# ---------- Legacy /message, /image, /audio (web UI, shortcuts) ----------
@app.route("/message", methods=["POST"])
@require_api_key
def receive_message():
    body = request.get_json(silent=True) or {}
    msg = body.get("message", request.form.get("message", ""))
    if not msg:
        return jsonify({"error": "No message provided"}), 400
    cmd = {"type": "print", "data": msg, "source": "shortcut", "at": _now()}
    err = _command_too_big(cmd)
    if err:
        return jsonify({"error": err}), 413
    cid = str(uuid.uuid4())[:8]
    with update_data() as data:
        data["messages"].append({"id": cid, "text": msg, "source": "shortcut", "at": cmd["at"]})
        data["messages"] = data["messages"][-500:]
        data["commands"][cid] = cmd  # what the TZT polls and prints
    return jsonify({"status": "received", "message": msg}), 201


def _receive_legacy_media(kind):
    _, _, default, field, _ = MEDIA[kind]
    f = request.files.get(field) or request.files.get("file")
    if not f or f.filename == "":
        return jsonify({"error": f"No {field} provided"}), 400
    url, name = _save_upload(kind, request.form.get("name") or f.filename or default, f.save)
    with update_data() as data:
        _add_media(data, kind, url, name)
    return jsonify({"status": "saved", "filename": name}), 201


@app.route("/image", methods=["POST"])
@require_api_key
def receive_image():
    return _receive_legacy_media("images")


@app.route("/audio", methods=["POST"])
@require_api_key
def receive_audio():
    return _receive_legacy_media("audio")


# ---------- File serving (ESP32 download) ----------
@app.route("/uploads/images/<path:name>")
def serve_image(name):
    return send_from_directory(IMAGES_DIR, name, as_attachment=request.args.get("dl") == "1")


@app.route("/uploads/audio/<path:name>")
def serve_audio(name):
    return send_from_directory(AUDIO_DIR, name, as_attachment=True)


# ---------- Firmware updates (the TZT installs what's published here) ----------
# The boards live away from the Mac, so updates go through the Pi: publish_firmware.py uploads a
# build, the Pi queues an "update_firmware" command, and the TZT downloads it over its usual
# connection and installs it. A device that reports (X-Firmware on its polls) that it's still on
# an older build is offered the update again every FIRMWARE_RETRY_S, so a failed install retries.
_firmware_offered_at = {}  # device -> monotonic time of the last automatic re-offer (per worker)


def _firmware_meta(device):
    try:
        return json.loads((FIRMWARE_DIR / f"{device}.json").read_text())
    except (OSError, ValueError):
        return None


def _queue_firmware_update(data, device, meta):
    """Replace any pending update for this device with one for meta's build."""
    data["commands"] = {k: v for k, v in data["commands"].items()
                        if not (v.get("type") == "update_firmware" and v.get("device") == device)}
    data["commands"]["fw-" + uuid.uuid4().hex[:8]] = {
        "type": "update_firmware", "device": device, "version": meta["version"],
        "size": meta["size"], "md5": meta["md5"], "path": f"/api/firmware/{device}/image", "at": _now(),
    }


def _offer_firmware_again(device, running):
    if not running or device not in FIRMWARE_DEVICES:
        return
    meta = _firmware_meta(device)
    if not meta or meta["version"] == running:
        return
    last = _firmware_offered_at.get(device)
    if last is not None and time.monotonic() - last < FIRMWARE_RETRY_S:
        return
    _firmware_offered_at[device] = time.monotonic()
    with update_data() as data:
        if not any(v.get("type") == "update_firmware" and v.get("device") == device
                   for v in data["commands"].values()):
            _queue_firmware_update(data, device, meta)
            app.logger.info("Offering firmware %s to %s again (it runs %s)", meta["version"], device, running)


@app.route("/api/firmware/<device>", methods=["POST"])
@require_api_key
def publish_firmware(device):
    """Raw .bin body, ?version=<build id>. Stores it and tells the device to update."""
    version = request.args.get("version", "").strip()
    if device not in FIRMWARE_DEVICES:
        return jsonify({"error": f"unknown device {device!r}"}), 404
    if not version or len(version) > 64:
        return jsonify({"error": "version required"}), 400
    blob = request.get_data()
    if not blob or blob[0] != 0xE9:  # every ESP32 app image starts with this magic byte
        return jsonify({"error": "not an ESP32 firmware image"}), 400
    if len(blob) > FIRMWARE_DEVICES[device]:
        return jsonify({"error": f"image is {len(blob)} bytes, the partition holds {FIRMWARE_DEVICES[device]}"}), 400
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    meta = {"version": version, "size": len(blob), "md5": hashlib.md5(blob).hexdigest(), "at": _now()}
    tmp = FIRMWARE_DIR / f"{device}.bin.tmp"
    tmp.write_bytes(blob)
    os.replace(tmp, FIRMWARE_DIR / f"{device}.bin")
    (FIRMWARE_DIR / f"{device}.json").write_text(json.dumps(meta))
    with update_data() as data:
        _queue_firmware_update(data, device, meta)
    _firmware_offered_at.pop(device, None)
    app.logger.info("Firmware %s published for %s (%d bytes)", version, device, len(blob))
    return jsonify(meta), 201


@app.route("/api/firmware/<device>", methods=["GET"])
@require_api_key
def firmware_info(device):
    """What's published. Main checks this itself (with X-Device-Id, which marks it seen)."""
    seen = request.headers.get("X-Device-Id", "").strip()
    if seen:
        touch_device(seen, request.headers.get("X-Device-Name", "").strip() or seen)
    meta = _firmware_meta(device) if device in FIRMWARE_DEVICES else None
    return (jsonify(meta), 200) if meta else (jsonify({"error": "nothing published"}), 404)


@app.route("/api/firmware/<device>/image", methods=["GET"])
@require_api_key
def firmware_image(device):
    if device not in FIRMWARE_DEVICES or not (FIRMWARE_DIR / f"{device}.bin").exists():
        return jsonify({"error": "nothing published"}), 404
    return send_from_directory(FIRMWARE_DIR, f"{device}.bin", mimetype="application/octet-stream")


# ---------- Health ----------
@app.route("/api/health")
def health():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    debug = os.environ.get("PRICK_N_PRINT_DEBUG", "0") == "1"
    app.run(host="0.0.0.0", port=5000, debug=debug)
