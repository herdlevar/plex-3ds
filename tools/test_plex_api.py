#!/usr/bin/env python3
"""
Plex3DS - API & Transcoding Verification Tool
Tests Plex PIN authentication, server discovery, library fetching,
and generates transcode URLs specifically tuned for the New Nintendo 3DS.
"""

import sys
import os
import time
import uuid
import json
import urllib.request
import urllib.error
import xml.etree.ElementTree as ET

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT_ID = f"plex-3ds-{uuid.uuid4().hex[:12]}"
APP_NAME = "Plex3DS"
APP_VERSION = "0.1.0"
DEVICE_NAME = "New Nintendo 3DS"

DEFAULT_HEADERS = {
    "X-Plex-Product": APP_NAME,
    "X-Plex-Version": APP_VERSION,
    "X-Plex-Client-Identifier": CLIENT_ID,
    "X-Plex-Device": "Nintendo 3DS",
    "X-Plex-Device-Name": DEVICE_NAME,
    "X-Plex-Platform": "CTR",
    "X-Plex-Platform-Version": "11.17",
    "Accept": "application/json"
}

def request_json(url, method="GET", headers=None, data=None):
    all_headers = dict(DEFAULT_HEADERS)
    if headers:
        all_headers.update(headers)
    req = urllib.request.Request(url, headers=all_headers, method=method)
    if data:
        req.data = json.dumps(data).encode("utf-8")
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        print(f"[!] HTTP Error {e.code}: {e.read().decode('utf-8', errors='ignore')}")
        return None
    except Exception as e:
        print(f"[!] Request error: {e}")
        return None

def request_xml(url, headers=None):
    all_headers = dict(DEFAULT_HEADERS)
    all_headers["Accept"] = "application/xml"
    if headers:
        all_headers.update(headers)
    req = urllib.request.Request(url, headers=all_headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return ET.fromstring(resp.read())
    except Exception as e:
        print(f"[!] XML Request error: {e}")
        return None

def authenticate_pin():
    print("=" * 60)
    print(" 1. PLEX PIN AUTHENTICATION (Just like console apps)")
    print("=" * 60)
    
    pin_data = request_json(
        "https://plex.tv/api/v2/pins",
        method="POST"
    )
    if not pin_data:
        print("[!] Failed to request PIN from plex.tv")
        return None
        
    pin_id = pin_data.get("id")
    code = pin_data.get("code")
    
    print(f"\n[*] Your 4-digit Link Code is:  >>> {code} <<<")
    print(f"[*] Go to:  https://plex.tv/link  in your browser to authorize.\n")
    print("[*] Waiting for authorization (checking every 2s, press Ctrl+C to cancel)...")
    
    while True:
        time.sleep(2)
        status = request_json(f"https://plex.tv/api/v2/pins/{pin_id}")
        if not status:
            continue
            
        token = status.get("authToken")
        if token:
            print(f"\n[+] Success! Authenticated with Plex.")
            return token
            
        expires_at = status.get("expiresAt")
        if status.get("expiresIn", 0) <= 0:
            print("[!] PIN expired. Please run again.")
            return None

def discover_servers(auth_token):
    print("\n" + "=" * 60)
    print(" 2. SERVER DISCOVERY")
    print("=" * 60)
    
    headers = {"X-Plex-Token": auth_token}
    resources = request_json(
        "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1",
        headers=headers
    )
    
    if not resources:
        print("[!] No resources found or error communicating with plex.tv")
        return []
        
    servers = []
    for item in resources:
        if "server" in item.get("provides", ""):
            server_info = {
                "name": item.get("name"),
                "clientIdentifier": item.get("clientIdentifier"),
                "accessToken": item.get("accessToken", auth_token),
                "connections": item.get("connections", [])
            }
            servers.append(server_info)
            print(f"\n[+] Found Server: '{server_info['name']}'")
            for c in server_info["connections"]:
                local_tag = "[LOCAL]" if c.get("local") else "[REMOTE]"
                print(f"    - {local_tag} {c.get('uri')} (relay={c.get('relay')})")
                
    return servers

def inspect_library(server):
    print("\n" + "=" * 60)
    print(f" 3. INSPECTING LIBRARIES ON '{server['name']}'")
    print("=" * 60)
    
    # Try local connections first
    connections = sorted(server["connections"], key=lambda c: (not c.get("local"), c.get("relay")))
    base_url = None
    token = server["accessToken"]
    
    for c in connections:
        test_url = f"{c['uri']}/library/sections"
        print(f"[*] Trying connection: {c['uri']} ...")
        res = request_json(test_url, headers={"X-Plex-Token": token})
        if res:
            base_url = c["uri"]
            print(f"[+] Connected to {base_url}!")
            break
            
    if not base_url:
        print("[!] Could not connect to any server endpoint.")
        return
        
    sections = request_json(f"{base_url}/library/sections", headers={"X-Plex-Token": token})
    directories = sections.get("MediaContainer", {}).get("Directory", [])
    
    print(f"\n[+] Found {len(directories)} library section(s):")
    for sec in directories:
        sec_id = sec.get("key")
        title = sec.get("title")
        sec_type = sec.get("type")
        print(f"    [{sec_id}] {title} (Type: {sec_type})")
        
        # Peek at 3 items
        items_data = request_json(f"{base_url}/library/sections/{sec_id}/all?X-Plex-Container-Start=0&X-Plex-Container-Size=3", headers={"X-Plex-Token": token})
        metadata = items_data.get("MediaContainer", {}).get("Metadata", [])
        for m in metadata:
            item_title = m.get("title")
            rating_key = m.get("ratingKey")
            thumb = m.get("thumb")
            
            # Format poster thumbnail scaled for 3DS bottom screen (128x192)
            transcoded_poster = f"{base_url}/photo/:/transcode?width=128&height=192&minSize=1&url={thumb}&X-Plex-Token={token}"
            print(f"        -> '{item_title}' (ID: {rating_key})")
            
            # If video, show the exact New 3DS transcode URL
            if sec_type in ("movie", "show"):
                media_list = m.get("Media", [])
                if media_list:
                    part = media_list[0].get("Part", [{}])[0]
                    part_key = part.get("key")
                    
                    # 400x240 H264 baseline + AAC stereo universal transcode
                    transcode_url = (
                        f"{base_url}/video/:/transcode/universal/start.mp4?"
                        f"path={m.get('key')}&mediaIndex=0&partIndex=0&protocol=http&offset=0&"
                        f"fastSeek=1&directPlay=0&directStream=0&videoQuality=60&maxVideoBitrate=1200&"
                        f"videoResolution=400x240&videoCodec=h264&audioCodec=aac&"
                        f"X-Plex-Platform=CTR&X-Plex-Token={token}"
                    )
                    print(f"           3DS Video Transcode URL preview:")
                    print(f"           {transcode_url[:100]}...")

    # Export sample config for 3DS
    config = {
        "clientIdentifier": CLIENT_ID,
        "token": token,
        "serverName": server["name"],
        "serverUrl": base_url,
        "videoResolution": "400x240",
        "videoBitrate": 1000,
        "audioCodec": "aac"
    }
    out_path = os.path.join(PROJECT_DIR, "config.json")
    with open(out_path, "w") as f:
        json.dump(config, f, indent=2)
    print(f"\n[+] Saved test config to '{out_path}'")

def main():
    token = authenticate_pin()
    if not token:
        return
    servers = discover_servers(token)
    if servers:
        inspect_library(servers[0])
    else:
        print("[!] No active Plex servers found on this account.")

if __name__ == "__main__":
    main()
