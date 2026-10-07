#!/usr/bin/env python3
import sys
import socket
import subprocess
import re
from concurrent.futures import ThreadPoolExecutor

NINTENDO_OUIs = [
    "00:09:bf", "00:16:56", "00:17:ab", "00:19:1d", "00:19:fd", "00:1a:e9",
    "00:1b:7a", "00:1b:ea", "00:1c:be", "00:1d:bc", "00:1e:35", "00:1e:a9",
    "00:1f:32", "00:1f:c5", "00:21:47", "00:21:bd", "00:22:4c", "00:22:aa",
    "00:22:d7", "00:23:31", "00:23:cc", "00:24:1e", "00:24:44", "00:24:f3",
    "00:25:a0", "00:26:59", "00:27:09", "04:03:d6", "08:a2:24", "08:f0:43",
    "08:f1:da", "0c:fe:45", "18:2a:7b", "2c:10:c1", "34:af:2c", "40:d2:8a",
    "40:f4:07", "58:2f:40", "58:bd:a3", "60:6b:ff", "78:a2:a0", "7c:bb:8a",
    "8c:56:c5", "94:01:c2", "94:58:cb", "98:b6:e9", "a4:38:cc", "a4:5c:27",
    "a4:c0:e1", "b8:78:26", "b8:ae:6e", "cc:9e:00", "d4:f0:57", "d8:6b:f7",
    "dc:68:eb", "e0:0c:7f", "e0:e7:51", "e8:4e:ce"
]

def ping(ip):
    # Sends quick UDP or TCP probe to trigger ARP
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.05)
        s.sendto(b'', (ip, 17491))
        s.close()
    except:
        pass

def check_port(ip, port):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.2)
        res = s.connect_ex((ip, port))
        s.close()
        return res == 0
    except:
        return False

def get_arp_table():
    output = subprocess.check_output("arp -a", shell=True).decode('latin1', errors='ignore')
    arp = {}
    for line in output.splitlines():
        match = re.search(r'(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F-]+)\s+dynamic', line)
        if match:
            ip, mac = match.groups()
            arp[ip] = mac.replace('-', ':').lower()
    return arp

def get_local_subnet():
    if len(sys.argv) > 1:
        prefix = sys.argv[1].rstrip('.')
        return f"{prefix}."
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
        parts = local_ip.split('.')
        return f"{parts[0]}.{parts[1]}.{parts[2]}."
    except Exception:
        return "192.168.1."

def main():
    subnet = get_local_subnet()
    print(f"[*] Probing {subnet}0/24 subnet for 3DS...")
    ips = [f"{subnet}{i}" for i in range(1, 255)]
    
    with ThreadPoolExecutor(max_workers=64) as ex:
        ex.map(ping, ips)
        
    arp = get_arp_table()
    print(f"[*] Found {len(arp)} active devices in ARP cache.")
    
    candidates = []
    
    for ip, mac in arp.items():
            
        prefix = mac[:8].lower()
        is_nintendo = prefix in NINTENDO_OUIs
        has_ftpd = check_port(ip, 5000)
        has_netloader = check_port(ip, 17491)
        
        if is_nintendo or has_ftpd or has_netloader:
            candidates.append({
                "ip": ip,
                "mac": mac,
                "nintendo": is_nintendo,
                "ftpd": has_ftpd,
                "netloader": has_netloader
            })
            
    if candidates:
        print("\n[+] MATCH FOUND!")
        for c in candidates:
            print(f"    IP: {c['ip']} | MAC: {c['mac']} | Nintendo MAC: {c['nintendo']} | FTPD (5000): {c['ftpd']} | NetLoader (17491): {c['netloader']}")
    else:
        print("\n[!] No device with Nintendo MAC or open FTPD/NetLoader ports was immediately identified.")
        print("[*] Active IP list:")
        for ip, mac in sorted(arp.items(), key=lambda x: [int(p) for p in x[0].split('.')]):
            print(f"    {ip:16} {mac}")

if __name__ == "__main__":
    main()
