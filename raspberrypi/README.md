# Raspberry Pi Server

Flask server with **web UI** + **ESP32-compatible API**. Everything sent to the ESP32 (commands, images, audio) is stored here; the ESP32 can request anything to listen or view at any time.


### Where data is stored on the Pi

| Data       | Location                      |
|------------|-------------------------------|
| **Text**   | `data.json` (messages array)  |
| **Images** | `uploads/images/`             |
| **Audio**  | `uploads/audio/`              |

All paths are relative to the app directory (e.g. `/home/dhara/raspberrypi/`).

## Quick setup (recommended)

Copy this folder to your Pi, then run:

```bash
chmod +x setup.sh
sudo ./setup.sh
```

This installs and configures:
- **Tailscale** – secure access from your tailnet ([docs](https://tailscale.com/docs/quick-guides))
- **nginx** – reverse proxy (port 80 → Flask on 5000)
- **ufw** – firewall (SSH, HTTP, Tailscale only)
- **Flask app** – runs under gunicorn as a systemd service
- **API key** – generated automatically into `raspberrypi/.env` (`PRICK_N_PRINT_API_KEY`); required as an `X-API-Key` header on every command/upload/config request (from the ESP32, the web UI, and any script). Copy it into `include/secrets.h` as `BACKEND_API_KEY` on every ESP32 you flash.
- **HTTP Basic Auth** – required only for public access (port forwarding); local & Tailscale IPs skip auth

During setup you'll be prompted for a username and password. When accessing via your **public IP** (e.g. from cellular), the browser will ask for these credentials. Local (192.168.x) and Tailscale (100.x.x) access works without a password. To change credentials later:

```bash
sudo ./set-http-password.sh
```

After setup, authenticate Tailscale in your browser:
```bash
sudo tailscale up
```

Then get your Tailscale IP: `tailscale ip -4`. Access the web UI at `http://<pi-ip>` or `http://<tailscale-ip>`.

### iOS Shortcuts

See **[IOS_SHORTCUTS.md](IOS_SHORTCUTS.md)** for step-by-step instructions to send messages, images, and voice memos from your iPhone or iPad.

### Web UI features

- **Send** – Message, image, and audio upload
- **History** – Messages (pending commands), images, and audio with **individual delete** per item
- **Devices** – TZT Display appears when it polls for commands (each poll counts as a heartbeat; last-seen times are kept in RAM and reset when the Pi reboots). Main ESP32 connects via TZT over ESP-NOW and does not appear here.

### TZT Display connectivity

The TZT polls the Pi for commands. Set `BACKEND_URL` in `include/config_tzt.h`:

- **Same LAN**: Use the Pi's local IP, e.g. `http://192.168.1.100:5000` (Flask) or `http://192.168.1.100` (nginx on 80)
- **Public IP**: Use `http://your-public-ip` only if nginx is proxying port 80 → 5000; otherwise add `:5000`

## Manual setup

1. Copy this folder to your Raspberry Pi.
2. Create a virtual environment (optional but recommended):
   ```bash
   python3 -m venv venv
   source venv/bin/activate
   ```
3. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```
4. Set the API key (must match `BACKEND_API_KEY` in `include/secrets.h` on the ESP32s) and run the server:
   ```bash
   export PRICK_N_PRINT_API_KEY="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
   python app.py
   ```

The server listens on `0.0.0.0:5000`, so it's reachable from other devices on your network. Point the app to `http://<pi-ip>:5000`. Every request that changes state (commands, uploads, config, reminders) must include an `X-API-Key: <key>` header — `GET` polling routes and `/api/health` are open.
