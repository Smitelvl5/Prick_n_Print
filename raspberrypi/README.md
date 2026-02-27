# Raspberry Pi Server

Flask server with **web UI** + **ESP32-compatible API**. Everything sent to the ESP32 (commands, images, audio) is stored here; the ESP32 can request anything to listen or view at any time.

- **sample.jpg** and **sample.mp3** are seeded on first run (place in `samples/` or project root)

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
- **Flask app** – runs as a systemd service
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
4. Run the server:
   ```bash
   python app.py
   ```

The server listens on `0.0.0.0:5000`, so it's reachable from other devices on your network. Point the app to `http://<pi-ip>:5000`.
