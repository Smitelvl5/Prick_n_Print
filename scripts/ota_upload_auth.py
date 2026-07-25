# For espota environments: read OTA_PASSWORD out of include/secrets.h and pass it as
# --auth to espota.py. Keeps the password out of platformio.ini (which is tracked in git).
Import("env")
import os
import re

if env.GetProjectOption("upload_protocol", "esptool") == "espota":
    project_dir = env["PROJECT_DIR"]
    secrets_path = os.path.join(project_dir, "include", "secrets.h")
    password = None
    if os.path.isfile(secrets_path):
        with open(secrets_path, "r") as f:
            match = re.search(r'#define\s+OTA_PASSWORD\s+"([^"]*)"', f.read())
            password = match.group(1) if match else None
    if password:
        # Must append to UPLOAD_FLAGS (mirrors the ini's `upload_flags` option), not UPLOADERFLAGS:
        # the espressif32 platform rebuilds UPLOADERFLAGS from UPLOAD_FLAGS at
        # "Configuring upload protocol..." time, which runs after this pre-script and would
        # otherwise wipe out an append made directly to UPLOADERFLAGS.
        env.Append(UPLOAD_FLAGS=["--auth=" + password])
        print("[ota_upload_auth] Using OTA_PASSWORD from include/secrets.h")
    else:
        print("[ota_upload_auth] WARNING: OTA_PASSWORD not found in include/secrets.h - OTA auth will fail")
