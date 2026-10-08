#include "audio_player.hpp"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <mpg123.h>
#include <curl/curl.h>
#include <cstring>
#include <algorithm>
#include <unistd.h>

AudioPlayer::AudioPlayer() {}

AudioPlayer::~AudioPlayer() {
    exit();
}

bool AudioPlayer::init() {
    if (m_initialized) return true;
#ifdef __3DS__
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(m_channel);
    ndspChnSetInterp(m_channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(m_channel, 44100.0f);
    ndspChnSetFormat(m_channel, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(m_channel, mix);
#endif
    m_initialized = true;
    return true;
}

void AudioPlayer::clearChunks() {
    std::lock_guard<std::mutex> lock(m_chunkMutex);
    for (size_t i = 0; i < MAX_CHUNKS; i++) {
        if (m_chunks[i]) {
            free(m_chunks[i]);
            m_chunks[i] = nullptr;
        }
    }
    m_chunkCount = 0;
    m_readChunkIdx = 0;
    m_readChunkOffset = 0;
    m_totalDownloadedBytes = 0;
}

void AudioPlayer::exit() {
    if (!m_initialized) return;
    stop();
    clearChunks();
    m_initialized = false;
}

int AudioPlayer::readStream(uint8_t* buf, int maxBytes) {
    if (m_stopRequested.load() || g_appExiting.load()) return -1;

    while (true) {
        if (m_stopRequested.load() || g_appExiting.load()) return -1;

        if (m_isPaused.load() || g_isSuspended.load()) {
            m_decodePaused = true;
            while ((m_isPaused.load() || g_isSuspended.load()) && !m_stopRequested.load() && !g_appExiting.load()) {
#ifdef __3DS__
                svcSleepThread(20000000); // 20ms
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
            }
            m_decodePaused = false;
            if (m_stopRequested.load() || g_appExiting.load()) return -1;
        }

        {
            std::lock_guard<std::mutex> lock(m_chunkMutex);
            size_t count = m_chunkCount.load();
            if (m_readChunkIdx < count) {
                AudioChunk* curChunk = m_chunks[m_readChunkIdx];
                if (curChunk) {
                    size_t chunkSize = curChunk->size;
                    if (m_readChunkOffset < chunkSize) {
                        size_t avail = chunkSize - m_readChunkOffset;
                        size_t toRead = std::min((size_t)maxBytes, avail);
                        memcpy(buf, curChunk->data + m_readChunkOffset, toRead);
                        m_readChunkOffset += toRead;
                        if (m_readChunkOffset >= AudioChunk::CHUNK_SIZE) {
                            m_readChunkIdx++;
                            m_readChunkOffset = 0;
                        }
                        return (int)toRead;
                    } else if (m_readChunkOffset >= AudioChunk::CHUNK_SIZE) {
                        m_readChunkIdx++;
                        m_readChunkOffset = 0;
                        continue;
                    }
                }
            }
        }

        if (m_downloadFinished.load()) {
            std::lock_guard<std::mutex> lock(m_chunkMutex);
            size_t finalCount = m_chunkCount.load();
            if (m_readChunkIdx >= finalCount ||
                (m_readChunkIdx == finalCount - 1 && m_chunks[m_readChunkIdx] && m_readChunkOffset >= m_chunks[m_readChunkIdx]->size)) {
                return 0; // True EOF! All downloaded bytes have been consumed.
            }
        }

#ifdef __3DS__
        svcSleepThread(5000000); // 5ms wait for download thread
#endif
    }
}

#ifdef __3DS__
void AudioPlayer::downloadThreadEntry(void* arg) {
    AudioPlayer* self = static_cast<AudioPlayer*>(arg);
    if (self) self->downloadLoop();
}

void AudioPlayer::decodeThreadEntry(void* arg) {
    AudioPlayer* self = static_cast<AudioPlayer*>(arg);
    if (self) self->decodeLoop();
}

void AudioPlayer::downloadLoop() {
    bool isLocal = (m_currentUrl.rfind("http://", 0) != 0 && m_currentUrl.rfind("https://", 0) != 0);

    if (isLocal) {
        FILE* f = fopen(m_currentUrl.c_str(), "rb");
        if (f) {
            while (!m_stopRequested.load() && !g_appExiting.load()) {
                size_t count = m_chunkCount.load();
                if (count >= MAX_CHUNKS) break;

                AudioChunk* chunk = (AudioChunk*)malloc(sizeof(AudioChunk));
                if (!chunk) break;

                size_t n = fread(chunk->data, 1, AudioChunk::CHUNK_SIZE, f);
                if (n == 0) {
                    free(chunk);
                    break;
                }

                chunk->size = n;
                {
                    std::lock_guard<std::mutex> lock(m_chunkMutex);
                    m_chunks[count] = chunk;
                    m_chunkCount = count + 1;
                }
                m_totalDownloadedBytes += n;
            }
            fclose(f);
        }
        m_downloadFinished = true;
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        m_downloadFinished = true;
        return;
    }

    struct WriteContext {
        AudioPlayer* player;
    } ctx = { this };

    auto writeCb = [](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        size_t totalBytes = size * nmemb;
        WriteContext* wc = (WriteContext*)userdata;
        AudioPlayer* p = wc->player;

        if (p->m_stopRequested.load() || g_appExiting.load()) return 0;

        const uint8_t* src = (const uint8_t*)ptr;
        size_t remaining = totalBytes;

        while (remaining > 0 && !p->m_stopRequested.load() && !g_appExiting.load()) {
#ifdef __3DS__
            if (p->m_isPaused.load() || g_isSuspended.load()) {
                svcSleepThread(50000000); // 50ms wait while paused/suspended
                continue;
            }
#endif
            AudioChunk* curChunk = nullptr;
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(p->m_chunkMutex);
                count = p->m_chunkCount.load();
                if (count > 0 && p->m_chunks[count - 1] && p->m_chunks[count - 1]->size < AudioChunk::CHUNK_SIZE) {
                    curChunk = p->m_chunks[count - 1];
                }
            }

            if (!curChunk) {
                if (count >= MAX_CHUNKS) {
                    // Buffer reached 32 MB cap (~22 minutes of audio). Wait for decoder before fetching more.
#ifdef __3DS__
                    svcSleepThread(20000000); // 20ms
#endif
                    continue;
                }
                curChunk = (AudioChunk*)malloc(sizeof(AudioChunk));
                if (!curChunk) {
                    // Memory low: sleep and retry
#ifdef __3DS__
                    svcSleepThread(50000000);
#endif
                    continue;
                }
                curChunk->size = 0;
                {
                    std::lock_guard<std::mutex> lock(p->m_chunkMutex);
                    p->m_chunks[count] = curChunk;
                    p->m_chunkCount = count + 1;
                }
            }

            size_t spaceInChunk = AudioChunk::CHUNK_SIZE - curChunk->size;
            size_t toCopy = std::min(remaining, spaceInChunk);
            memcpy(curChunk->data + curChunk->size, src, toCopy);
            curChunk->size += toCopy;
            p->m_totalDownloadedBytes += toCopy;
            src += toCopy;
            remaining -= toCopy;
        }
        return totalBytes;
    };

    auto xferInfoCb = [](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
        (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
        AudioPlayer* p = (AudioPlayer*)clientp;
        if (p->m_stopRequested.load() || g_appExiting.load()) return 1;
        return 0;
    };

    curl_easy_setopt(curl, CURLOPT_URL, m_currentUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (curl_write_callback)+writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, (curl_xferinfo_callback)+xferInfoCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 32768L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    if (access("/etc/ssl/certs/cacert.pem", R_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/cacert.pem");
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Plex3DS/1.0");
    headers = curl_slist_append(headers, "Accept: */*");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    m_downloadFinished = true;
}

void AudioPlayer::decodeLoop() {
    mpg123_init();
    int err = 0;
    mpg123_handle* mh = mpg123_new(NULL, &err);
    if (!mh) {
        m_isPlaying = false;
        return;
    }

    if (mpg123_open_feed(mh) != MPG123_OK) {
        mpg123_delete(mh);
        m_isPlaying = false;
        return;
    }

    const size_t NUM_BUFFERS = 4;
    const size_t SAMPLES_PER_BUF = 8192; // ~185ms per buffer @ 44.1kHz (total ~740ms queue depth)
    const size_t BUF_BYTES = SAMPLES_PER_BUF * sizeof(int16_t) * 2;
    int16_t* audioBuf = (int16_t*)linearAlloc(BUF_BYTES * NUM_BUFFERS);
    if (!audioBuf) {
        mpg123_close(mh);
        mpg123_delete(mh);
        m_isPlaying = false;
        return;
    }

    ndspWaveBuf waveBuf[NUM_BUFFERS];
    memset(waveBuf, 0, sizeof(waveBuf));
    for (size_t i = 0; i < NUM_BUFFERS; i++) {
        waveBuf[i].data_pcm16 = audioBuf + (i * SAMPLES_PER_BUF * 2);
        waveBuf[i].status = NDSP_WBUF_DONE;
    }

    int currentBuf = 0;
    bool formatSet = false;
    uint64_t samplesPlayed = 0;
    int channels = 2;
    long curRate = 44100;
    uint8_t feedChunk[16384];

    // Wait until we have at least 64 KB pre-buffered or download finishes
    while (m_totalDownloadedBytes.load() < 64 * 1024 && !m_downloadFinished.load() && !m_stopRequested.load() && !g_appExiting.load()) {
        if (m_isPaused.load() || g_isSuspended.load()) {
            m_decodePaused = true;
            while ((m_isPaused.load() || g_isSuspended.load()) && !m_stopRequested.load() && !g_appExiting.load()) {
                svcSleepThread(20000000); // 20ms
            }
            m_decodePaused = false;
        }
        svcSleepThread(10000000); // 10ms
    }

    while (!m_stopRequested.load() && !g_appExiting.load()) {
        if (m_isPaused.load() || g_isSuspended.load()) {
            m_decodePaused = true;
            while ((m_isPaused.load() || g_isSuspended.load()) && !m_stopRequested.load() && !g_appExiting.load()) {
#ifdef __3DS__
                svcSleepThread(20000000); // 20ms
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
            }
            m_decodePaused = false;
            if (m_stopRequested.load() || g_appExiting.load()) break;
        }

#ifdef __3DS__
        if (m_wasSuspended.exchange(false)) {
            ndspChnReset(m_channel);
            ndspChnSetInterp(m_channel, NDSP_INTERP_LINEAR);
            ndspChnSetRate(m_channel, (float)curRate);
            ndspChnSetFormat(m_channel, (channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
            float mix[12];
            memset(mix, 0, sizeof(mix));
            mix[0] = 1.0f;
            mix[1] = 1.0f;
            ndspChnSetMix(m_channel, mix);
            for (size_t i = 0; i < NUM_BUFFERS; i++) {
                waveBuf[i].status = NDSP_WBUF_DONE;
            }
            currentBuf = 0;
        }
#endif

        // Ensure next waveBuf is available
        if (waveBuf[currentBuf].status != NDSP_WBUF_DONE) {
            svcSleepThread(2000000); // 2ms
            continue;
        }

        size_t bytesDone = 0;
        int readRet = mpg123_read(mh, (unsigned char*)waveBuf[currentBuf].data_pcm16, BUF_BYTES, &bytesDone);

        if (readRet == MPG123_NEW_FORMAT || !formatSet) {
            long rate = 44100;
            int ch = 2, enc = 0;
            if (mpg123_getformat(mh, &rate, &ch, &enc) == MPG123_OK) {
                curRate = rate > 0 ? rate : 44100;
                channels = ch;
                m_sampleRate = curRate;
                m_channels = channels;
                ndspChnSetRate(m_channel, (float)curRate);
                ndspChnSetFormat(m_channel, (channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
                formatSet = true;
            }
        }

        if (bytesDone > 0) {
            size_t sampleFrameSize = sizeof(int16_t) * (channels == 1 ? 1 : 2);
            size_t numSamples = bytesDone / sampleFrameSize;
            samplesPlayed += numSamples;
            m_currentSec = m_initialSec.load() + (int)(samplesPlayed / curRate);

            waveBuf[currentBuf].nsamples = numSamples;
            DSP_FlushDataCache(waveBuf[currentBuf].data_pcm16, bytesDone);
            ndspChnWaveBufAdd(m_channel, &waveBuf[currentBuf]);
            currentBuf = (currentBuf + 1) % NUM_BUFFERS;
        }

        // If mpg123 needs more data, feed it from the ring buffer
        if (readRet == MPG123_NEED_MORE || bytesDone == 0) {
            int n = readStream(feedChunk, sizeof(feedChunk));
            if (n > 0) {
                mpg123_feed(mh, feedChunk, (size_t)n);
            } else if (n == 0) {
                // EOF: ring buffer is empty and download has completed!
                // Drain any final partial frame from mpg123
                size_t finalBytes = 0;
                mpg123_read(mh, (unsigned char*)waveBuf[currentBuf].data_pcm16, BUF_BYTES, &finalBytes);
                if (finalBytes > 0) {
                    size_t sampleFrameSize = sizeof(int16_t) * (channels == 1 ? 1 : 2);
                    size_t numSamples = finalBytes / sampleFrameSize;
                    samplesPlayed += numSamples;
                    waveBuf[currentBuf].nsamples = numSamples;
                    DSP_FlushDataCache(waveBuf[currentBuf].data_pcm16, finalBytes);
                    ndspChnWaveBufAdd(m_channel, &waveBuf[currentBuf]);
                }
                break; // Song stream completely finished!
            } else {
                // n < 0: stop requested
                break;
            }
        }
    }

    // Wait for remaining queued audio buffers to complete playing on hardware
    while (!m_stopRequested.load() && !g_appExiting.load()) {
        if (m_isPaused.load() || g_isSuspended.load()) {
            break;
        }
        bool anyBusy = false;
        for (size_t i = 0; i < NUM_BUFFERS; i++) {
            if (waveBuf[i].status != NDSP_WBUF_DONE) {
                anyBusy = true;
                break;
            }
        }
        if (!anyBusy) break;
        svcSleepThread(10000000); // 10ms
    }

    ndspChnReset(m_channel);
    linearFree(audioBuf);
    mpg123_close(mh);
    mpg123_delete(mh);
    m_isPlaying = false;
}
#endif

bool AudioPlayer::play(const std::string& audioUrl, int totalSec) {
    stop();
    m_currentUrl = audioUrl;
    m_totalSec = totalSec;
    m_initialSec = 0;
    m_currentSec = 0;
    m_stopRequested = false;
    m_isPaused = false;
    m_wasSuspended = false;
    m_decodePaused = false;
    m_isPlaying = true;
    m_readChunkIdx = 0;
    m_readChunkOffset = 0;
    m_chunkCount = 0;
    m_totalDownloadedBytes = 0;
    m_downloadFinished = false;

#ifdef __3DS__
    m_downloadThread = threadCreate(downloadThreadEntry, this, 128 * 1024, 0x32, -1, false);
    if (!m_downloadThread) {
        m_downloadThread = threadCreate(downloadThreadEntry, this, 128 * 1024, 0x32, -2, false);
    }

    m_decodeThread = threadCreate(decodeThreadEntry, this, 128 * 1024, 0x2A, -1, false);
    if (!m_decodeThread) {
        m_decodeThread = threadCreate(decodeThreadEntry, this, 128 * 1024, 0x2A, -2, false);
    }

    if (!m_downloadThread || !m_decodeThread) {
        stop();
        return false;
    }
#endif
    return true;
}

void AudioPlayer::pause() {
    m_isPaused = true;
#ifdef __3DS__
    if (m_initialized) {
        ndspChnSetPaused(m_channel, true);
    }
#endif
}

void AudioPlayer::resume() {
#ifdef __3DS__
    if (m_initialized) {
        if (m_wasSuspended.load()) {
            resumeFromSuspend();
            return;
        }
        ndspChnSetPaused(m_channel, false);
    }
#endif
    m_isPaused = false;
}

void AudioPlayer::suspend() {
    if (!m_initialized) return;
    m_wasSuspended = true;
    m_isPaused = true;
#ifdef __3DS__
    if (m_decodeThread && m_isPlaying.load()) {
        for (int i = 0; i < 50 && !m_decodePaused.load(); i++) {
            svcSleepThread(5000000ULL); // 5ms
        }
    }
    ndspChnReset(m_channel);
#endif
}

void AudioPlayer::resumeFromSuspend() {
#ifdef __3DS__
    if (m_initialized) {
        ndspChnReset(m_channel);
        ndspChnSetInterp(m_channel, NDSP_INTERP_LINEAR);
        ndspChnSetRate(m_channel, (float)m_sampleRate.load());
        ndspChnSetFormat(m_channel, (m_channels.load() == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        float mix[12];
        memset(mix, 0, sizeof(mix));
        mix[0] = 1.0f;
        mix[1] = 1.0f;
        ndspChnSetMix(m_channel, mix);
    }
#endif
    m_wasSuspended = false;
    m_isPaused = false;
}

void AudioPlayer::stop() {
    m_stopRequested = true;
    m_isPaused = false;
    m_wasSuspended = false;

#ifdef __3DS__
    if (m_decodeThread) {
        threadJoin(m_decodeThread, U64_MAX);
        threadFree(m_decodeThread);
        m_decodeThread = nullptr;
    }
    if (m_downloadThread) {
        threadJoin(m_downloadThread, U64_MAX);
        threadFree(m_downloadThread);
        m_downloadThread = nullptr;
    }
    ndspChnReset(m_channel);
#endif
    clearChunks();
    m_isPlaying = false;
}

void AudioPlayer::update() {
    // Threads manage download and decode autonomously
}

void AudioPlayer::seekTo(int targetSeconds) {
    if (!m_isPlaying.load() || m_currentUrl.empty()) return;
    if (targetSeconds < 0) targetSeconds = 0;
    int total = m_totalSec.load();
    if (total > 0 && targetSeconds > total) targetSeconds = total;

    bool isLocal = (m_currentUrl.rfind("http://", 0) != 0 && m_currentUrl.rfind("https://", 0) != 0);
    std::string url = m_currentUrl;
    if (!isLocal) {
        size_t offPos = url.find("&offset=");
        if (offPos != std::string::npos) {
            size_t nextAmp = url.find('&', offPos + 8);
            if (nextAmp != std::string::npos) {
                url.replace(offPos + 8, nextAmp - (offPos + 8), std::to_string(targetSeconds));
            } else {
                url.replace(offPos + 8, std::string::npos, std::to_string(targetSeconds));
            }
        } else {
            url += "&offset=" + std::to_string(targetSeconds);
        }
    }

    play(url, total);
    m_initialSec = targetSeconds;
    m_currentSec = targetSeconds;
}

void AudioPlayer::seek(int deltaSeconds) {
    seekTo(m_currentSec.load() + deltaSeconds);
}
