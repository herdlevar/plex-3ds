#!/usr/bin/env python3
"""
Auto-Deploy Plex3DS to Nintendo 3DS wirelessly (NetLoader or FTPD).
Usage: python auto_deploy.py [3DS_IP]
"""

import sys
import socket
import time
import subprocess
import ftplib
import os
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("3DS_IP", "192.168.1.100")
APP_FILE = os.path.join(PROJECT_DIR, "Plex3DS.3dsx")
SMDH_FILE = os.path.join(PROJECT_DIR, "Plex3DS.smdh")
CONFIG_FILE = os.path.join(PROJECT_DIR, "config.json")

devkitpro = os.environ.get("DEVKITPRO", r"C:\devkitPro")
LINK_EXE = os.path.join(devkitpro, "tools", "bin", "3dslink.exe" if os.name == "nt" else "3dslink")
if not os.path.exists(LINK_EXE):
    LINK_EXE = "3dslink"

def check_port(port):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.3)
        res = s.connect_ex((TARGET_IP, port))
        s.close()
        return res == 0
    except:
        return False

def deploy_via_ftp():
    print(f"\n[+] FTPD detected on {TARGET_IP}:5000! Connecting via FTP...")
    ftp = ftplib.FTP()
    ftp.connect(TARGET_IP, 5000, timeout=10)
    ftp.login()
    print("[+] Logged into 3DS SD Card via FTP.")

    # Create directories
    for d in ["/3ds", "/3ds/Plex3DS", "/3ds/plex-3ds"]:
        try:
            ftp.mkd(d)
        except:
            pass

    # Upload files
    print("[*] Uploading Plex3DS.3dsx ...")
    with open(APP_FILE, "rb") as f:
        ftp.storbinary("STOR /3ds/Plex3DS/Plex3DS.3dsx", f)

    print("[*] Uploading Plex3DS.smdh ...")
    with open(SMDH_FILE, "rb") as f:
        ftp.storbinary("STOR /3ds/Plex3DS/Plex3DS.smdh", f)

    if os.path.exists(CONFIG_FILE):
        print("[*] Uploading config.json (pre-paired configuration) ...")
        with open(CONFIG_FILE, "rb") as f:
            ftp.storbinary("STOR /3ds/plex-3ds/config.json", f)

    ftp.quit()
    print("\n[SUCCESS] Plex3DS and config.json are installed on your 3DS!")
    print("[*] Press START or B on your 3DS to exit FTPD, then open Plex3DS from Homebrew Launcher!")

def deploy_via_netloader():
    print(f"\n[+] NetLoader detected on {TARGET_IP}:17491! Beaming Plex3DS.3dsx wirelessly...")
    cmd = [LINK_EXE, "-s", "-a", TARGET_IP, APP_FILE]
    subprocess.run(cmd)
    print("\n[SUCCESS] Sent to 3DS! It should be launching on your screens now.")

def main():
    print("=" * 65)
    print(f"  TARGET 3DS: {TARGET_IP}")
    print("=" * 65)
    print("[*] Waiting for deployment trigger on your 3DS...")
    print("    Option A: In Homebrew Launcher, press [Y] for NetLoader")
    print("    Option B: Open the [FTPD] app on your 3DS")
    print("\n[*] Listening... (Press Ctrl+C to cancel)")

    for _ in range(300): # 150 seconds timeout
        if check_port(17491):
            deploy_via_netloader()
            return
        if check_port(5000):
            deploy_via_ftp()
            return
        time.sleep(0.5)

    print("\n[!] Timed out waiting for NetLoader or FTPD.")

if __name__ == "__main__":
    main()
