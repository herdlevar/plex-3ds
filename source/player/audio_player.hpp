#pragma once

#include "types.hpp"
#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>

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
    void suspend();
    void resumeFromSuspend();
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
    std::atomic<bool> m_decodePaused{false};
    std::atomic<bool> m_wasSuspended{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<int> m_currentSec{0};
    std::atomic<int> m_initialSec{0};
    std::atomic<int> m_totalSec{0};
    std::atomic<long> m_sampleRate{44100};
    std::atomic<int> m_channels{2};
    bool m_initialized = false;
    int m_channel = 0;
    // Chunked buffer for full track in-memory buffering (up to 32 MB)
    struct AudioChunk {
        static constexpr size_t CHUNK_SIZE = 64 * 1024; // 64 KB
        uint8_t data[CHUNK_SIZE];
        size_t size = 0;
    };
    static constexpr size_t MAX_CHUNKS = 512;
    AudioChunk* m_chunks[MAX_CHUNKS] = {nullptr};
    std::atomic<size_t> m_chunkCount{0};
    std::mutex m_chunkMutex;
    size_t m_readChunkIdx = 0;
    size_t m_readChunkOffset = 0;
    std::atomic<size_t> m_totalDownloadedBytes{0};
    std::atomic<bool> m_downloadFinished{false};

    void clearChunks();
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
