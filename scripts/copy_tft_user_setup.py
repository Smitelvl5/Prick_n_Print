# Copy project's User_Setup.h into TFT_eSPI library so the library compiles with our pins/driver.
# Must run at script load time (before any compile).
Import("env")
import os
import shutil

project_dir = env["PROJECT_DIR"]
pioenv = env.get("PIOENV", "")
src = os.path.join(project_dir, "include", "User_Setup.h")
libdeps = os.path.join(project_dir, ".pio", "libdeps", pioenv, "TFT_eSPI")
dst = os.path.join(libdeps, "User_Setup.h")
if os.path.isdir(libdeps) and os.path.isfile(src):
    shutil.copy2(src, dst)
    print("[copy_tft_user_setup] Copied include/User_Setup.h -> TFT_eSPI/User_Setup.h")
