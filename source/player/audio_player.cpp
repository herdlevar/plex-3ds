#include "audio_player.hpp"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <mpg123.h>
#include <curl/curl.h>
#include <cstring>
#include <iostream>

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

#ifdef __3DS__
void AudioPlayer::streamThreadEntry(void* arg) {
    AudioPlayer* self = static_cast<AudioPlayer*>(arg);
    if (self) {
        self->streamLoop();
    }
}

void AudioPlayer::streamLoop() {
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

    const size_t SAMPLES_PER_BUF = 4096;
    const size_t BUF_BYTES = SAMPLES_PER_BUF * sizeof(int16_t) * 2;
    int16_t* audioBuf = (int16_t*)linearAlloc(BUF_BYTES * 2);
    if (!audioBuf) {
        mpg123_close(mh);
        mpg123_delete(mh);
        m_isPlaying = false;
        return;
    }

    ndspWaveBuf waveBuf[2];
    memset(waveBuf, 0, sizeof(waveBuf));
    waveBuf[0].data_pcm16 = audioBuf;
    waveBuf[0].status = NDSP_WBUF_DONE;
    waveBuf[1].data_pcm16 = audioBuf + SAMPLES_PER_BUF * 2;
    waveBuf[1].status = NDSP_WBUF_DONE;

    struct StreamContext {
        AudioPlayer* player;
        mpg123_handle* mh;
        ndspWaveBuf* waveBuf;
        int currentBuf;
        bool formatSet;
        size_t samplesPerBuf;
        size_t bufBytes;
        int channel;
        uint64_t samplesPlayed;
    } ctx = { this, mh, waveBuf, 0, false, SAMPLES_PER_BUF, BUF_BYTES, m_channel, 0 };

    auto writeCb = [](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        size_t totalBytes = size * nmemb;
        StreamContext* sc = (StreamContext*)userdata;
        if (sc->player->m_stopRequested.load() || g_appExiting.load()) {
            return 0; // abort
        }

        while (sc->player->m_isPaused.load()) {
            if (sc->player->m_stopRequested.load() || g_appExiting.load()) return 0;
            svcSleepThread(20000000); // 20ms
        }

        int ret = mpg123_feed(sc->mh, (const unsigned char*)ptr, totalBytes);
        if (ret != MPG123_OK && ret != MPG123_NEED_MORE) {
            return totalBytes;
        }

        while (!sc->player->m_stopRequested.load() && !g_appExiting.load()) {
            if (sc->waveBuf[sc->currentBuf].status != NDSP_WBUF_DONE) {
                svcSleepThread(5000000); // 5ms
                continue;
            }

            size_t bytesDone = 0;
            int readRet = mpg123_read(sc->mh, (unsigned char*)sc->waveBuf[sc->currentBuf].data_pcm16, sc->bufBytes, &bytesDone);

            if (!sc->formatSet) {
                long rate = 44100;
                int channels = 2, encoding = 0;
                if (mpg123_getformat(sc->mh, &rate, &channels, &encoding) == MPG123_OK) {
                    ndspChnSetRate(sc->channel, (float)rate);
                    ndspChnSetFormat(sc->channel, (channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
                    sc->formatSet = true;
                }
            }

            if (bytesDone > 0) {
                size_t numSamples = bytesDone / (sizeof(int16_t) * 2);
                sc->samplesPlayed += numSamples;
                long curRate = 44100;
                int ch = 2, enc = 0;
                if (mpg123_getformat(sc->mh, &curRate, &ch, &enc) != MPG123_OK || curRate <= 0) {
                    curRate = 44100;
                }
                sc->player->m_currentSec = sc->player->m_initialSec.load() + (int)(sc->samplesPlayed / curRate);
                sc->waveBuf[sc->currentBuf].nsamples = numSamples;
                DSP_FlushDataCache(sc->waveBuf[sc->currentBuf].data_pcm16, bytesDone);
                ndspChnWaveBufAdd(sc->channel, &sc->waveBuf[sc->currentBuf]);
                sc->currentBuf = 1 - sc->currentBuf;
            }

            if (readRet == MPG123_NEED_MORE || bytesDone == 0) {
                break;
            }
        }

        return totalBytes;
    };

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
            const size_t CHUNK_SIZE = 16384;
            uint8_t readChunk[CHUNK_SIZE];
            while (!m_stopRequested.load() && !g_appExiting.load()) {
                while (m_isPaused.load()) {
                    if (m_stopRequested.load() || g_appExiting.load()) break;
                    svcSleepThread(20000000);
                }
                size_t n = fread(readChunk, 1, CHUNK_SIZE, f);
                if (n == 0) break;
                writeCb(readChunk, 1, n, &ctx);
            }
            fclose(f);
        }
    } else {
        CURL* curl = curl_easy_init();
        if (curl) {
            auto xferInfoCb = [](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
                (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
                AudioPlayer* p = (AudioPlayer*)clientp;
                if (p->m_stopRequested.load() || g_appExiting.load()) {
                    return 1;
                }
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
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "User-Agent: Plex3DS/1.0");
            headers = curl_slist_append(headers, "Accept: */*");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            curl_easy_perform(curl);

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
    }

    while (!m_stopRequested.load() && !g_appExiting.load() && (waveBuf[0].status != NDSP_WBUF_DONE || waveBuf[1].status != NDSP_WBUF_DONE)) {
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

#ifdef __3DS__
    m_thread = threadCreate(streamThreadEntry, this, 64 * 1024, 0x30, -2, false);
    if (!m_thread) {
        m_isPlaying = false;
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
    if (m_thread) {
        threadJoin(m_thread, 1000000000ULL);
        threadFree(m_thread);
        m_thread = nullptr;
    }
    ndspChnReset(m_channel);
#endif
    m_isPlaying = false;
}

void AudioPlayer::update() {
    // Thread manages playback autonomously
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

    // Stop current stream thread
    m_stopRequested = true;
#ifdef __3DS__
    if (m_thread) {
        threadJoin(m_thread, 1000000000ULL);
        threadFree(m_thread);
        m_thread = nullptr;
    }
    ndspChnReset(m_channel);
#endif

    m_currentUrl = url;
    m_totalSec = total;
    m_initialSec = targetSeconds;
    m_currentSec = targetSeconds;
    m_stopRequested = false;
    m_isPaused = false;
    m_isPlaying = true;

#ifdef __3DS__
    m_thread = threadCreate(streamThreadEntry, this, 64 * 1024, 0x30, -2, false);
    if (!m_thread) {
        m_isPlaying = false;
    }
#endif
}

void AudioPlayer::seek(int deltaSeconds) {
    seekTo(m_currentSec.load() + deltaSeconds);
}
