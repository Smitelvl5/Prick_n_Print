# Running the TZT backend on Raspberry Pi 4

Use your Pi as the always-on server so the ESP32 and your shortcuts can reach it 24/7.

## 1. Copy the server to the Pi

From your PC (PowerShell or scp):

```bash
scp -r "C:\Users\Smite\Desktop\My Stuff\Dhara\Prick_n_Print\server" pi@YOUR_PI_IP:~/tzt-backend
```

Or copy the `server` folder onto a USB stick / over SMB and put it in `~/tzt-backend` on the Pi.

## 2. On the Raspberry Pi

SSH in (or use the desktop terminal):

```bash
ssh pi@YOUR_PI_IP
cd ~/tzt-backend
```

Install Python 3 and venv if needed (Pi OS usually has them):

```bash
sudo apt update
sudo apt install -y python3 python3-pip python3-venv
```

Create a venv and install dependencies:

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Run the server once to test (listens on all interfaces so ESP32 can reach it):

```bash
python -m flask --app app run --host 0.0.0.0 --port 5000
```

Press Ctrl+C to stop. To keep it running in the background:

```bash
nohup python -m flask --app app run --host 0.0.0.0 --port 5000 > flask.log 2>&1 &
```

## 3. Run on boot (systemd service)

So the server starts automatically after a reboot:

```bash
sudo nano /etc/systemd/system/tzt-backend.service
```

Paste this (adjust `User` and path if your folder is elsewhere):

```ini
[Unit]
Description=TZT HTTP Backend
After=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/tzt-backend
ExecStart=/home/pi/tzt-backend/venv/bin/python -m flask --app app run --host 0.0.0.0 --port 5000
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable tzt-backend
sudo systemctl start tzt-backend
sudo systemctl status tzt-backend
```

Logs: `journalctl -u tzt-backend -f`

## 4. Set the Pi’s IP and configure the ESP32

- Find the Pi’s IP: on the Pi run `hostname -I` (first address is usually the one you want), or check your router’s DHCP list.
- In your project, **`include/config_tzt.h`** set:
  - `BACKEND_URL "http://192.168.x.x:5000"`  
  Use the Pi’s actual IP (e.g. `192.168.1.50`).
- Rebuild and flash the TZT firmware.

## 5. Send from your PC or shortcuts

Use the same URL:

```bash
python scripts/post_to_backend.py --url http://PI_IP:5000 --message "Hello from Pi"
python scripts/post_to_backend.py --url http://PI_IP:5000 --image path/to/photo.jpg
python scripts/post_to_backend.py --url http://PI_IP:5000 --audio path/to/voice.mp3
```

For iOS Shortcuts, use “Get contents of URL” with method POST and the same `http://PI_IP:5000/api/commands` or `/api/images` or `/api/audio` URLs (and the JSON/file body as in the server README).

## Optional: reserve the Pi’s IP

In your router’s DHCP settings, assign a fixed IP (DHCP reservation) to the Pi’s MAC address so `BACKEND_URL` never changes.
