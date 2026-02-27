#!/bin/bash
# Change HTTP Basic Auth password for Prick_n_Print (public access)
# Run: sudo ./set-http-password.sh

HTPASSWD_FILE="/etc/nginx/.htpasswd.prick-n-print"

if [[ $EUID -ne 0 ]]; then
   echo "Run with sudo: sudo ./set-http-password.sh"
   exit 1
fi

if [[ ! -f "$HTPASSWD_FILE" ]]; then
   echo "Auth file not found. Run setup.sh first."
   exit 1
fi

echo "Set new HTTP Basic Auth credentials"
read -p "Username [admin]: " USER
USER="${USER:-admin}"
while true; do
   read -s -p "Password: " PASS
   echo
   [ -n "$PASS" ] && break
   echo "Password cannot be empty."
done

htpasswd -bc "$HTPASSWD_FILE" "$USER" "$PASS"
chmod 640 "$HTPASSWD_FILE"
echo "Done. Reloading nginx..."
systemctl reload nginx
echo "Password updated."
