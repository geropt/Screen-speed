import subprocess
import sys
import os

MAIN_SCRIPT = "extract_tiles.py"

def install_requirements():
    if not os.path.exists("requirements.txt"):
        print("ERROR: requirements.txt not found. Aborting.")
        sys.exit(1)  # Stop the script with error code

    print("Verifying and installing packages from requirements.txt...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-r", "requirements.txt"])

def run_main_script():
    if not os.path.exists(MAIN_SCRIPT):
        print(f"ERROR: main script '{MAIN_SCRIPT}' not found in this folder.")
        sys.exit(1)

    print(f"\nLaunching {MAIN_SCRIPT}...\n")
    
    # Pass arguments from launcher to main script
    subprocess.check_call([sys.executable, MAIN_SCRIPT] + sys.argv[1:])

if __name__ == "__main__":
    install_requirements()
    run_main_script()
