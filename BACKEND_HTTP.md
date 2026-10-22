# Using the HTTP backend

The TZT display talks to your Raspberry Pi server for commands, audio, and images. You POST to the same server from your app or shortcuts; the ESP32 polls the server.

## 1. Run the server

See **[raspberrypi/README.md](raspberrypi/README.md)** for full setup (recommended: `sudo ./setup.sh`, which also generates the API key). For a quick manual run:

```bash
cd raspberrypi
pip install -r requirements.txt
export PRICK_N_PRINT_API_KEY="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
python app.py
```

Use the IP of the machine running the server (e.g. `192.168.1.100`). The ESP32 and your phone must be able to reach this URL (same Wi‑Fi or forwarded port).

## 2. Configure the ESP32

In **`include/secrets.h`** set **`BACKEND_API_KEY`** to the same value as `PRICK_N_PRINT_API_KEY` above.

In **`include/config_tzt.h`** set **`BACKEND_URL`** to your server, e.g. `"http://192.168.1.100:5000"` (no trailing slash).

Rebuild and flash the TZT firmware. It will poll `/api/commands`, `/api/audio`, and `/api/images`, authenticating with the `X-API-Key` header.

## 3. Send messages, images, and audio

From your computer or a shortcut:

```bash
# Message (print + show on display)
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --api-key YOUR_KEY --message "Hello"

# Image
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --api-key YOUR_KEY --image path/to/photo.jpg

# Voice memo
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --api-key YOUR_KEY --audio path/to/voice.mp3
```

Or use any HTTP client to POST to the server with an `X-API-Key` header (see **[raspberrypi/README.md](raspberrypi/README.md)** for the full API, and **[raspberrypi/IOS_SHORTCUTS.md](raspberrypi/IOS_SHORTCUTS.md)** for iOS Shortcuts setup).
