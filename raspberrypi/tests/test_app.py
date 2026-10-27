"""
Tests for raspberrypi/app.py: API key auth, upload path-traversal rejection,
and basic CRUD round-trips for commands/reminders/config.

Run: pip install -r requirements.txt -r tests/requirements.txt && pytest raspberrypi/tests
"""

import io
import os
import sys

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


def test_image_upload_queues_silent_download_command(client):
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
    assert cmds[mid]["silent"] is True
    assert cmds[mid]["url"].endswith("/uploads/images/cat.jpg")


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
