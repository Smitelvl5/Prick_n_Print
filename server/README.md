# TZT HTTP Backend

Your app or shortcuts POST here; the TZT display polls via GET.

## Run the server

```bash
cd server
pip install -r requirements.txt
flask --app app run --host 0.0.0.0 --port 5000
```

Or: `python app.py`

- **Host**: Use `0.0.0.0` so the ESP32 (and your phone) can reach it on your LAN.
- **Port**: Default 5000. Set the same in `include/config_tzt.h` as `BACKEND_URL` (e.g. `http://YOUR_PC_IP:5000`).

Data is stored in `server/data.json` and uploaded files in `server/uploads/`.

## API (same shape as before)

| Method | Path | Description |
|--------|------|-------------|
| POST | /api/commands | Add command(s). Body: `{"type":"print","data":"Hello","source":"shortcut"}` or array of same. |
| GET | /api/commands | ESP32 polls; returns all pending commands. |
| DELETE | /api/commands/<id> | ESP32 deletes after processing. |
| POST | /api/audio | Add audio. JSON: `{"url":"...","name":"voice.mp3"}` or multipart file. |
| GET | /api/audio | ESP32 polls; returns list with `url`, `name`, `downloaded`. |
| PUT | /api/audio/<id> | Body `{"downloaded":true}` (ESP32 marks after download). |
| POST | /api/images | Add image. JSON: `{"url":"...","name":"photo.jpg"}` or multipart file. |
| GET | /api/images | ESP32 polls; same as audio. |
| PUT | /api/images/<id> | Mark downloaded. |
| GET/PUT | /api/reminders | Reminders list (optional). |
| GET | /api/health | Health check. |

## Sending from your side

- **Message (print)**: `POST /api/commands` with body `{"type":"print","data":"Your message","source":"shortcut"}`.
- **Image**: `POST /api/images` with JSON `{"url":"https://..."}` or multipart form `file` + optional `name`.
- **Voice memo**: `POST /api/audio` with JSON `{"url":"https://..."}` or multipart form `file` + optional `name`.

If you upload a file (multipart), the server saves it under `uploads/audio` or `uploads/images` and returns a URL the ESP32 can GET. So the ESP32 in the apartment will download from your server (e.g. `http://YOUR_PC_IP:5000/uploads/audio/voice.mp3`).

## ESP32 config

In `include/config_tzt.h`:

- `USE_HTTP_BACKEND 1` to use this server.
- `BACKEND_URL "http://YOUR_SERVER_IP:5000"` (no trailing slash).

The ESP32 will poll commands every 10–30 s and audio/images every 60 s.
