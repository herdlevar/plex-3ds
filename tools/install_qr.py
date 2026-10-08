import http.server
import socket
import socketserver
import threading
import webbrowser
import os
import sys

PORT = 8000

def get_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip

def generate_html(target_url):
    js_path = os.path.join(os.path.dirname(__file__), "qrcode.min.js")
    js_content = ""
    if os.path.exists(js_path):
        with open(js_path, "r", encoding="utf-8") as f:
            js_content = f.read()

    return f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Plex3DS FBI Wireless Installer</title>
    <style>
        body {{
            background: #1a1d20;
            color: #e0e0e0;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            margin: 0;
            padding: 20px;
            box-sizing: border-box;
        }}
        .card {{
            background: #24282c;
            border: 1px solid #33383f;
            border-radius: 12px;
            padding: 32px;
            text-align: center;
            max-width: 480px;
            box-shadow: 0 8px 24px rgba(0,0,0,0.4);
        }}
        h1 {{
            margin-top: 0;
            color: #e5a00d;
            font-size: 26px;
        }}
        p {{
            line-height: 1.5;
            color: #b0b5be;
            margin-bottom: 24px;
        }}
        #qrcode {{
            background: white;
            padding: 16px;
            border-radius: 8px;
            display: inline-block;
            margin-bottom: 20px;
        }}
        .url-box {{
            background: #16181b;
            padding: 10px 16px;
            border-radius: 6px;
            font-family: monospace;
            font-size: 14px;
            color: #e5a00d;
            word-break: break-all;
        }}
        ol {{
            text-align: left;
            margin: 20px 0 0 0;
            padding-left: 20px;
            color: #9da3ad;
            font-size: 14px;
            line-height: 1.7;
        }}
        li strong {{
            color: #ffffff;
        }}
    </style>
    <script>{js_content}</script>
</head>
<body>
    <div class="card">
        <h1>Plex3DS Wireless FBI Install</h1>
        <p>Scan this QR code with <strong>FBI</strong> on your 3DS to install Plex3DS directly to your HOME Menu without taking out the SD card.</p>
        
        <div id="qrcode"></div>

        <div class="url-box">{target_url}</div>

        <ol>
            <li>Open <strong>FBI</strong> on your Nintendo 3DS.</li>
            <li>Select <strong>Remote Install</strong> &rarr; <strong>Scan QR Code</strong>.</li>
            <li>Point your 3DS camera at the code above.</li>
            <li>Press <strong>(A)</strong> to confirm install, then return to HOME Menu!</li>
        </ol>
    </div>

    <script>
        new QRCode(document.getElementById("qrcode"), {{
            text: "{target_url}",
            width: 256,
            height: 256,
            correctLevel: QRCode.CorrectLevel.M
        }});
    </script>
</body>
</html>
"""

class CustomHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/qr" or self.path == "/index.html":
            lan_ip = get_lan_ip()
            target_url = f"http://{lan_ip}:{PORT}/Plex3DS.cia"
            html = generate_html(target_url).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return
        return super().do_GET()

    def log_message(self, format, *args):
        # Clean logging
        msg = format % args
        if "GET /Plex3DS.cia" in msg:
            print(f"[FBI CONNECTED] Streaming Plex3DS.cia to 3DS: {msg}")
        elif "GET /" not in msg and "GET /favicon.ico" not in msg:
            print(f"[HTTP] {msg}")

def main():
    cia_path = os.path.join(os.getcwd(), "Plex3DS.cia")
    if not os.path.exists(cia_path):
        print(f"[ERROR] Plex3DS.cia not found in {os.getcwd()}!")
        print("Please build or place Plex3DS.cia first.")
        sys.exit(1)

    lan_ip = get_lan_ip()
    target_url = f"http://{lan_ip}:{PORT}/Plex3DS.cia"

    print("=" * 65)
    print("      Plex3DS FBI Wireless QR Code Installer Server")
    print("=" * 65)
    print(f"[*] Local LAN IP detected: {lan_ip}")
    print(f"[*] Serving Plex3DS.cia at: {target_url}")
    print(f"[*] Web UI with QR Code:   http://localhost:{PORT}/")
    print("=" * 65)
    print("Instructions:")
    print(" 1. Opening your web browser with the QR code now...")
    print(" 2. On your 3DS, open FBI -> Remote Install -> Scan QR Code")
    print(" 3. Point the 3DS camera at the browser screen and press (A)")
    print("=" * 65)
    print("Press Ctrl+C to stop the server when installation is complete.\n")

    webbrowser.open(f"http://localhost:{PORT}/")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), CustomHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")

if __name__ == "__main__":
    main()
