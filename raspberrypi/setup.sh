#!/bin/bash
#
# Prick_n_Print Raspberry Pi setup
# Installs: Tailscale, nginx (reverse proxy), ufw (firewall), Python Flask server
# Run: sudo ./setup.sh
# Ref: https://tailscale.com/docs/quick-guides
#

set -e

if [[ $EUID -ne 0 ]]; then
   echo "Run with sudo: sudo ./setup.sh"
   exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_USER="${SUDO_USER:-$(logname 2>/dev/null || whoami)}"

echo "=== Prick_n_Print Raspberry Pi Setup ==="
echo "Script dir: $SCRIPT_DIR"
echo "App user:   $APP_USER"
echo ""

# --- Clean install: remove old runtime files ---
echo ">>> Cleaning for fresh install..."
systemctl stop prick-n-print 2>/dev/null || true
cd "$SCRIPT_DIR"
rm -rf venv
rm -rf __pycache__ .pytest_cache "templates/__pycache__"
rm -f data.json
rm -rf uploads
find . -type d -name "__pycache__" -exec rm -rf {} + 2>/dev/null || true
find . -type f -name "*.pyc" -delete 2>/dev/null || true
echo "   Cleaned venv, cache, data.json, uploads"

# --- Update system ---
echo ">>> Updating apt..."
apt-get update -qq
apt-get install -y -qq curl python3 python3-pip python3-venv nginx ufw apache2-utils

# --- Install Tailscale ---
echo ">>> Installing Tailscale..."
curl -fsSL https://tailscale.com/install.sh | sh
systemctl enable --now tailscaled

# --- Python Flask app ---
echo ">>> Setting up Flask app..."
cd "$SCRIPT_DIR"
chown -R "$APP_USER:$APP_USER" .
sudo -u "$APP_USER" python3 -m venv venv
sudo -u "$APP_USER" ./venv/bin/pip install -q -r requirements.txt

# --- Systemd service ---
echo ">>> Creating systemd service..."
cat > /etc/systemd/system/prick-n-print.service << EOF
[Unit]
Description=Prick_n_Print Flask server
After=network.target

[Service]
Type=simple
User=$APP_USER
WorkingDirectory=$SCRIPT_DIR
ExecStart=$SCRIPT_DIR/venv/bin/python $SCRIPT_DIR/app.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable prick-n-print
systemctl start prick-n-print

# --- HTTP Basic Auth (for public/port-forwarded access) ---
# Local & Tailscale IPs skip auth; public internet requires password
HTPASSWD_FILE="/etc/nginx/.htpasswd.prick-n-print"
if [ -t 0 ]; then
  echo ">>> Set HTTP Basic Auth (for public access via port forwarding)"
  read -p "   Username [admin]: " AUTH_USER
  AUTH_USER="${AUTH_USER:-admin}"
  while true; do
    read -s -p "   Password: " AUTH_PASS
    echo
    [ -n "$AUTH_PASS" ] && break
    echo "   Password cannot be empty."
  done
  htpasswd -bc "$HTPASSWD_FILE" "$AUTH_USER" "$AUTH_PASS"
  chmod 640 "$HTPASSWD_FILE"
  echo "   Auth configured. Local/Tailscale: no password. Public: username=$AUTH_USER"
else
  echo ">>> Creating default HTTP auth (change with: sudo ./set-http-password.sh)"
  htpasswd -bc "$HTPASSWD_FILE" admin changeme
  chmod 640 "$HTPASSWD_FILE"
  echo "   WARNING: Using admin/changeme. Run set-http-password.sh to change."
fi

# --- Nginx reverse proxy ---
echo ">>> Configuring nginx..."
cat > /etc/nginx/sites-available/prick-n-print << NGINX
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;

    location / {
        satisfy any;
        allow 10.0.0.0/8;
        allow 172.16.0.0/12;
        allow 192.168.0.0/16;
        allow 100.64.0.0/10;
        allow 127.0.0.0/8;
        deny all;
        auth_basic "Prick_n_Print";
        auth_basic_user_file $HTPASSWD_FILE;

        proxy_pass http://127.0.0.1:5000;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        client_max_body_size 50M;
    }
}
NGINX

rm -f /etc/nginx/sites-enabled/default
ln -sf /etc/nginx/sites-available/prick-n-print /etc/nginx/sites-enabled/
nginx -t && systemctl reload nginx

# --- Firewall (ufw) ---
echo ">>> Configuring firewall..."
ufw default deny incoming 2>/dev/null || true
ufw default allow outgoing 2>/dev/null || true
ufw allow 22/tcp comment 'SSH'
ufw allow 80/tcp comment 'HTTP (public + local, for ESP32)'
ufw allow in on tailscale0 comment 'Tailscale'
ufw --force enable

echo ""
echo "=== Setup complete ==="
echo ""
echo "Tailscale: Authenticate to join your tailnet (opens browser):"
echo "  sudo tailscale up"
echo ""
echo "After auth, get your Tailscale IP:"
echo "  tailscale ip -4"
echo ""
echo "Access the server:"
echo "  - Local:     http://$(hostname -I | awk '{print $1}')"
echo "  - Public:    http://YOUR_PUBLIC_IP  (port forward 80, for ESP32)"
echo "  - Tailscale: http://\$(tailscale ip -4)"
echo ""
echo "HTTP Basic Auth: required for public access only"
echo "  sudo ./set-http-password.sh   # change password"
echo ""
echo "Service: prick-n-print (Flask on port 5000)"
echo "  sudo systemctl status prick-n-print"
echo "  sudo systemctl restart prick-n-print"
echo ""
