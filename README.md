# Plex3DS 🎬

[![Build Status](https://github.com/Jared/plex-3ds/actions/workflows/build.yml/badge.svg)](https://github.com/Jared/plex-3ds/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Nintendo%203DS-red.svg)](https://github.com/devkitPro/libctru)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-brightgreen.svg)]()

A high-performance, native **Plex Media Server client** engineered from scratch in modern C++17 for the **Nintendo 3DS** (optimized for New Nintendo 3DS / New 2DS XL).

Plex3DS offloads heavy media transcoding to your Plex Media Server, delivering lightweight **400×240 H.264 Baseline** video and **AAC/MP3 audio** streams perfectly tailored for the 3DS display and ARM11 architecture.

---

## ✨ Features

* **📱 Instant QR Code & PIN Sign-In:**
  * Scan the on-screen QR code with your smartphone camera to authenticate instantly at `plex.tv/link`.
  * Fallback 4-digit link code or direct username/password sign-in with on-screen keyboard (SWKBD).
  * Plaintext password memory buffers are wiped immediately after authentication.

* **🖥️ Dual-Screen Citro2D Interface:**
  * **Top Screen (400×240):** High-resolution poster artwork, live metadata previews (synopsis, director/artist, release year, duration), and playback status.
  * **Bottom Screen (320×240):** Touch-driven library navigation, character-by-character horizontal marquee scrolling for long titles, and interactive playback scrubbers.

* **🎬 Hardware-Tuned Video Playback:**
  * Live Plex universal transcoding to 400×240 H.264 Baseline at 24/30 FPS via bundled ARMv6k FFmpeg.
  * Subtitle stream selection and automatic server-side burn-in.
  * **Smart Resume / Restart:** Asks to resume from your last position or restart from the beginning, automatically persisting progress in `resume.json`.

* **🎵 Clamshell Music Playback & Pocket Mode:**
  * Listen to albums and audio tracks with the 3DS clamshell closed like a dedicated portable MP3 player.
  * Bypasses standard console sleep mode (`aptSetSleepAllowed(false)`) to maintain audio decoding threads while powering off display backlights to maximize battery life.
  * **Pocket Controls:** Use the physical `(L)` and `(R)` shoulder buttons to skip tracks, and press `(START)` or `(L + R)` to toggle play/pause directly through your pocket without opening the console.

* **💡 Intelligent Screen Dimming & Touch Protection:**
  * Bottom screen backlight automatically turns off during video (10s) and music (15s) playback to conserve battery and eliminate glare.
  * **Accidental Touch Prevention:** The first touch on a dimmed screen simply restores brightness without triggering any UI buttons.

* **💾 Offline Media Downloads (Zero-Wi-Fi Playback):**
  * Download TV episodes, movies, and music tracks directly to your SD card (`sdmc:/3ds/plex-3ds/downloads/`).
  * **100% Offline Access:** Press `(L)` or tap `[Downloads (L)]` on the server selection or sign-in screens to access your downloaded library at any time—even with 0% Wi-Fi or no internet connection.
  * If launched offline with downloaded media on SD, Plex3DS automatically opens your offline library.
  * Full path traversal protection, background download progress tracking, and automatic cleanup of partial files.

* **🌐 Multi-Server & Paginated Libraries:**
  * Discover and switch between multiple local and remote Plex servers seamlessly.
  * Automatic pagination (`--> [Load Next 100 Items...]`) for massive libraries without memory spikes.

* **🔒 Security & Graceful Home Menu Handling:**
  * Full `aptHook` integration for clean exits via the HOME Menu or Power button without console hangs or audio buzzing.
  * Optional custom CA certificate verification via `sdmc:/3ds/plex-3ds/cacert.pem`.

---

## 🎧 Clamshell Pocket Listening & Offline Media

### Why Offline Downloads for Clamshell Playback?
The Nintendo 3DS OS firmware automatically powers down the wireless radio at the hardware level whenever the clamshell lid is shut. As a result, active TCP connections drop and the 3DS cannot fetch new media streams over Wi-Fi while closed.

Plex3DS solves this with two complementary systems:
1. **Full-Track RAM Buffering (Streaming):** When streaming audio online, Plex3DS downloads the entire track into memory within 2–3 seconds at maximum Wi-Fi throughput. The current song continues playing smoothly through your headphones even after you close the lid.
2. **Offline Downloads (Recommended for Walkman Mode):** By downloading albums, playlists, or videos to your SD card, playback has **zero network dependencies**. When playing downloaded tracks with the lid closed, Plex3DS seamlessly auto-advances through your entire playlist track by track.

### Pocket Controls (Lid Closed)
| Input | Action |
| :--- | :--- |
| **L Shoulder** | Previous track |
| **R Shoulder** | Next track |
| **START** or **L + R** | Toggle play / pause |

---

## 🎮 Controls

### Global & Offline Shortcuts
| Input | Action |
| :--- | :--- |
| **L Shoulder** | Open **Offline Downloads** (from Server Select, Sign-In, or Library screen) |
| **START** | Gracefully exit application |

### Library & Server Navigation
| Input | Action |
| :--- | :--- |
| **D-Pad Up / Down** or **Circle Pad** | Scroll through servers, libraries, and media items |
| **A** or **Touch Item** | Select / Open item details / Connect to server |
| **B** | Return to previous screen / Return from Downloads |
| **X** | Remove selected server (on Server Select) or delete downloaded item (in Downloads) |
| **Y** | Sync servers from Plex account |
| **SELECT** | Switch to Plex Account / Sign-In screen |

### Media Detail View
| Input | Action |
| :--- | :--- |
| **A** or **Touch Play / Resume** | Start playback (or resume from saved timestamp) |
| **Y** or **Touch Restart** | Restart playback from beginning (0:00) |
| **X** or **Touch Download / Delete** | Download item to SD card, or delete existing download |
| **SELECT** or **Touch CC** | Cycle subtitle tracks and closed captions (Video) |
| **B** or **Touch Back** | Return to item list |

### Media Playback
| Input | Action |
| :--- | :--- |
| **A** or **Touch Play/Pause** | Toggle playback |
| **D-Pad Left / Right** | Seek backward / forward 15 seconds |
| **Touch Progress Bar** | Jump / scrub directly to timestamp |
| **L / R Shoulders** | Previous / Next audio track (Music player) |
| **B** or **Touch Stop** | Stop playback and return to browser |
| **Touch Screen (When Dimmed)** | Wake display (suppresses accidental button click) |

---

## 📦 Installation

### Option 1: Direct HOME Menu Icon via FBI (Recommended)
Tired of opening Rosalina Menu (`L + Down + Select`) and taking over a title every time you use homebrew? Install the Homebrew Launcher as a standalone title on your 3DS HOME Menu:

1. Download [`Homebrew_Launcher.cia`](https://github.com/PabloMK7/homebrew_launcher/releases) (or grab the bundled copy from this repository).
2. Copy `Homebrew_Launcher.cia` to your 3DS SD card.
3. Open **FBI** on your 3DS, navigate to `SD` -> locate `Homebrew_Launcher.cia`, and select **Install and delete CIA**.
4. Press `HOME` to unwrap the **Homebrew Launcher** gift box on your HOME Menu.
5. Place `Plex3DS.3dsx` and `Plex3DS.smdh` in `sdmc:/3ds/Plex3DS/`.
6. Launch **Homebrew Launcher** directly from your HOME Menu anytime and select **Plex3DS**!

### Option 2: Manual SD Card Install
1. Download the latest `Plex3DS.3dsx` and `Plex3DS.smdh` from the [Releases](https://github.com/Jared/plex-3ds/releases) page.
2. Insert your 3DS SD card into your computer.
3. Place `Plex3DS.3dsx` and `Plex3DS.smdh` into `/3ds/Plex3DS/` on your SD card:
   ```text
   sdmc:/
   └── 3ds/
       └── Plex3DS/
           ├── Plex3DS.3dsx
           └── Plex3DS.smdh
   ```
4. Launch via **Homebrew Launcher**.

### Option 3: Wireless Auto-Deployment (NetLoader)
If your 3DS is connected to Wi-Fi:
* In Homebrew Launcher, press `Y` to activate NetLoader, then send the binary directly:
  ```bash
  3dslink -a <3DS_IP> Plex3DS.3dsx
  ```
  or run:
  ```bash
  python tools/auto_deploy.py <3DS_IP>
  ```

---

## ⚙️ Configuration

On first launch, Plex3DS will generate a QR code and link PIN. Once paired, your server configuration and credentials are saved to `sdmc:/3ds/plex-3ds/config.json`.

You can also pre-configure a server manually by copying [`config.example.json`](config.example.json) to `sdmc:/3ds/plex-3ds/config.json`:

```json
{
  "clientIdentifier": "plex-3ds-custom",
  "token": "YOUR_PLEX_TOKEN",
  "serverName": "MyPlexServer",
  "serverUrl": "http://192.168.1.100:32400",
  "videoResolution": "400x240",
  "videoBitrate": 1000,
  "audioCodec": "aac"
}
```

### Custom SSL / HTTPS Certificates
If your Plex server uses a private certificate authority (CA) or self-signed certificate, copy your PEM-formatted root bundle to:
`sdmc:/3ds/plex-3ds/cacert.pem`

---

## 🛠️ Building from Source

### Prerequisites
Install the [devkitPro](https://devkitpro.org/wiki/Getting_Started) toolchain with the **Nintendo 3DS development** group:

```bash
# On Arch / Manjaro or using dkp-pacman:
sudo dkp-pacman -S devkitARM libctru citro2d citro3d 3ds-curl 3ds-mpg123 3ds-flac 3ds-libogg
```

### 1. Run Unit Tests
Plex3DS includes an automated test suite verifying time formatters, transcode URL builders, path sanitizers, and resume logic:

```bash
make test
```

### 2. Compile Executable
```bash
make -j$(nproc)
```

This will produce `Plex3DS.3dsx`, `Plex3DS.smdh`, and `Plex3DS.elf`.

---

## 📂 Project Architecture

```text
plex-3ds/
├── .github/workflows/       # GitHub Actions CI & automated releases
├── external/                # Bundled ARMv6k FFmpeg static libraries & headers
│   ├── include/             # libavcodec, libavformat, libavutil, libswscale
│   └── lib/                 # Precompiled static archives for 3DS
├── include/                 # Header declarations & lightweight dependencies
│   ├── types.hpp            # Server, library, and media data structures
│   ├── cJSON.h              # Fast ANSI C JSON parser
│   └── qrcodegen.h          # Embedded QR Code generator
├── source/                  # Implementation
│   ├── main.cpp             # State machine, input handler & event loop
│   ├── network/             # libcurl HTTP/HTTPS client with TLS support
│   ├── plex/                # Plex REST API, PIN pairing & XML/JSON models
│   ├── player/              # NDSP audio stream buffer & FFmpeg video pipeline
│   ├── download/            # Offline download manager with path sanitization
│   └── ui/                  # Citro2D / Citro3D dual-screen renderer
├── tests/                   # Core logic regression test suite
├── tools/                   # Wireless deployment & network discovery utilities
├── Makefile                 # devkitARM makefile
└── config.example.json      # Sample configuration template
```

---

## 🤝 Contributing

Contributions, issues, and feature requests are welcome! Feel free to check out the [issues page](https://github.com/Jared/plex-3ds/issues).

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Run unit tests (`make test`)
4. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
5. Push to the branch (`git push origin feature/AmazingFeature`)
6. Open a Pull Request

---

## 📄 License

Distributed under the **MIT License**. See [`LICENSE`](LICENSE) for more information.

### Acknowledgments
* **[devkitPro](https://devkitpro.org/)** & **[libctru](https://github.com/devkitPro/libctru)** developers for making 3DS homebrew possible.
* **[Core-2-Extreme](https://github.com/Core-2-Extreme/Video_player_for_3DS)** for 3DS video player reference implementations.
* **[Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library)** for the fast, embedded QR code generation library.
* **[Dave Gamble](https://github.com/DaveGamble/cJSON)** for the ultra-lightweight cJSON library.
