import ftplib
import os
import sys
import time

TARGET_IP = "192.168.86.33"
FTP_PORT = 5000

def deploy():
    print(f"Connecting to FTPD on {TARGET_IP}:{FTP_PORT}...")
    ftp = ftplib.FTP()
    try:
        ftp.connect(TARGET_IP, FTP_PORT, timeout=5)
        ftp.login()
    except Exception as e:
        print(f"Error connecting: {e}")
        print("Please make sure FTPD is open on your 3DS.")
        return False

    print("Connected! Ensuring /3ds/Plex3DS directory exists...")
    try:
        ftp.cwd("/3ds")
    except Exception:
        pass

    try:
        ftp.mkd("Plex3DS")
    except Exception:
        pass

    try:
        ftp.cwd("/3ds/Plex3DS")
    except Exception:
        pass

    for filename in ["Plex3DS.3dsx", "Plex3DS.smdh"]:
        if not os.path.exists(filename):
            continue
        size = os.path.getsize(filename)
        print(f"Uploading {filename} ({size / (1024*1024):.2f} MB)...", end="", flush=True)
        t0 = time.time()
        with open(filename, "rb") as f:
            ftp.storbinary(f"STOR {filename}", f)
        elapsed = time.time() - t0
        speed = (size / 1024) / max(0.01, elapsed)
        print(f" Done in {elapsed:.2f}s ({speed:.1f} KB/s)")

    if os.path.exists("Plex3DS.cia"):
        try:
            ftp.cwd("/")
        except Exception:
            pass
        try:
            ftp.mkd("cias")
        except Exception:
            pass
        try:
            ftp.cwd("/cias")
        except Exception:
            pass

        size = os.path.getsize("Plex3DS.cia")
        print(f"Uploading Plex3DS.cia to /cias/ ({size / (1024*1024):.2f} MB)...", end="", flush=True)
        t0 = time.time()
        with open("Plex3DS.cia", "rb") as f:
            ftp.storbinary("STOR Plex3DS.cia", f)
        elapsed = time.time() - t0
        speed = (size / 1024) / max(0.01, elapsed)
        print(f" Done in {elapsed:.2f}s ({speed:.1f} KB/s)")

    ftp.quit()
    print("\n[SUCCESS] Deployed!")
    print(" - 3DSX: Available in Homebrew Launcher (/3ds/Plex3DS/)")
    if os.path.exists("Plex3DS.cia"):
        print(" - CIA: Uploaded to /cias/Plex3DS.cia (install via FBI to add to HOME Menu)")
    return True

if __name__ == "__main__":
    deploy()
