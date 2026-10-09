#include "ui.hpp"
#include "qrcodegen.h"

#ifdef __3DS__
#include <citro3d.h>
#endif

#include <algorithm>

// Plex Color Palette
#define COLOR_PLEX_ORANGE C2D_Color32(0xE5, 0xA0, 0x0D, 0xFF)
#define COLOR_DARK_BG     C2D_Color32(0x1F, 0x23, 0x26, 0xFF)
#define COLOR_PANEL_BG    C2D_Color32(0x28, 0x2C, 0x30, 0xFF)
#define COLOR_WHITE       C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define COLOR_GRAY        C2D_Color32(0x9E, 0x9E, 0x9E, 0xFF)
#define COLOR_HIGHLIGHT   C2D_Color32(0x3B, 0x42, 0x48, 0xFF)

UIRenderer::UIRenderer() {}

UIRenderer::~UIRenderer() {
    exit();
}

bool UIRenderer::init() {
    if (m_initialized) return true;
#ifdef __3DS__
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    m_topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    m_bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    m_staticTextBuf = C2D_TextBufNew(4096);
    m_dynamicTextBuf = C2D_TextBufNew(4096);
    if (!m_topTarget || !m_bottomTarget) return false;
#endif
    m_initialized = true;
    return true;
}

void UIRenderer::exit() {
#ifdef __3DS__
    if (!m_initialized) return;
    if (m_dynamicTextBuf) {
        C2D_TextBufDelete(m_dynamicTextBuf);
        m_dynamicTextBuf = nullptr;
    }
    if (m_staticTextBuf) {
        C2D_TextBufDelete(m_staticTextBuf);
        m_staticTextBuf = nullptr;
    }
    m_topTarget = nullptr;
    m_bottomTarget = nullptr;
    if (!g_gpuRightLost.load()) {
        C2D_Fini();
        C3D_Fini();
    }
    gfxExit();
#endif
    m_initialized = false;
}

void UIRenderer::beginFrame() {
#ifdef __3DS__
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(m_dynamicTextBuf);
#endif
}

void UIRenderer::endFrame() {
#ifdef __3DS__
    C3D_FrameEnd(0);
#endif
}

void UIRenderer::drawHeader(const std::string& title, float screenWidth) {
#ifdef __3DS__
    C2D_DrawRectSolid(0, 0, 0.5f, screenWidth, 24, COLOR_PLEX_ORANGE);
    drawText(8, 4, 0.55f, C2D_Color32(0x00, 0x00, 0x00, 0xFF), title);
#endif
}

void UIRenderer::drawButton(float x, float y, float w, float h, const std::string& label, bool highlighted, float depth) {
#ifdef __3DS__
    uint32_t bg = highlighted ? COLOR_PLEX_ORANGE : COLOR_PANEL_BG;
    uint32_t textCol = highlighted ? C2D_Color32(0, 0, 0, 255) : COLOR_WHITE;
    C2D_DrawRectSolid(x, y, depth, w, h, bg);
    drawText(x + (w <= 75.0f ? 6.0f : 10.0f), y + (h / 4), (w <= 75.0f ? 0.42f : 0.5f), textCol, label, depth + 0.05f);
#endif
}

void UIRenderer::drawText(float x, float y, float size, uint32_t color, const std::string& text, float depth) {
#ifdef __3DS__
    if (text.empty()) return;
    C2D_Text c2dText;
    C2D_TextParse(&c2dText, m_dynamicTextBuf, text.c_str());
    C2D_TextOptimize(&c2dText);
    C2D_DrawText(&c2dText, C2D_WithColor, x, y, depth, size, size, color);
#endif
}

#include "player/video_player.hpp"
#include "player/audio_player.hpp"

static std::string formatTime(int totalSeconds) {
    if (totalSeconds < 0) totalSeconds = 0;
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;
    char buf[32];
    if (hours > 0) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
    }
    return std::string(buf);
}

static std::string formatTimePair(int curSec, int totSec) {
    if (curSec < 0) curSec = 0;
    if (totSec < 0) totSec = 0;
    char buf[64];
    if (totSec >= 3600) {
        int ch = curSec / 3600;
        int cm = (curSec % 3600) / 60;
        int cs = curSec % 60;
        int th = totSec / 3600;
        int tm = (totSec % 3600) / 60;
        int ts = totSec % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d / %02d:%02d:%02d", ch, cm, cs, th, tm, ts);
    } else {
        int cm = curSec / 60;
        int cs = curSec % 60;
        int tm = totSec / 60;
        int ts = totSec % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d", cm, cs, tm, ts);
    }
    return std::string(buf);
}

void UIRenderer::drawQRCode(float startX, float startY, const std::string& text, int scale) {
#ifdef __3DS__
    if (text.empty()) return;
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(text.c_str(), tempBuffer, qrcode,
                                   qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, 10,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) return;

    int qrSize = qrcodegen_getSize(qrcode);
    int totalPx = qrSize * scale;
    int border = scale * 2; // 2-module quiet border

    // Draw white quiet zone background
    C2D_DrawRectSolid(startX - border, startY - border, 0.5f, totalPx + border * 2, totalPx + border * 2, C2D_Color32(255, 255, 255, 255));

    // Draw dark modules
    uint32_t black = C2D_Color32(0, 0, 0, 255);
    for (int y = 0; y < qrSize; y++) {
        for (int x = 0; x < qrSize; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                C2D_DrawRectSolid(startX + x * scale, startY + y * scale, 0.6f, scale, scale, black);
            }
        }
    }
#else
    (void)startX; (void)startY; (void)text; (void)scale;
#endif
}

void UIRenderer::renderTopScreen(AppState state, 
                                 const PlexMediaItem* nowPlayingItem,
                                 bool isMediaPlaying,
                                 const PlexMediaItem* selectedItem, 
                                 const std::string& statusMessage, 
                                 const VideoPlayer* videoPlayer,
                                 const AudioPlayer* audioPlayer,
                                 const std::string& username,
                                 bool isLoggedIn,
                                 const std::string& pinCode) {
#ifdef __3DS__
    C2D_TargetClear(m_topTarget, COLOR_DARK_BG);
    C2D_SceneBegin(m_topTarget);

    // 1. Video Playback Active (continuous top screen)
    if (videoPlayer && (videoPlayer->isPlaying() || videoPlayer->hasFrame())) {
        if (videoPlayer->hasFrame()) {
            C2D_DrawImageAt(videoPlayer->getImage(), 0, 0, 0.5f, NULL, 1.0f, 1.0f);
            if (!statusMessage.empty()) {
                float pillW = 280.0f;
                float pillH = 24.0f;
                float pillX = (400.0f - pillW) / 2.0f;
                float pillY = 10.0f;
                C2D_DrawRectSolid(pillX, pillY, 0.6f, pillW, pillH, C2D_Color32(20, 20, 25, 220));
                C2D_DrawRectSolid(pillX, pillY, 0.61f, 3.0f, pillH, COLOR_PLEX_ORANGE);
                drawText(pillX + 10.0f, pillY + 4.0f, 0.42f, COLOR_WHITE, statusMessage, 0.82f);
            }
            return;
        } else {
            // Connecting or buffering
            drawHeader("NOW PLAYING VIDEO", 400);
            if (nowPlayingItem) {
                drawText(20, 42, 0.65f, COLOR_PLEX_ORANGE, nowPlayingItem->title);
                if (!nowPlayingItem->parentTitle.empty()) {
                    drawText(20, 68, 0.5f, COLOR_WHITE, nowPlayingItem->parentTitle);
                }
            } else {
                drawText(20, 42, 0.65f, COLOR_PLEX_ORANGE, "Streaming Video...");
            }

            static const char* s_spinners[] = { "|", "/", "-", "\\" };
            static uint32_t s_spinTick = 0;
            s_spinTick++;
            const char* spin = s_spinners[(s_spinTick / 8) % 4];

            int elapsed = videoPlayer->getElapsedConnectSec();
            int64_t kb = videoPlayer->getBytesReceived() / 1024;

            if (videoPlayer->isLocalFile()) {
                std::string statusLine = videoPlayer->getStatusMessage();
                drawText(20, 96, 0.5f, COLOR_WHITE, statusLine + " " + std::string(spin));
                drawText(20, 122, 0.45f, COLOR_PLEX_ORANGE, "Loading offline video from SD card...");
                drawText(20, 146, 0.42f, COLOR_GRAY, "Initializing hardware video player...");
            } else if (kb == 0) {
                std::string statusLine = videoPlayer->getStatusMessage();
                if (statusLine == "Connecting to Plex transcode server..." || statusLine == "Buffering stream from Plex...") {
                    statusLine = "Connecting to Plex transcoder " + std::string(spin) + " (" + std::to_string(elapsed) + "s)";
                }
                drawText(20, 96, 0.5f, COLOR_WHITE, statusLine);
                drawText(20, 122, 0.45f, COLOR_PLEX_ORANGE, "Server is preparing H.264 video stream...");
                drawText(20, 146, 0.42f, COLOR_GRAY, "Initial transcode startup typically takes 10-15s");
            } else {
                drawText(20, 96, 0.5f, COLOR_WHITE, "Buffering stream: " + std::to_string(kb) + " KB / 256 KB " + std::string(spin));
                // Buffer progress bar
                C2D_DrawRectSolid(20, 122, 0.5f, 360, 8, COLOR_PANEL_BG);
                float bufRatio = std::clamp((float)kb / 256.0f, 0.0f, 1.0f);
                C2D_DrawRectSolid(20, 122, 0.5f, bufRatio * 360.0f, 8, COLOR_PLEX_ORANGE);
                drawText(20, 146, 0.42f, COLOR_GRAY, "Starting video playback when pre-buffer fills...");
            }

            drawText(20, 185, 0.42f, COLOR_GRAY, "(Controls on bottom screen | Browse library anytime)");
            return;
        }
    }

    // 2. Audio Playback Active (continuous top screen)
    if (audioPlayer && audioPlayer->isPlaying()) {
        drawHeader("NOW PLAYING AUDIO", 400);
        if (nowPlayingItem) {
            drawText(20, 40, 0.65f, COLOR_PLEX_ORANGE, nowPlayingItem->title);
            if (!nowPlayingItem->parentTitle.empty()) {
                drawText(20, 70, 0.55f, COLOR_WHITE, nowPlayingItem->parentTitle);
            }
            if (nowPlayingItem->year > 0) {
                drawText(20, 95, 0.45f, COLOR_GRAY, "Year: " + std::to_string(nowPlayingItem->year));
            }
        } else {
            drawText(20, 40, 0.65f, COLOR_PLEX_ORANGE, "Audio Stream");
        }
        int cur = audioPlayer->getCurrentSeconds();
        int tot = audioPlayer->getTotalSeconds();
        std::string timeStr = formatTimePair(cur, tot);
        drawText(20, 125, 0.6f, COLOR_WHITE, timeStr);

        // Progress bar on top screen
        C2D_DrawRectSolid(20, 155, 0.5f, 360, 6, COLOR_PANEL_BG);
        if (tot > 0) {
            float f = std::clamp((float)cur / (float)tot, 0.0f, 1.0f);
            C2D_DrawRectSolid(20, 155, 0.5f, f * 360.0f, 6, COLOR_PLEX_ORANGE);
        }
        if (!statusMessage.empty()) {
            drawText(20, 195, 0.45f, COLOR_PLEX_ORANGE, statusMessage);
        } else {
            drawText(20, 195, 0.45f, COLOR_GRAY, "(Controls on bottom screen | Browse library anytime)");
        }
        return;
    }

    // 3. Normal State Top Screen (when no media is active)
    drawHeader("PLEX FOR 3DS", 400);

    if (state == AppState::PIN_AUTH) {
        if (isLoggedIn) {
            drawText(20, 50, 0.65f, COLOR_WHITE, "Signed In Account");
            std::string userDisplay = username.empty() ? "Connected User" : ("Username: " + username);
            drawText(20, 80, 0.58f, COLOR_PLEX_ORANGE, userDisplay);
            drawText(20, 115, 0.48f, COLOR_WHITE, "Authentication Token: Active");
            drawText(20, 140, 0.45f, COLOR_GRAY, "Manage servers or switch account on bottom screen.");
            drawText(20, 195, 0.45f, COLOR_PLEX_ORANGE, "(B) Return to Servers   (X) Log Out");
        } else {
            // Left Column (x = 15..240): Instructions & 4-letter code
            drawText(15, 36, 0.58f, COLOR_WHITE, "Link Plex Account");
            drawText(15, 60, 0.44f, COLOR_GRAY, "Scan QR code with phone,");
            drawText(15, 78, 0.44f, COLOR_GRAY, "or go to plex.tv/link");
            drawText(15, 104, 0.44f, COLOR_GRAY, "4-DIGIT PIN CODE:");
            drawText(15, 124, 0.95f, COLOR_PLEX_ORANGE, pinCode.empty() ? "...." : pinCode);
            if (!statusMessage.empty()) {
                std::string sMsg = statusMessage;
                if (sMsg.length() > 28) sMsg = sMsg.substr(0, 26) + "..";
                drawText(15, 168, 0.42f, COLOR_WHITE, sMsg);
            }
            drawText(15, 198, 0.38f, COLOR_GRAY, "(Controls on bottom screen)");

            // Right Column (x = 250..390): Beautiful Scannable QR Code!
            std::string qrUrl = "https://plex.tv/link";
            if (!pinCode.empty()) {
                qrUrl += "?code=" + pinCode;
            }
            drawQRCode(258, 42, qrUrl, 4);
            drawText(244, 180, 0.40f, COLOR_PLEX_ORANGE, "Scan with Phone");
        }
    } else if (state == AppState::SERVER_SELECT) {
        drawText(20, 45, 0.65f, COLOR_WHITE, "Plex Media Server Selection");
        if (isLoggedIn && !username.empty()) {
            drawText(20, 75, 0.48f, COLOR_PLEX_ORANGE, "Account: " + username);
        } else {
            drawText(20, 75, 0.48f, COLOR_GRAY, "Account: Not signed in (Local mode)");
        }
        if (!statusMessage.empty()) {
            drawText(20, 105, 0.48f, COLOR_WHITE, statusMessage);
        }
        drawText(20, 175, 0.42f, COLOR_GRAY, "(A) Connect    (X) Remove Selected Server");
        drawText(20, 195, 0.42f, COLOR_GRAY, "(Y) Sync Servers    (SELECT) Account Settings");
    } else if ((state == AppState::DETAIL_VIEW || state == AppState::ITEM_LIST) && selectedItem) {
        if (selectedItem->key == "__LOAD_MORE__") {
            drawText(20, 35, 0.65f, COLOR_PLEX_ORANGE, "Load More Items");
            drawText(20, 75, 0.48f, COLOR_WHITE, "Select this item to fetch the next 100 entries.");
            drawText(20, 195, 0.45f, COLOR_GRAY, "(A) Load Next 100   (B) Back");
        } else {
            drawText(20, 35, 0.65f, COLOR_WHITE, selectedItem->title);
            if (!selectedItem->parentTitle.empty()) {
                drawText(20, 58, 0.5f, COLOR_PLEX_ORANGE, selectedItem->parentTitle);
            }
            if (selectedItem->year > 0) {
                drawText(20, 78, 0.45f, COLOR_GRAY, "Year: " + std::to_string(selectedItem->year));
            }

            std::string summary = selectedItem->summary;
            if (summary.empty() && selectedItem->type == MediaType::TRACK) {
                summary = "Audio Track";
            }
            if (summary.length() > 220) summary = summary.substr(0, 217) + "...";
            drawText(20, 100, 0.45f, COLOR_WHITE, summary);

            if (state == AppState::ITEM_LIST) {
                drawText(20, 195, 0.45f, COLOR_GRAY, "(A) Open / Play   (X) Delete DL   (B) Back");
            } else if (selectedItem->type == MediaType::TRACK) {
                drawText(20, 195, 0.5f, COLOR_PLEX_ORANGE, "(A) Play Music   (B) Back");
            } else {
                bool hasResume = (selectedItem->viewOffsetMs > 10000) &&
                                 (selectedItem->durationMs <= 0 || selectedItem->viewOffsetMs < selectedItem->durationMs - 15000);
                if (hasResume) {
                    std::string resumeTime = formatTime((int)(selectedItem->viewOffsetMs / 1000));
                    drawText(20, 195, 0.44f, COLOR_PLEX_ORANGE, "(A) Resume [" + resumeTime + "]  (Y) Restart  (B) Back");
                } else {
                    drawText(20, 195, 0.5f, COLOR_PLEX_ORANGE, "(A) Play Video   (B) Back");
                }
            }
        }
    } else {
        // Standard Top Screen Dashboard
        drawText(20, 60, 0.6f, COLOR_WHITE, "Library Browser");
        if (!statusMessage.empty()) {
            drawText(20, 100, 0.5f, COLOR_GRAY, statusMessage);
        }
        drawText(20, 200, 0.45f, COLOR_GRAY, "(Circle Pad / D-Pad) Navigate   (A) Select   (B) Back");
    }
#endif
}

void UIRenderer::renderBottomScreen(AppState state,
                                    const std::string& pinCode,
                                    const std::vector<PlexServer>& servers,
                                    const std::vector<PlexLibrary>& libraries,
                                    const std::vector<PlexMediaItem>& items,
                                    int selectedIndex,
                                    int scrollOffset,
                                    const std::string& listTitle,
                                    bool isPaused,
                                    bool hasNowPlaying,
                                    const PlexMediaItem* nowPlayingItem,
                                    bool controlsExpanded,
                                    int currentSec,
                                    int totalSec,
                                    bool isScrubbing,
                                    int scrubSec,
                                    bool canPrev,
                                    bool canNext,
                                    bool isItemDownloaded,
                                    bool isDownloadingCurrent,
                                    int dlPercent,
                                    const std::string& dlBadge,
                                    bool subtitlesEnabled,
                                    const std::string& subtitleName,
                                    const std::string& username,
                                    bool isLoggedIn,
                                    bool isItemQueued) {
#ifdef __3DS__
    C2D_TargetClear(m_bottomTarget, COLOR_PANEL_BG);
    C2D_SceneBegin(m_bottomTarget);

    // MODE A: Expanded Playback Controls take over bottom screen
    if (controlsExpanded && hasNowPlaying) {
        drawHeader("NOW PLAYING", 320);
        if (!dlBadge.empty()) {
            drawText(230, 4, 0.45f, C2D_Color32(0, 0, 0, 255), dlBadge);
        }

        std::string title = nowPlayingItem ? nowPlayingItem->title : "Media";
        if (title.length() > 26) title = title.substr(0, 24) + "..";
        drawText(15, 28, 0.55f, COLOR_WHITE, title);

        std::string parent = (nowPlayingItem && !nowPlayingItem->parentTitle.empty()) ? nowPlayingItem->parentTitle : "";
        if (parent.length() > 30) parent = parent.substr(0, 28) + "..";
        if (!parent.empty()) {
            drawText(15, 48, 0.42f, COLOR_PLEX_ORANGE, parent);
        }

        // Playback scrubber bar (hit box is x: 10..310, y: 55..95)
        int dispSec = isScrubbing ? scrubSec : currentSec;
        float prog = (totalSec > 0) ? std::clamp((float)dispSec / (float)totalSec, 0.0f, 1.0f) : 0.0f;
        C2D_DrawRectSolid(15, 70, 0.5f, 290, 10, COLOR_DARK_BG);
        C2D_DrawRectSolid(15, 70, 0.5f, prog * 290.0f, 10, COLOR_PLEX_ORANGE);
        // Scrubber thumb
        C2D_DrawRectSolid(15.0f + prog * 290.0f - 2.0f, 66.0f, 0.6f, 4.0f, 18.0f, COLOR_WHITE);

        std::string timeStr = formatTimePair(dispSec, totalSec);
        drawText(15, 86, 0.48f, isScrubbing ? COLOR_PLEX_ORANGE : COLOR_WHITE, timeStr);

        // Control buttons row: [ |< ] [ <<15 ] [ PAUSE ] [ 15>> ] [ >| ]
        drawButton(10, 106, 55, 44, "|<", canPrev);
        drawButton(70, 106, 55, 44, "<< 15");
        drawButton(130, 104, 60, 48, isPaused ? "RESUME" : "PAUSE", true);
        drawButton(195, 106, 55, 44, "15 >>");
        drawButton(255, 106, 55, 44, ">|", canNext);

        // Navigation row
        if (nowPlayingItem && nowPlayingItem->type != MediaType::TRACK) {
            std::string ccText = "CC: OFF";
            if (subtitlesEnabled) {
                ccText = subtitleName.empty() ? "CC: ON" : ("CC: " + subtitleName);
                if (ccText.length() > 10) ccText = ccText.substr(0, 9) + ".";
            }
            drawButton(12, 158, 92, 38, "v Browse");
            drawButton(110, 158, 100, 38, ccText, subtitlesEnabled);
            drawButton(216, 158, 92, 38, "Stop");

            drawText(12, 208, 0.38f, COLOR_GRAY, "Drag bar: seek | (L/R) Next/Prev | (X) CC | (Y) Collapse");
        } else {
            drawButton(15, 160, 140, 36, "v Browse Library");
            drawButton(165, 160, 140, 36, "Stop Playback");

            drawText(15, 208, 0.40f, COLOR_GRAY, "Drag bar to seek | (L/R) Prev/Next | (Y) Collapse");
        }
        return;
    }

    // MODE B: Normal Screens (with optional Collapsed Mini-Player docked at bottom)
    if (state == AppState::PIN_AUTH) {
        if (isLoggedIn) {
            drawHeader("ACCOUNT SETTINGS", 320);
            drawText(15, 28, 0.44f, COLOR_GRAY, "Signed In User:");
            std::string uStr = username.empty() ? "(Logged In)" : username;
            if (uStr.length() > 24) uStr = uStr.substr(0, 22) + "..";
            drawText(15, 46, 0.65f, COLOR_PLEX_ORANGE, uStr);

            // Action Buttons:
            drawButton(15, 75, 290, 34, "Sync Servers from Account", false);
            drawButton(15, 115, 290, 34, "Sign In with Different Account", false);
            drawButton(15, 155, 290, 34, "Log Out / Unlink Account", false);
            drawButton(15, 195, 290, 34, "< Back to Servers", true);
        } else {
            drawHeader("PLEX SIGN IN & LINK", 320);

            // Left box: 4-digit PIN (x: 15..148, y: 30..82)
            C2D_DrawRectSolid(15, 30, 0.5f, 133, 52, COLOR_DARK_BG);
            drawText(22, 34, 0.40f, COLOR_GRAY, "LINK CODE (plex.tv/link):");
            drawText(36, 52, 0.95f, COLOR_PLEX_ORANGE, pinCode.empty() ? "...." : pinCode);

            // Right button: [ Sign In Email ] (x: 158..305, y: 30..82)
            drawButton(158, 30, 147, 52, "Sign In Email", true);

            // Row 2: [+ Add Server by Local IP] (x: 15..305, y: 90..124)
            drawButton(15, 90, 290, 34, "+ Add Server by Local IP", false);

            // Row 3: [Refresh Link] (x: 15..150, y: 130..164) and [Downloads (L)] (x: 155..305, y: 130..164)
            drawButton(15, 130, 135, 34, "Refresh Link", false);
            drawButton(155, 130, 150, 34, "Downloads (L)", false);

            // Row 4: If servers already exist, show [Back to Servers] (x: 15..305, y: 170..204)
            if (!servers.empty()) {
                drawButton(15, 170, 290, 34, "< Back to Servers", false);
                drawText(15, 214, 0.38f, COLOR_GRAY, "(L) Offline Downloads  (B) Back to Servers");
            } else {
                drawText(15, 178, 0.42f, COLOR_GRAY, "Enter link code on your phone or PC browser,");
                drawText(15, 198, 0.42f, COLOR_GRAY, "or tap 'Sign In Email' to use the 3DS keyboard.");
                drawText(15, 214, 0.38f, COLOR_GRAY, "Press (L) anytime to open Offline Downloads.");
            }
        }
        return;
    } else if (state == AppState::SERVER_SELECT) {
        drawHeader("SELECT PLEX SERVER", 320);
        if (!dlBadge.empty()) {
            drawText(230, 4, 0.45f, C2D_Color32(0, 0, 0, 255), dlBadge);
        }

        int maxVis = hasNowPlaying ? 2 : 3;
        float itemH = 30.0f;
        float y = 28.0f;

        if (servers.empty()) {
            drawText(20, 50, 0.5f, COLOR_GRAY, "No servers configured yet.");
            drawText(20, 72, 0.45f, COLOR_WHITE, "Tap [+ Add IP] or [Sync] below to connect.");
        } else {
            for (int i = 0; i < maxVis; i++) {
                int idx = scrollOffset + i;
                if (idx >= (int)servers.size()) break;
                bool sel = (idx == selectedIndex);
                std::string sName = servers[idx].name;
                if (servers[idx].isCustom) {
                    sName += " (Direct)";
                }
                drawButton(15, y, 290, itemH, sName, sel);
                y += (itemH + 4.0f);
            }
        }

        // Row 1 Action buttons: [+ Add IP] [Sync (Y)] [Remove (X)]
        float btnY1 = hasNowPlaying ? 96.0f : 132.0f;
        float btnH = hasNowPlaying ? 28.0f : 32.0f;
        drawButton(15, btnY1, 92, btnH, "+ Add IP");
        drawButton(114, btnY1, 92, btnH, "Sync (Y)");
        drawButton(213, btnY1, 92, btnH, "Remove (X)");

        // Row 2 Action buttons: [Downloads (B)] and [Account: ...]
        float btnY2 = hasNowPlaying ? 128.0f : 168.0f;
        drawButton(15, btnY2, 135, btnH, "Downloads (B)", false);

        std::string acctLabel;
        if (isLoggedIn && !username.empty()) {
            acctLabel = username;
        } else if (isLoggedIn) {
            acctLabel = "Signed In";
        } else {
            acctLabel = "Link/Login";
        }
        if (acctLabel.length() > 16) acctLabel = acctLabel.substr(0, 14) + "..";
        drawButton(155, btnY2, 150, btnH, "Account: " + acctLabel, false);

        if (!hasNowPlaying) {
            drawText(15, 214, 0.38f, COLOR_GRAY, "(A) Connect  (B) Downloads  (X) Del  (Y) Sync  (Sel) Acct");
        }
    } else if (state == AppState::LIBRARY_LIST) {
        drawHeader("SECTIONS", 320);
        if (!dlBadge.empty()) {
            drawText(230, 4, 0.45f, C2D_Color32(0, 0, 0, 255), dlBadge);
        }
        float y = 35;
        int maxVis = hasNowPlaying ? 4 : 5;
        for (int i = 0; i < maxVis; i++) {
            int idx = scrollOffset + i;
            if (idx >= (int)libraries.size()) break;
            bool sel = (idx == selectedIndex);
            std::string label = libraries[idx].title + " (" + libraries[idx].type + ")";
            drawButton(15, y, 290, 32, label, sel);
            y += 38;
        }
        if (libraries.empty()) {
            drawText(20, 80, 0.5f, COLOR_GRAY, "Connecting & loading sections...");
        }
    } else if (state == AppState::ITEM_LIST) {
        std::string headerTitle = listTitle.empty() ? "MEDIA ITEMS" : listTitle;
        bool canDLAll = (listTitle != "Downloads" && !items.empty() && (items[0].type == MediaType::TRACK || items[0].type == MediaType::EPISODE));
        if (canDLAll && !dlBadge.empty()) {
            if (headerTitle.length() > 14) headerTitle = headerTitle.substr(0, 12) + "..";
        } else {
            if (headerTitle.length() > 22) headerTitle = headerTitle.substr(0, 19) + "...";
        }
        drawHeader(headerTitle, 320);
        if (canDLAll) {
            drawButton(218, 2, 96, 20, "DL All (Y)", false);
            if (!dlBadge.empty()) {
                drawText(120, 4, 0.42f, C2D_Color32(0, 0, 0, 255), dlBadge);
            }
        } else if (!dlBadge.empty()) {
            drawText(220, 4, 0.45f, C2D_Color32(0, 0, 0, 255), dlBadge);
        }
        float y = 32;
        int maxVisible = hasNowPlaying ? 4 : 5;
        for (int i = 0; i < maxVisible; i++) {
            int itemIdx = scrollOffset + i;
            if (itemIdx >= (int)items.size()) break;
            bool sel = (itemIdx == selectedIndex);
            std::string label = items[itemIdx].title;
            if (items[itemIdx].type == MediaType::TRACK && items[itemIdx].index > 0) {
                label = std::to_string(items[itemIdx].index) + ". " + items[itemIdx].title;
            } else if (items[itemIdx].type == MediaType::EPISODE && items[itemIdx].index > 0) {
                label = std::to_string(items[itemIdx].index) + ". " + items[itemIdx].title;
            } else if (items[itemIdx].type == MediaType::ARTIST) {
                label = "[Artist] " + items[itemIdx].title;
            } else if (items[itemIdx].type == MediaType::ALBUM) {
                if (items[itemIdx].year > 0) {
                    label = "[Album] " + items[itemIdx].title + " (" + std::to_string(items[itemIdx].year) + ")";
                } else {
                    label = "[Album] " + items[itemIdx].title;
                }
            } else if (items[itemIdx].type == MediaType::SHOW) {
                label = "[Show] " + items[itemIdx].title;
            } else if (items[itemIdx].type == MediaType::SEASON) {
                label = "[Season] " + items[itemIdx].title;
            } else if (items[itemIdx].type == MediaType::ARTIST) {
                label = "[Artist] " + items[itemIdx].title;
            }
            if (items[itemIdx].isOffline && !isMediaContainer(items[itemIdx].type) && listTitle != "Downloads" && listTitle.rfind("Downloaded", 0) != 0 && items[itemIdx].key.rfind("__offline", 0) != 0) {
                label = "[DL] " + label;
            }

            bool isContainer = (items[itemIdx].type == MediaType::ALBUM || items[itemIdx].type == MediaType::SEASON) &&
                               (listTitle != "Downloads") && (listTitle.rfind("Downloaded", 0) != 0) &&
                               (items[itemIdx].key.rfind("__offline", 0) != 0);
            int maxLen = isContainer ? 19 : 27;

            std::string displayLabel = label;
            if (displayLabel.length() > (size_t)maxLen) {
                if (sel) {
                    // Smooth horizontal marquee for the currently focused item
                    static uint64_t s_marqueeStart = 0;
                    static int s_lastMarqueeIdx = -1;
                    uint64_t now = 0;
#ifdef __3DS__
                    now = osGetTime();
#else
                    now = (uint64_t)time(nullptr) * 1000;
#endif
                    if (s_lastMarqueeIdx != itemIdx) {
                        s_lastMarqueeIdx = itemIdx;
                        s_marqueeStart = now;
                    }
                    int overflow = (int)displayLabel.length() - maxLen;
                    int pauseMs = 1200;
                    int speedMs = 180;
                    int cycleMs = pauseMs * 2 + overflow * speedMs;
                    int elapsed = (int)((now - s_marqueeStart) % cycleMs);
                    int offset = 0;
                    if (elapsed > pauseMs) {
                        offset = (elapsed - pauseMs) / speedMs;
                        if (offset > overflow) offset = overflow;
                    }
                    displayLabel = displayLabel.substr(offset, maxLen);
                } else {
                    displayLabel = displayLabel.substr(0, maxLen - 2) + "...";
                }
            }

            if (isContainer) {
                drawButton(10, y, 226, 34, displayLabel, sel);
                drawButton(240, y, 70, 34, "DL (Y)", false);
            } else {
                drawButton(10, y, 300, 34, displayLabel, sel);
            }

            bool hasResume = (items[itemIdx].type != MediaType::TRACK) &&
                             (items[itemIdx].viewOffsetMs > 10000) &&
                             (items[itemIdx].durationMs <= 0 || items[itemIdx].viewOffsetMs < items[itemIdx].durationMs - 15000);
            if (hasResume && items[itemIdx].durationMs > 0) {
                float pct = (float)items[itemIdx].viewOffsetMs / (float)items[itemIdx].durationMs;
                if (pct > 1.0f) pct = 1.0f;
                float barW = isContainer ? 222.0f : 296.0f;
                C2D_DrawRectSolid(12, y + 31, 0.5f, barW * pct, 2, COLOR_PLEX_ORANGE);
            }
            y += 38;
        }
        if (items.empty()) {
            if (listTitle == "Downloads") {
                if (!dlBadge.empty()) {
                    drawText(20, 65, 0.52f, COLOR_PLEX_ORANGE, "Downloading in progress...");
                    drawText(20, 92, 0.44f, COLOR_WHITE, dlBadge);
                    drawText(20, 118, 0.38f, COLOR_GRAY, "Items appear here as each finishes.");
                } else {
                    drawText(20, 75, 0.5f, COLOR_WHITE, "No offline downloads found.");
                    drawText(20, 100, 0.42f, COLOR_GRAY, "Download media from your server");
                    drawText(20, 120, 0.42f, COLOR_GRAY, "to enjoy pocket listening & offline playback.");
                }
                drawButton(15, 160, 290, 36, "< Back (B)", false);
            } else {
                drawText(20, 80, 0.5f, COLOR_GRAY, "No items found.");
            }
        }
    } else if (state == AppState::DETAIL_VIEW) {
        drawHeader(isItemDownloaded ? "DETAILS (DOWNLOADED)" : "DETAILS", 320);
        if (!dlBadge.empty()) {
            drawText(230, 4, 0.45f, C2D_Color32(0, 0, 0, 255), dlBadge);
        }
        if (selectedIndex >= 0 && selectedIndex < (int)items.size()) {
            const auto& item = items[selectedIndex];
            std::string itTitle = item.title;
            if (itTitle.length() > 28) itTitle = itTitle.substr(0, 26) + "..";
            drawText(15, 28, 0.55f, COLOR_WHITE, itTitle);

            if (!item.parentTitle.empty()) {
                std::string pt = item.parentTitle;
                if (pt.length() > 32) pt = pt.substr(0, 30) + "..";
                drawText(15, 48, 0.42f, COLOR_PLEX_ORANGE, pt);
            }

            std::string infoLine = "";
            if (item.year > 0) infoLine += std::to_string(item.year) + "  ";
            if (item.durationMs > 0) {
                int mins = (int)(item.durationMs / 60000);
                infoLine += "|  " + std::to_string(mins) + " min  ";
            }
            if (item.isOffline && item.localFileSize > 0) {
                double mb = (double)item.localFileSize / (1024.0 * 1024.0);
                char sBuf[32];
                if (mb >= 1024.0) snprintf(sBuf, sizeof(sBuf), "|  %.1f GB", mb / 1024.0);
                else snprintf(sBuf, sizeof(sBuf), "|  %.0f MB", mb);
                infoLine += sBuf;
            }
            if (!infoLine.empty()) {
                drawText(15, 65, 0.38f, COLOR_GRAY, infoLine);
            }

            std::string sub = item.summary;
            if (sub.empty() && item.type == MediaType::TRACK) sub = "Audio Track";
            if (sub.length() > 110) sub = sub.substr(0, 107) + "...";
            drawText(15, 82, 0.40f, COLOR_WHITE, sub);

            if (isDownloadingCurrent) {
                // Download progress bar
                C2D_DrawRectSolid(15, 122, 0.5f, 290, 8, COLOR_DARK_BG);
                float dlProgW = (dlPercent > 0) ? (290.0f * (float)std::clamp(dlPercent, 0, 100) / 100.0f) : 0.0f;
                if (dlProgW > 0.0f) {
                    C2D_DrawRectSolid(15, 122, 0.55f, dlProgW, 8, COLOR_PLEX_ORANGE);
                }
                std::string progText = dlBadge.empty() ? ("Downloading: " + std::to_string(dlPercent) + "%") : dlBadge;
                drawText(15, 134, 0.42f, COLOR_PLEX_ORANGE, progText);

                drawButton(15, 150, 140, 34, "Cancel DL");
                drawButton(165, 150, 140, 34, "Back");
            } else {
                bool hasResume = (item.type != MediaType::TRACK) &&
                                 (item.viewOffsetMs > 10000) &&
                                 (item.durationMs <= 0 || item.viewOffsetMs < item.durationMs - 15000);

                if (hasResume) {
                    std::string ccDetail = "Captions: OFF";
                    if (subtitlesEnabled) {
                        ccDetail = subtitleName.empty() ? "Captions: ON" : ("Captions: " + subtitleName);
                        if (ccDetail.length() > 16) ccDetail = ccDetail.substr(0, 14) + "..";
                    }
                    drawButton(15, 104, 135, 32, ccDetail, subtitlesEnabled);
                    std::string dlLabel = isItemDownloaded ? "Delete" : (isItemQueued ? "Queued" : "Download");
                    drawButton(155, 104, 78, 32, dlLabel, isItemQueued);
                    drawButton(238, 104, 67, 32, "Back");

                    std::string resumeTime = formatTime((int)(item.viewOffsetMs / 1000));
                    std::string resumeLabel = "Resume (" + resumeTime + ")";
                    drawButton(15, 142, 145, 38, resumeLabel, true);
                    drawButton(165, 142, 140, 38, "Restart (0:00)");
                } else {
                    if (item.type != MediaType::TRACK) {
                        std::string ccDetail = "Captions: OFF";
                        if (subtitlesEnabled) {
                            ccDetail = subtitleName.empty() ? "Captions: ON" : ("Captions: " + subtitleName);
                            if (ccDetail.length() > 22) ccDetail = ccDetail.substr(0, 20) + "..";
                        }
                        drawButton(15, 108, 175, 30, ccDetail, subtitlesEnabled);
                        drawText(198, 114, 0.40f, COLOR_GRAY, "(Sel: Toggle)");
                    }

                    std::string playLabel = isItemDownloaded ? "Play Offline" : ((item.type == MediaType::TRACK) ? "Play Music" : "Play Video");
                    drawButton(15, 144, 130, 36, playLabel, true);

                    std::string dlLabel = isItemDownloaded ? "Delete" : (isItemQueued ? "Queued" : "Download");
                    drawButton(150, 144, 78, 36, dlLabel, isItemQueued);
                    drawButton(233, 144, 72, 36, "Back");
                }
            }
        }
    }

    // Docked Mini-Player Bar at bottom (y = 192..240)
    if (hasNowPlaying) {
        // Mini progress line
        C2D_DrawRectSolid(0, 192, 0.5f, 320, 2, COLOR_DARK_BG);
        float prog = (totalSec > 0) ? std::clamp((float)currentSec / (float)totalSec, 0.0f, 1.0f) : 0.0f;
        C2D_DrawRectSolid(0, 192, 0.5f, prog * 320.0f, 2, COLOR_PLEX_ORANGE);

        // Background
        C2D_DrawRectSolid(0, 194, 0.5f, 320, 46, C2D_Color32(0x16, 0x19, 0x1C, 0xFF));

        // Title & time
        std::string miniTitle = nowPlayingItem ? nowPlayingItem->title : "Media";
        if (miniTitle.length() > 16) miniTitle = miniTitle.substr(0, 14) + "..";
        drawText(8, 198, 0.46f, COLOR_WHITE, miniTitle);
        drawText(8, 218, 0.38f, COLOR_GRAY, formatTime(currentSec) + " / " + formatTime(totalSec));

        // Mini buttons: [ |< ] [ > / || ] [ >| ] [ [^] ]
        drawButton(142, 198, 38, 36, "|<", canPrev);
        drawButton(184, 198, 42, 36, isPaused ? ">" : "||", true);
        drawButton(230, 198, 38, 36, ">|", canNext);
        drawButton(272, 198, 42, 36, "[^]");
    }
#endif
}

void UIRenderer::renderBlankBottomScreen() {
#ifdef __3DS__
    C2D_TargetClear(m_bottomTarget, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(m_bottomTarget);
#endif
}

void UIRenderer::renderConfirmDialog(const std::string& title,
                                     const std::string& prompt,
                                     const std::string& itemTitle,
                                     const std::string& warning,
                                     const std::string& confirmLabel,
                                     const std::string& cancelLabel) {
#ifdef __3DS__
    // Semi-transparent dark overlay covering entire bottom screen
    C2D_DrawRectSolid(0, 0, 0.70f, 320, 240, C2D_Color32(0, 0, 0, 210));

    // Centered modal card (320x240 screen, card 288x196)
    float cardX = 16.0f, cardY = 22.0f, cardW = 288.0f, cardH = 196.0f;
    // Outer border (Plex orange accent)
    C2D_DrawRectSolid(cardX - 2.0f, cardY - 2.0f, 0.72f, cardW + 4.0f, cardH + 4.0f, COLOR_PLEX_ORANGE);
    // Card panel background
    C2D_DrawRectSolid(cardX, cardY, 0.73f, cardW, cardH, COLOR_PANEL_BG);

    // Title banner
    C2D_DrawRectSolid(cardX, cardY, 0.75f, cardW, 28.0f, COLOR_PLEX_ORANGE);
    drawText(cardX + 12.0f, cardY + 5.0f, 0.52f, C2D_Color32(0, 0, 0, 255), title, 0.80f);

    // Prompt message
    drawText(cardX + 14.0f, cardY + 38.0f, 0.44f, COLOR_WHITE, prompt, 0.80f);

    // Item title
    std::string dispItem = itemTitle;
    if (dispItem.length() > 28) {
        dispItem = dispItem.substr(0, 26) + "..";
    }
    drawText(cardX + 14.0f, cardY + 62.0f, 0.54f, COLOR_PLEX_ORANGE, dispItem, 0.80f);

    // Warning text (support 2 lines if needed)
    if (!warning.empty()) {
        std::string warn1 = warning;
        std::string warn2 = "";
        if (warn1.length() > 36) {
            size_t sp = warn1.rfind(' ', 36);
            if (sp != std::string::npos) {
                warn2 = warn1.substr(sp + 1);
                warn1 = warn1.substr(0, sp);
            }
        }
        uint32_t warnColor = C2D_Color32(0xFF, 0x75, 0x75, 0xFF);
        drawText(cardX + 14.0f, cardY + 92.0f, 0.38f, warnColor, warn1, 0.80f);
        if (!warn2.empty()) {
            drawText(cardX + 14.0f, cardY + 110.0f, 0.38f, warnColor, warn2, 0.80f);
        }
    }

    // Action buttons at bottom of card
    float btnY = cardY + cardH - 56.0f;
    float btnW = 120.0f, btnH = 38.0f;

    // Left button: Cancel
    drawButton(cardX + 14.0f, btnY, btnW, btnH, cancelLabel, false, 0.76f);

    // Right button: Confirm / Delete (Red highlight)
    float confX = cardX + cardW - 14.0f - btnW;
    C2D_DrawRectSolid(confX, btnY, 0.76f, btnW, btnH, C2D_Color32(0xD3, 0x2F, 0x2F, 0xFF));
    drawText(confX + (btnW <= 90.0f ? 8.0f : 14.0f), btnY + 9.0f, 0.48f, COLOR_WHITE, confirmLabel, 0.82f);

    // Bottom footnote hint
    drawText(cardX + 18.0f, cardY + cardH - 14.0f, 0.33f, COLOR_GRAY, "Press (A) to confirm, (B) to cancel", 0.80f);
#endif
}

int UIRenderer::handleTouch(AppState state, int touchX, int touchY, int itemCount, bool hasNowPlaying, bool isLoggedIn, bool hasServers, const std::vector<PlexMediaItem>* items, int scrollOffset, const std::string& listTitle) {
    if (state == AppState::SERVER_SELECT) {
        int maxVis = hasNowPlaying ? 2 : 3;
        float y = 28.0f;
        float itemH = 30.0f;
        for (int i = 0; i < std::min(itemCount, maxVis); i++) {
            if (touchX >= 15 && touchX <= 305 && touchY >= y && touchY <= (y + itemH)) {
                return i;
            }
            y += (itemH + 4.0f);
        }

        // Row 1 Action buttons: [+ Add IP] [Sync (Y)] [Remove (X)]
        float btnY1 = hasNowPlaying ? 96.0f : 132.0f;
        float btnH = hasNowPlaying ? 28.0f : 32.0f;
        if (touchY >= btnY1 && touchY <= btnY1 + btnH) {
            if (touchX >= 15 && touchX <= 107) return TOUCH_SERVER_ADD_IP;
            if (touchX >= 114 && touchX <= 206) return TOUCH_SERVER_SYNC;
            if (touchX >= 213 && touchX <= 305) return TOUCH_SERVER_REMOVE;
        }

        // Row 2 Action buttons: [Downloads (L)] and [Account Management]
        float btnY2 = hasNowPlaying ? 128.0f : 168.0f;
        if (touchY >= btnY2 && touchY <= btnY2 + btnH) {
            if (touchX >= 15 && touchX <= 150) return TOUCH_SERVER_DOWNLOADS;
            if (touchX >= 155 && touchX <= 305) return TOUCH_SERVER_ACCOUNT;
        }
        return TOUCH_NONE;
    } else if (state == AppState::PIN_AUTH) {
        if (isLoggedIn) {
            if (touchX >= 15 && touchX <= 305 && touchY >= 75 && touchY <= 109) return TOUCH_AUTH_SYNC;
            if (touchX >= 15 && touchX <= 305 && touchY >= 115 && touchY <= 149) return TOUCH_AUTH_SWITCH_ACCOUNT;
            if (touchX >= 15 && touchX <= 305 && touchY >= 155 && touchY <= 189) return TOUCH_AUTH_LOGOUT;
            if (touchX >= 15 && touchX <= 305 && touchY >= 195 && touchY <= 229) return TOUCH_AUTH_BACK;
        } else {
            // Right button: [ Sign In Email ] (x: 158..305, y: 30..82)
            if (touchX >= 158 && touchX <= 305 && touchY >= 30 && touchY <= 82) return TOUCH_AUTH_SIGN_IN_EMAIL;
            // Row 2: [+ Add Server by Local IP] (x: 15..305, y: 90..124)
            if (touchX >= 15 && touchX <= 305 && touchY >= 90 && touchY <= 124) return TOUCH_AUTH_ADD_LOCAL_IP;
            // Row 3: [Refresh Link] (x: 15..150) and [Downloads (L)] (x: 155..305)
            if (touchY >= 130 && touchY <= 164) {
                if (touchX >= 15 && touchX <= 150) return TOUCH_AUTH_REFRESH_PIN;
                if (touchX >= 155 && touchX <= 305) return TOUCH_AUTH_DOWNLOADS;
            }
            // Row 4: If servers already exist, show [Back to Servers] (x: 15..305, y: 170..204)
            if (hasServers && touchX >= 15 && touchX <= 305 && touchY >= 170 && touchY <= 204) return TOUCH_AUTH_BACK;
        }
        return TOUCH_NONE;
    } else if (state == AppState::LIBRARY_LIST) {
        int maxVis = hasNowPlaying ? 4 : 5;
        float y = 35;
        for (int i = 0; i < std::min(itemCount, maxVis); i++) {
            if (touchX >= 15 && touchX <= 305 && touchY >= y && touchY <= (y + 32)) {
                return i;
            }
            y += 38;
        }
    } else if (state == AppState::ITEM_LIST) {
        if (itemCount == 0) {
            if (touchX >= 15 && touchX <= 305 && touchY >= 155 && touchY <= 200) {
                return TOUCH_ITEM_BACK;
            }
            return TOUCH_NONE;
        }

        // Header button: [DL All (Y)] (x: 215..318, y: 0..26)
        if (touchX >= 215 && touchX <= 318 && touchY >= 0 && touchY <= 26) {
            if (listTitle != "Downloads" && items && !items->empty() && ((*items)[0].type == MediaType::TRACK || (*items)[0].type == MediaType::EPISODE)) {
                return TOUCH_ITEM_DOWNLOAD_ALL;
            }
        }

        int maxVis = hasNowPlaying ? 4 : 5;
        float y = 32;
        for (int i = 0; i < std::min(itemCount, maxVis); i++) {
            int itemIdx = scrollOffset + i;
            bool isContainer = false;
            if (items && itemIdx < (int)items->size() && listTitle != "Downloads" && listTitle.rfind("Downloaded", 0) != 0) {
                if ((*items)[itemIdx].key.rfind("__offline", 0) != 0) {
                    isContainer = ((*items)[itemIdx].type == MediaType::ALBUM || (*items)[itemIdx].type == MediaType::SEASON);
                }
            }

            if (isContainer) {
                if (touchX >= 10 && touchX <= 236 && touchY >= y && touchY <= (y + 34)) {
                    return i;
                }
                if (touchX >= 238 && touchX <= 315 && touchY >= y && touchY <= (y + 34)) {
                    return TOUCH_ITEM_CONTAINER_DL_BASE + i;
                }
            } else {
                if (touchX >= 10 && touchX <= 310 && touchY >= y && touchY <= (y + 34)) {
                    return i;
                }
            }
            y += 38;
        }
    }
    return TOUCH_NONE;
}
