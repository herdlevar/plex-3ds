#pragma once

#include "types.hpp"
#include <string>
#include <cstdint>
#include <atomic>
#include <vector>

#ifdef __3DS__
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <tex3ds.h>
#endif

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    bool init();
    void exit();

    bool start(const std::string& streamUrl, int64_t durationMs = 0, int64_t initialOffsetMs = 0);
    void update();
    void pause();
    void resume();
    void stop();
    void seek(int deltaSeconds);
    void seekTo(int targetSeconds);

    bool isPlaying() const { return m_isPlaying.load(); }
    bool isPaused() const { return m_isPaused.load(); }
    bool hasFrame() const { return m_hasFrame.load(); }
    int64_t getCurrentTimeMs() const { return m_currentTimeMs.load(); }
    int64_t getDurationMs() const { return m_durationMs.load(); }
    int getCurrentSeconds() const { return (int)(m_currentTimeMs.load() / 1000); }
    int getTotalSeconds() const { return (int)(m_durationMs.load() / 1000); }
    void setDurationMs(int64_t durMs) { m_durationMs = durMs; }
    void setClientIdentifier(const std::string& id) { m_clientIdentifier = id; }
    int64_t getBytesReceived() const { return m_bytesReceived.load(); }
    std::string getStatusMessage() const { return m_statusMsg; }
    bool isLocalFile() const { return m_isLocalFile.load(); }
    int getElapsedConnectSec() const {
        if (m_connectStartTick == 0) return 0;
#ifdef __3DS__
        uint64_t now = osGetTime();
#else
        uint64_t now = (uint64_t)time(nullptr) * 1000;
#endif
        return (now >= m_connectStartTick) ? (int)((now - m_connectStartTick) / 1000) : 0;
    }

#ifdef __3DS__
    C2D_Image getImage() const { return m_videoImage; }
#endif

    // Reader for AVIOContext
    int readStream(uint8_t* buf, int bufSize);

private:
    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isPaused{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_hasFrame{false};
    std::atomic<int64_t> m_currentTimeMs{0};
    std::atomic<int64_t> m_durationMs{0};
    std::atomic<int64_t> m_initialOffsetMs{0};
    std::atomic<int64_t> m_bytesReceived{0};
    std::atomic<bool> m_isLocalFile{false};
    std::string m_clientIdentifier;
    std::string m_currentUrl;
    std::string m_statusMsg{"Idle"};
    std::atomic<uint64_t> m_connectStartTick{0};
    bool m_initialized = false;

#ifdef __3DS__
    C3D_Tex m_videoTex;
    Tex3DS_SubTexture m_subTex;
    C2D_Image m_videoImage;
    bool m_texInitialized = false;
    int m_audioChannel = 1;

    Thread m_downloadThread = nullptr;
    Thread m_decodeThread = nullptr;

    static void downloadThreadEntry(void* arg);
    static void decodeThreadEntry(void* arg);

    void downloadLoop();
    void decodeLoop();
#endif

    // Ring Buffer
    uint8_t* m_ringBuf = nullptr;
    size_t m_ringCap = 2 * 1024 * 1024; // 2 MB
    size_t m_ringHead = 0;
    size_t m_ringTail = 0;
    std::atomic<size_t> m_ringSize{0};
    std::atomic<bool> m_downloadFinished{false};
};
