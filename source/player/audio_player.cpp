#include "audio_player.hpp"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <mpg123.h>
#include <curl/curl.h>
#include <cstring>
#include <algorithm>

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

void AudioPlayer::exit() {
    if (!m_initialized) return;
    stop();
    m_initialized = false;
}

int AudioPlayer::readStream(uint8_t* buf, int maxBytes) {
    while (m_ringSize.load() == 0) {
        if (m_stopRequested.load() || g_appExiting.load()) return -1;
        if (m_downloadFinished.load()) return 0; // EOF
#ifdef __3DS__
        svcSleepThread(5000000); // 5ms
#endif
    }

    if (m_stopRequested.load() || g_appExiting.load()) return -1;

    size_t toRead = std::min((size_t)maxBytes, m_ringSize.load());
    size_t firstPart = std::min(toRead, m_ringCap - m_ringTail);
    memcpy(buf, &m_ringBuf[m_ringTail], firstPart);
    if (toRead > firstPart) {
        memcpy(buf + firstPart, &m_ringBuf[0], toRead - firstPart);
    }
    m_ringTail = (m_ringTail + toRead) % m_ringCap;
    m_ringSize -= toRead;
    return (int)toRead;
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
            if (m_initialSec.load() > 0 && m_totalSec.load() > 0) {
                fseek(f, 0, SEEK_END);
                long fLen = ftell(f);
                long seekPos = (long)(((double)m_initialSec.load() / (double)m_totalSec.load()) * fLen);
                fseek(f, seekPos, SEEK_SET);
            }
            uint8_t chunk[16384];
            while (!m_stopRequested.load() && !g_appExiting.load()) {
                size_t freeSpace = m_ringCap - m_ringSize.load();
                if (freeSpace < sizeof(chunk)) {
                    svcSleepThread(10000000); // 10ms wait for decoder
                    continue;
                }
                size_t n = fread(chunk, 1, sizeof(chunk), f);
                if (n == 0) break;

                size_t firstPart = std::min(n, m_ringCap - m_ringHead);
                memcpy(&m_ringBuf[m_ringHead], chunk, firstPart);
                if (n > firstPart) {
                    memcpy(&m_ringBuf[0], chunk + firstPart, n - firstPart);
                }
                m_ringHead = (m_ringHead + n) % m_ringCap;
                m_ringSize += n;
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
            size_t freeSpace = p->m_ringCap - p->m_ringSize.load();
            if (freeSpace < 4096) {
                svcSleepThread(10000000); // 10ms wait for decoder to consume
                continue;
            }
            size_t toWrite = std::min(remaining, freeSpace);
            size_t firstPart = std::min(toWrite, p->m_ringCap - p->m_ringHead);
            memcpy(&p->m_ringBuf[p->m_ringHead], src, firstPart);
            if (toWrite > firstPart) {
                memcpy(&p->m_ringBuf[0], src + firstPart, toWrite - firstPart);
            }
            p->m_ringHead = (p->m_ringHead + toWrite) % p->m_ringCap;
            p->m_ringSize += toWrite;
            src += toWrite;
            remaining -= toWrite;
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
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

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
    while (m_ringSize.load() < 64 * 1024 && !m_downloadFinished.load() && !m_stopRequested.load() && !g_appExiting.load()) {
        svcSleepThread(10000000); // 10ms
    }

    while (!m_stopRequested.load() && !g_appExiting.load()) {
        while (m_isPaused.load()) {
            if (m_stopRequested.load() || g_appExiting.load()) break;
            svcSleepThread(20000000); // 20ms
        }
        if (m_stopRequested.load() || g_appExiting.load()) break;

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
    m_isPlaying = true;
    m_ringHead = 0;
    m_ringTail = 0;
    m_ringSize = 0;
    m_downloadFinished = false;

#ifdef __3DS__
    m_ringBuf = (uint8_t*)linearAlloc(m_ringCap);
    if (!m_ringBuf) {
        m_isPlaying = false;
        return false;
    }

    m_downloadThread = threadCreate(downloadThreadEntry, this, 64 * 1024, 0x31, -2, false);
    if (!m_downloadThread) {
        m_downloadThread = threadCreate(downloadThreadEntry, this, 64 * 1024, 0x31, -1, false);
    }

    m_decodeThread = threadCreate(decodeThreadEntry, this, 64 * 1024, 0x2A, -1, false);
    if (!m_decodeThread) {
        m_decodeThread = threadCreate(decodeThreadEntry, this, 64 * 1024, 0x2A, -2, false);
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
    ndspChnSetPaused(m_channel, true);
#endif
}

void AudioPlayer::resume() {
    m_isPaused = false;
#ifdef __3DS__
    ndspChnSetPaused(m_channel, false);
#endif
}

void AudioPlayer::stop() {
    m_stopRequested = true;
    m_isPaused = false;

#ifdef __3DS__
    if (m_decodeThread) {
        threadJoin(m_decodeThread, 1000000000ULL);
        threadFree(m_decodeThread);
        m_decodeThread = nullptr;
    }
    if (m_downloadThread) {
        threadJoin(m_downloadThread, 1000000000ULL);
        threadFree(m_downloadThread);
        m_downloadThread = nullptr;
    }
    ndspChnReset(m_channel);
    if (m_ringBuf) {
        linearFree(m_ringBuf);
        m_ringBuf = nullptr;
    }
#endif
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
