"""
Tests for raspberrypi/app.py: API key auth, upload path-traversal rejection,
and basic CRUD round-trips for commands/reminders/config.

Run: pip install -r requirements.txt -r tests/requirements.txt && pytest raspberrypi/tests
"""

import io
import os
import sys
import threading
import time

TEST_API_KEY = "test-key-do-not-use-in-prod"
os.environ.setdefault("PRICK_N_PRINT_API_KEY", TEST_API_KEY)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pytest  # noqa: E402

import app as app_module  # noqa: E402


@pytest.fixture
def client(tmp_path, monkeypatch):
    monkeypatch.setattr(app_module, "DATA_FILE", tmp_path / "data.json")
    monkeypatch.setattr(app_module, "LOCK_FILE", tmp_path / "data.json.lock")
    monkeypatch.setattr(app_module, "IMAGES_DIR", tmp_path / "images")
    monkeypatch.setattr(app_module, "AUDIO_DIR", tmp_path / "audio")
    monkeypatch.setattr(app_module, "DEVICES_FILE", tmp_path / "devices.json")
    (tmp_path / "images").mkdir()
    (tmp_path / "audio").mkdir()
    app_module.app.config["TESTING"] = True
    with app_module.app.test_client() as c:
        yield c


AUTH = {"X-API-Key": TEST_API_KEY}


# ---------- Auth ----------

def test_post_command_requires_api_key(client):
    r = client.post("/api/commands", json={"type": "print", "data": "hi"})
    assert r.status_code == 401


def test_post_command_rejects_wrong_api_key(client):
    r = client.post("/api/commands", json={"type": "print", "data": "hi"}, headers={"X-API-Key": "wrong"})
    assert r.status_code == 401


def test_post_command_accepts_correct_api_key(client):
    r = client.post("/api/commands", json={"type": "print", "data": "hi"}, headers=AUTH)
    assert r.status_code == 201


def test_get_commands_requires_api_key(client):
    r = client.get("/api/commands")
    assert r.status_code == 401
    r = client.get("/api/commands", headers=AUTH)
    assert r.status_code == 200


def test_get_audio_requires_api_key(client):
    assert client.get("/api/audio").status_code == 401
    assert client.get("/api/audio", headers=AUTH).status_code == 200


def test_get_images_requires_api_key(client):
    assert client.get("/api/images").status_code == 401
    assert client.get("/api/images", headers=AUTH).status_code == 200


def test_get_reminders_requires_api_key(client):
    assert client.get("/api/reminders").status_code == 401
    assert client.get("/api/reminders", headers=AUTH).status_code == 200


def test_get_config_requires_api_key(client):
    assert client.get("/api/config").status_code == 401
    assert client.get("/api/config", headers=AUTH).status_code == 200


def test_get_devices_requires_api_key(client):
    assert client.get("/api/devices").status_code == 401
    assert client.get("/api/devices", headers=AUTH).status_code == 200


def test_health_does_not_require_api_key(client):
    r = client.get("/api/health")
    assert r.status_code == 200


def test_put_config_requires_api_key(client):
    r = client.put("/api/config", json={"foo": "bar"})
    assert r.status_code == 401


def test_put_reminders_requires_api_key(client):
    r = client.put("/api/reminders", json=[])
    assert r.status_code == 401


def test_device_heartbeat_requires_api_key(client):
    r = client.post("/api/devices/heartbeat", json={"device_id": "tzt"})
    assert r.status_code == 401


# ---------- Path traversal ----------

def test_image_upload_rejects_path_traversal_filename(client):
    data = {
        "file": (io.BytesIO(b"fake image bytes"), "../../etc/passwd.jpg"),
    }
    r = client.post("/api/images", headers=AUTH, data=data, content_type="multipart/form-data")
    assert r.status_code == 201
    cid = r.get_json()["id"]
    r2 = client.get("/api/images", headers=AUTH)
    saved_name = r2.get_json()[cid]["name"]
    assert "/" not in saved_name
    assert ".." not in saved_name
    assert (app_module.IMAGES_DIR / saved_name).resolve().parent == app_module.IMAGES_DIR.resolve()


def test_audio_upload_rejects_disallowed_extension(client):
    data = {
        "file": (io.BytesIO(b"not really audio"), "payload.exe"),
    }
    r = client.post("/api/audio", headers=AUTH, data=data, content_type="multipart/form-data")
    assert r.status_code == 201
    body = r.get_json()
    r2 = client.get("/api/audio", headers=AUTH)
    saved_name = r2.get_json()[body["id"]]["name"]
    assert saved_name.endswith(".mp3")  # disallowed ext swapped for the safe default


def test_legacy_image_route_sanitizes_filename(client):
    data = {"image": (io.BytesIO(b"bytes"), "../../../etc/passwd")}
    r = client.post("/image", headers=AUTH, data=data, content_type="multipart/form-data")
    assert r.status_code == 201
    filename = r.get_json()["filename"]
    assert "/" not in filename and ".." not in filename


# ---------- CRUD round-trips ----------

def test_commands_crud_round_trip(client):
    r = client.post("/api/commands", json={"type": "print", "data": "hello"}, headers=AUTH)
    cid = r.get_json()["id"]

    r = client.get("/api/commands", headers=AUTH)
    assert cid in r.get_json()

    r = client.delete(f"/api/commands/{cid}", headers=AUTH)
    assert r.status_code == 204

    r = client.get("/api/commands", headers=AUTH)
    assert cid not in r.get_json()


def test_print_command_survives_in_history_after_command_deleted(client):
    """Sent messages must stay visible in /api/history even after the TZT
    deletes the pending command post-print (the whole point of the log)."""
    r = client.post("/api/commands", json={"type": "print", "data": "hello", "source": "shortcut"}, headers=AUTH)
    cid = r.get_json()["id"]

    r = client.delete(f"/api/commands/{cid}", headers=AUTH)
    assert r.status_code == 204

    r = client.get("/api/history")
    msgs = r.get_json()["messages"]
    assert any(m["id"] == cid and m["text"] == "hello" for m in msgs)


def test_non_print_command_not_logged_as_message(client):
    client.post("/api/commands", json={"type": "test_printer"}, headers=AUTH)
    r = client.get("/api/history")
    assert r.get_json()["messages"] == []


def test_delete_message_removes_from_history(client):
    r = client.post("/api/commands", json={"type": "print", "data": "delete me"}, headers=AUTH)
    cid = r.get_json()["id"]

    r = client.delete(f"/api/messages/{cid}", headers=AUTH)
    assert r.status_code == 204

    r = client.get("/api/history")
    msgs = r.get_json()["messages"]
    assert not any(m["id"] == cid for m in msgs)


def test_delete_message_requires_api_key(client):
    r = client.post("/api/commands", json={"type": "print", "data": "hi"}, headers=AUTH)
    cid = r.get_json()["id"]
    r = client.delete(f"/api/messages/{cid}")
    assert r.status_code == 401


def test_reminders_round_trip(client):
    r = client.put("/api/reminders", json=[{"text": "take a break"}], headers=AUTH)
    assert r.status_code == 200

    r = client.get("/api/reminders", headers=AUTH)
    assert r.get_json() == [{"text": "take a break"}]


def test_config_round_trip(client):
    r = client.put("/api/config", json={"autoDispense": True}, headers=AUTH)
    assert r.status_code == 200

    r = client.get("/api/config", headers=AUTH)
    assert r.get_json() == {"autoDispense": True}


def test_device_heartbeat_round_trip(client):
    r = client.post("/api/devices/heartbeat", json={"device_id": "tzt-display", "name": "TZT"}, headers=AUTH)
    assert r.status_code == 200

    r = client.get("/api/devices", headers=AUTH)
    body = r.get_json()
    assert "tzt-display" in body
    assert body["tzt-display"]["name"] == "TZT"


def test_commands_poll_marks_device_seen(client):
    r = client.get("/api/commands", headers={**AUTH, "X-Device-Id": "tzt-display", "X-Device-Name": "TZT Display"})
    assert r.status_code == 200

    body = client.get("/api/devices", headers=AUTH).get_json()
    assert body["tzt-display"]["name"] == "TZT Display"
    assert body["tzt-display"]["last_seen"]


def test_commands_long_poll_returns_at_once_when_queued(client, monkeypatch):
    monkeypatch.setattr(app_module, "COMMANDS_WAIT_CHECK_S", 0.01)
    client.post("/api/commands", json={"type": "print", "data": "hi"}, headers=AUTH)
    start = time.monotonic()
    r = client.get("/api/commands?wait=5", headers=AUTH)
    assert time.monotonic() - start < 1
    assert len(r.get_json()) == 1


def test_commands_long_poll_wakes_on_new_command(client, monkeypatch):
    monkeypatch.setattr(app_module, "COMMANDS_WAIT_CHECK_S", 0.01)
    def queue_later():
        time.sleep(0.3)
        with app_module.update_data() as data:
            data["commands"]["c1"] = {"type": "print", "data": "hi"}
    threading.Thread(target=queue_later).start()
    start = time.monotonic()
    r = client.get("/api/commands?wait=5", headers=AUTH)
    assert 0.2 < time.monotonic() - start < 3
    assert list(r.get_json()) == ["c1"]


def test_commands_poll_ack_deletes_handled_commands(client):
    a = client.post("/api/commands", json={"type": "print", "data": "a"}, headers=AUTH).get_json()["id"]
    b = client.post("/api/commands", json={"type": "print", "data": "b"}, headers=AUTH).get_json()["id"]
    r = client.get(f"/api/commands?ack={a},unknown", headers=AUTH)
    assert list(r.get_json()) == [b]
    # Acks are idempotent: a repeat (after a failed poll) is harmless
    r = client.get(f"/api/commands?ack={a},{b}", headers=AUTH)
    assert r.get_json() == {}


def test_commands_long_poll_times_out_empty(client, monkeypatch):
    monkeypatch.setattr(app_module, "COMMANDS_WAIT_CHECK_S", 0.01)
    start = time.monotonic()
    r = client.get("/api/commands?wait=0.3", headers=AUTH)
    assert 0.25 < time.monotonic() - start < 2
    assert r.get_json() == {}


def test_image_upload_queues_download_command(client):
    r = client.post(
        "/api/images",
        data={"file": (io.BytesIO(b"\xff\xd8jpeg"), "cat.jpg")},
        content_type="multipart/form-data",
        headers=AUTH,
    )
    assert r.status_code == 201
    mid = r.get_json()["id"]

    cmds = client.get("/api/commands", headers=AUTH).get_json()
    assert cmds[mid]["type"] == "download_image"
    assert cmds[mid]["media_id"] == mid
    assert "silent" not in cmds[mid]  # the TZT shows / plays new uploads on arrival
    assert cmds[mid]["url"].endswith("/uploads/images/cat.jpg")


def test_image_upload_accepts_any_form_field_name(client):
    # iOS Shortcuts "Form" body: the file sits under whatever key the user typed
    r = client.post(
        "/api/images",
        data={"Photo": (io.BytesIO(b"\xff\xd8jpeg"), "IMG_0042.HEIC")},
        content_type="multipart/form-data",
        headers=AUTH,
    )
    assert r.status_code == 201
    cmds = client.get("/api/commands", headers=AUTH).get_json()
    assert any(c["type"] == "download_image" for c in cmds.values())


def test_deleting_media_drops_pending_download_command(client):
    r = client.post(
        "/api/audio",
        data={"file": (io.BytesIO(b"ID3"), "hi.mp3")},
        content_type="multipart/form-data",
        headers=AUTH,
    )
    mid = r.get_json()["id"]
    assert client.delete(f"/api/audio/{mid}", headers=AUTH).status_code == 204
    assert mid not in client.get("/api/commands", headers=AUTH).get_json()


# ---------- One command per poll, delivery state, size limits ----------

def test_commands_poll_hands_out_oldest_command_only(client):
    a = client.post("/api/commands", json={"type": "print", "data": "a"}, headers=AUTH).get_json()["id"]
    b = client.post("/api/commands", json={"type": "print", "data": "b"}, headers=AUTH).get_json()["id"]
    r = client.get("/api/commands", headers=AUTH)
    assert list(r.get_json()) == [a]
    assert r.headers["X-Command-Id"] == a
    r = client.get(f"/api/commands?ack={a}", headers=AUTH)
    assert list(r.get_json()) == [b]
    assert r.headers["X-Command-Id"] == b


def test_empty_poll_has_no_command_id_header(client):
    r = client.get("/api/commands", headers=AUTH)
    assert r.get_json() == {}
    assert "X-Command-Id" not in r.headers


def test_ack_marks_message_delivered(client):
    cid = client.post("/api/commands", json={"type": "print", "data": "hi"}, headers=AUTH).get_json()["id"]
    msg = client.get("/api/history").get_json()["messages"][0]
    assert msg["pending"] is True and "delivered_at" not in msg
    client.get(f"/api/commands?ack={cid}", headers=AUTH)
    msg = client.get("/api/history").get_json()["messages"][0]
    assert msg["pending"] is False and msg["delivered_at"]


def test_skip_removes_command_and_marks_message_failed(client):
    cid = client.post("/api/commands", json={"type": "print", "data": "hi"}, headers=AUTH).get_json()["id"]
    # The TZT sends a skipped id in both ack= (older servers) and skip=
    r = client.get(f"/api/commands?ack={cid}&skip={cid}", headers=AUTH)
    assert r.get_json() == {}
    msg = client.get("/api/history").get_json()["messages"][0]
    assert msg["failed"] and "delivered_at" not in msg


def test_print_over_printer_limit_is_rejected(client):
    ok = "x" * app_module.PRINT_MAX_BYTES
    assert client.post("/api/commands", json={"type": "print", "data": ok}, headers=AUTH).status_code == 201
    r = client.post("/api/commands", json={"type": "print", "data": ok + "x"}, headers=AUTH)
    assert r.status_code == 413
    assert "too long" in r.get_json()["error"]


def test_print_limit_counts_utf8_bytes(client):
    # 3 bytes per character: the printer limit is bytes, not characters
    text = "\u0928" * (app_module.PRINT_MAX_BYTES // 3 + 1)
    r = client.post("/api/commands", json={"type": "print", "data": text}, headers=AUTH)
    assert r.status_code == 413


def test_oversized_batch_queues_nothing(client):
    big = {"type": "print", "data": "x" * (app_module.PRINT_MAX_BYTES + 1)}
    r = client.post("/api/commands", json=[{"type": "print", "data": "ok"}, big], headers=AUTH)
    assert r.status_code == 413
    assert client.get("/api/commands", headers=AUTH).get_json() == {}


def test_legacy_message_route_enforces_limit(client):
    r = client.post("/message", json={"message": "x" * (app_module.PRINT_MAX_BYTES + 1)}, headers=AUTH)
    assert r.status_code == 413


def test_commands_sent_as_utf8_not_escaped(client):
    client.post("/api/commands", json={"type": "print", "data": "caf\u00e9"}, headers=AUTH)
    r = client.get("/api/commands", headers=AUTH)
    assert "caf\u00e9".encode("utf-8") in r.data


def test_wrong_api_key_rejected_on_open_routes(client):
    # nginx's public listener skips its password for requests that carry a key, so every route
    # (even ones that need no key) must reject a wrong one
    for path in ("/", "/api/history", "/api/health", "/uploads/images/x.jpg"):
        assert client.get(path, headers={"X-API-Key": "wrong"}).status_code == 401, path
    assert client.get("/api/health", headers=AUTH).status_code == 200


def test_concurrent_writes_are_not_lost(client):
    """Two workers' read-modify-write must not undo each other (lost or resurrected commands)."""
    errors = []

    def post(i):
        try:
            with app_module.app.test_client() as c:
                r = c.post("/api/commands", json={"type": "print", "data": f"m{i}"}, headers=AUTH)
                assert r.status_code == 201
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    threads = [threading.Thread(target=post, args=(i,)) for i in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
    data = app_module.get_data()
    assert len(data["commands"]) == 20
    assert len(data["messages"]) == 20


def test_clearing_messages_keeps_other_pending_commands(client):
    client.post("/api/commands", json={"type": "print", "data": "hi"}, headers=AUTH)
    alarm = client.post("/api/commands", json={"type": "set_alarm", "time": "07:30"}, headers=AUTH).get_json()["id"]
    assert client.delete("/api/history/messages", headers=AUTH).status_code == 200
    assert client.get("/api/history").get_json()["messages"] == []
    assert list(client.get("/api/commands", headers=AUTH).get_json()) == [alarm]


def test_uploads_with_the_same_name_get_unique_files(client):
    names = []
    for body in (b"\xff\xd8first", b"\xff\xd8second"):
        r = client.post("/image", headers=AUTH, content_type="multipart/form-data",
                        data={"image": (io.BytesIO(body), "image.jpg")})
        assert r.status_code == 201
        names.append(r.get_json()["filename"])
    assert names[0] != names[1]
    assert (app_module.IMAGES_DIR / names[0]).read_bytes() == b"\xff\xd8first"
    assert (app_module.IMAGES_DIR / names[1]).read_bytes() == b"\xff\xd8second"


def test_media_list_pending_only(client):
    ids = []
    for name in ("a.jpg", "b.jpg"):
        r = client.post("/api/images", data={"file": (io.BytesIO(b"\xff\xd8x"), name)},
                        content_type="multipart/form-data", headers=AUTH)
        ids.append(r.get_json()["id"])
    client.put(f"/api/images/{ids[0]}", json={"downloaded": True}, headers=AUTH)
    assert set(client.get("/api/images", headers=AUTH).get_json()) == set(ids)
    assert list(client.get("/api/images?pending=1", headers=AUTH).get_json()) == [ids[1]]
