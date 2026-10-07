#pragma once

#include "types.hpp"
#include <string>
#include <cstdint>
#include <atomic>

#ifdef __3DS__
#include <3ds.h>
#endif

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    bool init();
    void exit();

    bool play(const std::string& audioUrl, int totalSec = 0);
    void pause();
    void resume();
    void stop();
    void seek(int deltaSeconds);
    void seekTo(int targetSeconds);

    bool isPlaying() const { return m_isPlaying.load(); }
    bool isPaused() const { return m_isPaused.load(); }
    int getCurrentSeconds() const { return m_currentSec.load(); }
    int getTotalSeconds() const { return m_totalSec.load(); }
    void setTotalSeconds(int totalSec) { m_totalSec = totalSec; }

    void update();

private:
    std::string m_currentUrl;
    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isPaused{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<int> m_currentSec{0};
    std::atomic<int> m_initialSec{0};
    std::atomic<int> m_totalSec{0};
    bool m_initialized = false;
    int m_channel = 0;
    // Ring buffer for streaming
    uint8_t* m_ringBuf = nullptr;
    size_t m_ringCap = 512 * 1024; // 512 KB (~15-20s cushion)
    size_t m_ringHead = 0;
    size_t m_ringTail = 0;
    std::atomic<size_t> m_ringSize{0};
    std::atomic<bool> m_downloadFinished{false};

    int readStream(uint8_t* buf, int maxBytes);

#ifdef __3DS__
    Thread m_downloadThread = nullptr;
    Thread m_decodeThread = nullptr;
    static void downloadThreadEntry(void* arg);
    static void decodeThreadEntry(void* arg);
    void downloadLoop();
    void decodeLoop();
#endif
};
