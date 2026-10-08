#pragma once

#include "types.hpp"
#include <string>
#include <vector>

#ifdef __3DS__
#include <3ds.h>
#include <citro2d.h>
#endif

class AudioPlayer;
class VideoPlayer;

enum TouchAction {
    TOUCH_NONE = -1,
    // 0..999 = list item index clicked
    TOUCH_SERVER_ADD_IP = 1001,
    TOUCH_SERVER_SYNC = 1002,
    TOUCH_SERVER_REMOVE = 1003,
    TOUCH_SERVER_ACCOUNT = 1004,
    TOUCH_SERVER_DOWNLOADS = 1005,

    TOUCH_AUTH_SYNC = 2001,
    TOUCH_AUTH_SWITCH_ACCOUNT = 2002,
    TOUCH_AUTH_LOGOUT = 2003,
    TOUCH_AUTH_BACK = 2004,

    TOUCH_AUTH_SIGN_IN_EMAIL = 2101,
    TOUCH_AUTH_ADD_LOCAL_IP = 2102,
    TOUCH_AUTH_REFRESH_PIN = 2103,
    TOUCH_AUTH_DOWNLOADS = 2104,

    TOUCH_ITEM_BACK = 3001,
    TOUCH_ITEM_DOWNLOAD_ALL = 3002,
    TOUCH_ITEM_CONTAINER_DL_BASE = 4000
};

class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    bool init();
    void exit();

    void beginFrame();
    void endFrame();

    // Screen Renderers
    void renderTopScreen(AppState state, 
                         const PlexMediaItem* nowPlayingItem,
                         bool isMediaPlaying,
                         const PlexMediaItem* selectedItem, 
                         const std::string& statusMessage, 
                         const VideoPlayer* videoPlayer = nullptr,
                         const AudioPlayer* audioPlayer = nullptr,
                         const std::string& username = "",
                         bool isLoggedIn = false,
                         const std::string& pinCode = "");

    void drawQRCode(float startX, float startY, const std::string& text, int scale = 4);

    void renderBottomScreen(AppState state, 
                            const std::string& pinCode,
                            const std::vector<PlexServer>& servers,
                            const std::vector<PlexLibrary>& libraries,
                            const std::vector<PlexMediaItem>& items,
                            int selectedIndex,
                            int scrollOffset,
                            const std::string& listTitle = "",
                            bool isPaused = false,
                            bool hasNowPlaying = false,
                            const PlexMediaItem* nowPlayingItem = nullptr,
                            bool controlsExpanded = false,
                            int currentSec = 0,
                            int totalSec = 0,
                            bool isScrubbing = false,
                            int scrubSec = 0,
                            bool canPrev = false,
                            bool canNext = false,
                            bool isItemDownloaded = false,
                            bool isDownloadingCurrent = false,
                            int dlPercent = 0,
                            const std::string& dlBadge = "",
                            bool subtitlesEnabled = false,
                            const std::string& subtitleName = "",
                            const std::string& username = "",
                            bool isLoggedIn = false,
                            bool isItemQueued = false);

    void renderBlankBottomScreen();

    void renderConfirmDialog(const std::string& title,
                             const std::string& prompt,
                             const std::string& itemTitle,
                             const std::string& warning = "",
                             const std::string& confirmLabel = "Delete (A)",
                             const std::string& cancelLabel = "Cancel (B)");

    // Touch Interaction
    int handleTouch(AppState state, int touchX, int touchY, int itemCount, bool hasNowPlaying = false, bool isLoggedIn = false, bool hasServers = false, const std::vector<PlexMediaItem>* items = nullptr, int scrollOffset = 0, const std::string& listTitle = "");

private:
    bool m_initialized = false;
#ifdef __3DS__
    C3D_RenderTarget* m_topTarget = nullptr;
    C3D_RenderTarget* m_bottomTarget = nullptr;
    C2D_TextBuf m_staticTextBuf = nullptr;
    C2D_TextBuf m_dynamicTextBuf = nullptr;
#endif

    void drawHeader(const std::string& title, float screenWidth);
    void drawButton(float x, float y, float w, float h, const std::string& label, bool highlighted = false, float depth = 0.5f);
    void drawText(float x, float y, float size, uint32_t color, const std::string& text, float depth = 0.6f);
};
