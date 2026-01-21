Import("env")

# Copy User_Setup.h to TFT_eSPI library folder
import os
import shutil

def copy_user_setup():
    # Get the library path
    lib_path = os.path.join(env.PioLibDir(), "TFT_eSPI")
    user_setup_src = os.path.join(env.get("PROJECT_DIR"), "include", "User_Setup.h")
    user_setup_dst = os.path.join(lib_path, "User_Setup.h")
    
    if os.path.exists(user_setup_src):
        if not os.path.exists(lib_path):
            os.makedirs(lib_path)
        shutil.copy2(user_setup_src, user_setup_dst)
        print(f"Copied User_Setup.h to {user_setup_dst}")

env.AddPreAction("$BUILD_DIR/src/HardwareAbstraction.cpp.o", copy_user_setup)
