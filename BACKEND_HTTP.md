# Using the HTTP backend

The TZT display talks to your web server for commands, audio, and images. You POST to the same server from your app or shortcuts; the ESP32 polls the server.

## 1. Run the server

From the project root:

```bash
cd server
pip install -r requirements.txt
flask --app app run --host 0.0.0.0 --port 5000
```

Use the IP of the machine running the server (e.g. `192.168.1.100`). The ESP32 and your phone must be able to reach this URL (same Wi‑Fi or forwarded port).

## 2. Configure the ESP32

In **`include/config_tzt.h`** set **`BACKEND_URL`** to your server, e.g. `"http://192.168.1.100:5000"` (no trailing slash).

Rebuild and flash the TZT firmware. It will poll `/api/commands`, `/api/audio`, and `/api/images`.

## 3. Send messages, images, and audio

From your computer or a shortcut:

```bash
# Message (print + show on display)
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --message "Hello"

# Image
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --image path/to/photo.jpg

# Voice memo
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --audio path/to/voice.mp3
```

Or use any HTTP client to POST to the server (see **`server/README.md`** for the API).
