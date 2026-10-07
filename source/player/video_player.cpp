#include "video_player.hpp"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <curl/curl.h>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

// 8x8 Morton Order Lookup Table for Nintendo 3DS PICA200 Tiling
static const uint8_t s_mortonTable[8][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
    { 32, 33, 36, 37, 48, 49, 52, 53 },
    { 34, 35, 38, 39, 50, 51, 54, 55 },
    { 40, 41, 44, 45, 56, 57, 60, 61 },
    { 42, 43, 46, 47, 58, 59, 62, 63 }
};

static void tileImageRGB565(const uint16_t* src, uint16_t* dst, int width, int height, int texWidth) {
    int tilesX = width / 8;
    int tilesY = height / 8;
    int texTilesX = texWidth / 8;

    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            uint16_t* tileDst = dst + (ty * texTilesX + tx) * 64;
            const uint16_t* tileSrc = src + (ty * 8 * width) + (tx * 8);

            for (int py = 0; py < 8; py++) {
                const uint16_t* rowSrc = tileSrc + py * width;
                const uint8_t* mortonRow = s_mortonTable[py];
                for (int px = 0; px < 8; px++) {
                    tileDst[mortonRow[px]] = rowSrc[px];
                }
            }
        }
    }
}

static int readPacketCallback(void* opaque, uint8_t* buf, int bufSize) {
    VideoPlayer* self = static_cast<VideoPlayer*>(opaque);
    if (!self) return -1;
    return self->readStream(buf, bufSize);
}

VideoPlayer::VideoPlayer() {}

VideoPlayer::~VideoPlayer() {
    exit();
}

bool VideoPlayer::init() {
    if (m_initialized) return true;
#ifdef __3DS__
    if (!m_texInitialized) {
        if (!C3D_TexInit(&m_videoTex, 512, 256, GPU_RGB565)) {
            return false;
        }
        C3D_TexSetFilter(&m_videoTex, GPU_LINEAR, GPU_LINEAR);

        m_subTex.width = 400;
        m_subTex.height = 240;
        m_subTex.left = 0.0f;
        m_subTex.top = 240.0f / 256.0f;
        m_subTex.right = 400.0f / 512.0f;
        m_subTex.bottom = 0.0f;

        m_videoImage.tex = &m_videoTex;
        m_videoImage.subtex = &m_subTex;

        // Clear texture to black
        memset(m_videoTex.data, 0, m_videoTex.size);
        GSPGPU_FlushDataCache(m_videoTex.data, m_videoTex.size);

        m_texInitialized = true;
    }
    if (!m_ringBuf) {
        m_ringBuf = (uint8_t*)malloc(m_ringCap);
    }
#endif
    m_initialized = true;
    return true;
}

void VideoPlayer::exit() {
    if (!m_initialized) return;
    stop();
#ifdef __3DS__
    if (m_ringBuf) {
        free(m_ringBuf);
        m_ringBuf = nullptr;
    }
    if (m_texInitialized) {
        C3D_TexDelete(&m_videoTex);
        m_texInitialized = false;
    }
#endif
    m_initialized = false;
}

#ifdef __3DS__
void VideoPlayer::downloadThreadEntry(void* arg) {
    VideoPlayer* self = static_cast<VideoPlayer*>(arg);
    if (self) self->downloadLoop();
}

void VideoPlayer::decodeThreadEntry(void* arg) {
    VideoPlayer* self = static_cast<VideoPlayer*>(arg);
    if (self) self->decodeLoop();
}

void VideoPlayer::downloadLoop() {
    CURL* curl = curl_easy_init();
    if (!curl) {
        m_downloadFinished = true;
        return;
    }

    struct WriteContext {
        VideoPlayer* player;
    } ctx = { this };

    auto writeCb = [](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        size_t totalBytes = size * nmemb;
        WriteContext* wc = (WriteContext*)userdata;
        VideoPlayer* p = wc->player;

        if (p->m_stopRequested.load() || g_appExiting.load()) return 0; // Abort transfer

        const uint8_t* src = (const uint8_t*)ptr;
        size_t remaining = totalBytes;
        while (remaining > 0 && !p->m_stopRequested.load() && !g_appExiting.load()) {
            size_t currentSize = p->m_ringSize.load();
            size_t space = (currentSize < p->m_ringCap) ? (p->m_ringCap - 1 - currentSize) : 0;
            if (space == 0) {
                svcSleepThread(5000000); // 5ms wait for decoder
                continue;
            }
            size_t toWrite = std::min(remaining, space);
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
        p->m_bytesReceived += totalBytes;
        return totalBytes;
    };

    auto xferInfoCb = [](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
        (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
        VideoPlayer* p = (VideoPlayer*)clientp;
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
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 32768L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Plex3DS/1.0");
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "X-Plex-Client-Identifier: Plex3DS-Client-001");
    headers = curl_slist_append(headers, "X-Plex-Client-Profile-Name: Generic");
    headers = curl_slist_append(headers, "X-Plex-Client-Profile-Extra: add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mkv&videoCodec=h264&audioCodec=aac)");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    m_statusMsg = "Connecting to Plex transcode server...";
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && !m_stopRequested.load() && !g_appExiting.load()) {
        m_statusMsg = std::string("Network error: ") + curl_easy_strerror(res);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    m_downloadFinished = true;
}

void VideoPlayer::decodeLoop() {
    AVFormatContext* fmtCtx = nullptr;
    AVIOContext* avioCtx = nullptr;
    unsigned char* avioBuf = nullptr;

    if (m_isLocalFile.load()) {
        m_statusMsg = "Opening local video...";
        if (avformat_open_input(&fmtCtx, m_currentUrl.c_str(), NULL, NULL) < 0) {
            m_statusMsg = "Failed to open local video";
            m_isPlaying = false;
            return;
        }
    } else {
        m_statusMsg = "Connecting to Plex transcode server...";
        // Wait until at least 256 KB is in the ring buffer before opening container
        while (m_ringSize.load() < 256 * 1024 && !m_downloadFinished.load()) {
            if (m_stopRequested.load() || g_appExiting.load()) {
                m_isPlaying = false;
                return;
            }
            svcSleepThread(10000000); // 10ms
        }

        if (m_stopRequested.load() || g_appExiting.load()) {
            m_isPlaying = false;
            return;
        }

        if (m_ringSize.load() == 0 && m_downloadFinished.load()) {
            if (m_statusMsg.find("Network error") == std::string::npos && m_statusMsg.find("Curl:") == std::string::npos) {
                m_statusMsg = "Plex server returned empty stream";
            }
            m_isPlaying = false;
            return;
        }

        m_statusMsg = "Opening container...";
        const size_t AVIO_BUF_SIZE = 32 * 1024;
        avioBuf = (unsigned char*)av_malloc(AVIO_BUF_SIZE);
        if (!avioBuf) {
            m_statusMsg = "Out of memory (avio)";
            m_isPlaying = false;
            return;
        }

        avioCtx = avio_alloc_context(avioBuf, AVIO_BUF_SIZE, 0, this, readPacketCallback, NULL, NULL);
        if (!avioCtx) {
            av_free(avioBuf);
            m_statusMsg = "Failed to allocate AVIO";
            m_isPlaying = false;
            return;
        }

        fmtCtx = avformat_alloc_context();
        if (!fmtCtx) {
            avio_context_free(&avioCtx);
            m_statusMsg = "Failed to allocate format context";
            m_isPlaying = false;
            return;
        }
        fmtCtx->pb = avioCtx;

        if (avformat_open_input(&fmtCtx, NULL, NULL, NULL) < 0) {
            m_statusMsg = "Failed to parse video stream";
            avformat_free_context(fmtCtx);
            avio_context_free(&avioCtx);
            m_isPlaying = false;
            return;
        }
    }

    m_statusMsg = "Reading stream info...";
    if (avformat_find_stream_info(fmtCtx, NULL) < 0) {
        m_statusMsg = "Failed to find stream info";
        avformat_close_input(&fmtCtx);
        if (avioCtx) avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    if (fmtCtx->duration > 0 && m_durationMs.load() == 0) {
        m_durationMs = (fmtCtx->duration * 1000) / AV_TIME_BASE;
    }

    if (m_isLocalFile.load() && m_initialOffsetMs.load() > 0) {
        int64_t targetTimestamp = (m_initialOffsetMs.load() * AV_TIME_BASE) / 1000;
        av_seek_frame(fmtCtx, -1, targetTimestamp, AVSEEK_FLAG_BACKWARD);
    }

    int videoStreamIdx = -1;
    int audioStreamIdx = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIdx == -1) {
            videoStreamIdx = (int)i;
        } else if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIdx == -1) {
            audioStreamIdx = (int)i;
        }
    }

    if (videoStreamIdx == -1) {
        m_statusMsg = "No video stream found in stream";
        avformat_close_input(&fmtCtx);
        avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    AVCodecParameters* codecPar = fmtCtx->streams[videoStreamIdx]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecPar->codec_id);
    if (!decoder) {
        m_statusMsg = "H.264 decoder not available";
        avformat_close_input(&fmtCtx);
        avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx) {
        avformat_close_input(&fmtCtx);
        avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    if (avcodec_parameters_to_context(codecCtx, codecPar) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    // New 3DS ARM11 speed optimizations
    codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    codecCtx->thread_count = 1;

    if (avcodec_open2(codecCtx, decoder, NULL) < 0) {
        m_statusMsg = "Failed to open H.264 decoder";
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    // Audio setup (AAC / MP3)
    AVCodecContext* aCodecCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    int16_t* audioPcmPool = nullptr;
    const int NUM_AUDIO_BUFS = 8;
    const int SAMPLES_PER_AUDIO_BUF = 4096;
    ndspWaveBuf audioWaveBufs[NUM_AUDIO_BUFS];
    memset(audioWaveBufs, 0, sizeof(audioWaveBufs));
    int currentAudioBuf = 0;
    int currentAudioSamples = 0;
    int lastInRate = 0;
    enum AVSampleFormat lastInFmt = AV_SAMPLE_FMT_NONE;

    if (audioStreamIdx != -1) {
        AVCodecParameters* aCodecPar = fmtCtx->streams[audioStreamIdx]->codecpar;
        const AVCodec* aDecoder = avcodec_find_decoder(aCodecPar->codec_id);
        if (aDecoder) {
            aCodecCtx = avcodec_alloc_context3(aDecoder);
            if (aCodecCtx) {
                avcodec_parameters_to_context(aCodecCtx, aCodecPar);
                aCodecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
                aCodecCtx->thread_count = 1;
                if (avcodec_open2(aCodecCtx, aDecoder, NULL) == 0) {
                    size_t poolBytes = NUM_AUDIO_BUFS * SAMPLES_PER_AUDIO_BUF * 2 * sizeof(int16_t);
                    audioPcmPool = (int16_t*)linearAlloc(poolBytes);
                    if (audioPcmPool) {
                        for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
                            audioWaveBufs[i].data_pcm16 = audioPcmPool + (i * SAMPLES_PER_AUDIO_BUF * 2);
                            audioWaveBufs[i].status = NDSP_WBUF_DONE;
                            audioWaveBufs[i].nsamples = 0;
                        }
                        ndspChnReset(m_audioChannel);
                        ndspChnSetInterp(m_audioChannel, NDSP_INTERP_LINEAR);
                        ndspChnSetRate(m_audioChannel, 44100.0f);
                        ndspChnSetFormat(m_audioChannel, NDSP_FORMAT_STEREO_PCM16);
                        float mix[12];
                        memset(mix, 0, sizeof(mix));
                        mix[0] = 1.0f;
                        mix[1] = 1.0f;
                        ndspChnSetMix(m_audioChannel, mix);
                    }
                }
            }
        }
    }

    int srcW = codecCtx->width > 0 ? codecCtx->width : 400;
    int srcH = codecCtx->height > 0 ? codecCtx->height : 240;
    int dstW = 400;
    int dstH = 240;

    struct SwsContext* swsCtx = sws_getContext(
        srcW, srcH, codecCtx->pix_fmt,
        dstW, dstH, AV_PIX_FMT_RGB565LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL
    );

    uint16_t* rgb565Buf = (uint16_t*)av_malloc(dstW * dstH * sizeof(uint16_t));
    if (!rgb565Buf) {
        m_statusMsg = "Out of memory (RGB buffer)";
        if (swsCtx) sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        if (aCodecCtx) avcodec_free_context(&aCodecCtx);
        if (swrCtx) swr_free(&swrCtx);
        if (audioPcmPool) linearFree(audioPcmPool);
        avformat_close_input(&fmtCtx);
        avio_context_free(&avioCtx);
        m_isPlaying = false;
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* aFrame = av_frame_alloc();

    double fps = 30.0;
    AVRational r_fps = fmtCtx->streams[videoStreamIdx]->r_frame_rate;
    if (r_fps.den > 0 && r_fps.num > 0) {
        fps = av_q2d(r_fps);
    } else {
        AVRational avg_fps = fmtCtx->streams[videoStreamIdx]->avg_frame_rate;
        if (avg_fps.den > 0 && avg_fps.num > 0) {
            fps = av_q2d(avg_fps);
        }
    }
    if (fps < 10.0 || fps > 60.0) fps = 30.0;
    int64_t frameDurationMs = (int64_t)(1000.0 / fps);

    uint64_t playbackStartTick = osGetTime();
    int64_t playbackStartPtsMs = -1;
    uint64_t lastFrameTick = osGetTime();
    AVRational vTimeBase = fmtCtx->streams[videoStreamIdx]->time_base;

    m_statusMsg = "Playing";

    uint8_t* dstData[4] = { (uint8_t*)rgb565Buf, NULL, NULL, NULL };
    int dstLinesize[4] = { dstW * (int)sizeof(uint16_t), 0, 0, 0 };

    while (!m_stopRequested.load() && !g_appExiting.load()) {
        if (m_isPaused.load()) {
            uint64_t pauseStart = osGetTime();
            while (m_isPaused.load()) {
                if (m_stopRequested.load() || g_appExiting.load()) break;
                svcSleepThread(20000000); // 20ms
            }
            playbackStartTick += (osGetTime() - pauseStart);
            lastFrameTick = osGetTime();
        }

        int ret = av_read_frame(fmtCtx, pkt);
        if (ret < 0) {
            if (m_downloadFinished.load() && m_ringSize.load() == 0) {
                break; // Stream ended
            }
            svcSleepThread(5000000); // 5ms
            continue;
        }

        if (pkt->stream_index == audioStreamIdx && aCodecCtx && audioPcmPool) {
            if (avcodec_send_packet(aCodecCtx, pkt) == 0) {
                while (avcodec_receive_frame(aCodecCtx, aFrame) == 0) {
                    if (m_stopRequested.load() || g_appExiting.load()) break;

                    int inRate = aFrame->sample_rate > 0 ? aFrame->sample_rate : 44100;
                    enum AVSampleFormat inFmt = (AVSampleFormat)aFrame->format;

                    if (!swrCtx || inRate != lastInRate || inFmt != lastInFmt) {
                        if (swrCtx) swr_free(&swrCtx);
                        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
                        AVChannelLayout inLayout = aFrame->ch_layout;
                        if (inLayout.nb_channels <= 0) {
                            av_channel_layout_default(&inLayout, 2);
                        }
                        swr_alloc_set_opts2(
                            &swrCtx,
                            &outLayout, AV_SAMPLE_FMT_S16, inRate,
                            &inLayout, inFmt, inRate,
                            0, NULL
                        );
                        if (swrCtx) swr_init(swrCtx);
                        ndspChnSetRate(m_audioChannel, (float)inRate);
                        lastInRate = inRate;
                        lastInFmt = inFmt;
                    }

                    if (!swrCtx) continue;

                    int16_t converted[4096 * 2];
                    uint8_t* outData[1] = { (uint8_t*)converted };
                    int actualSamples = swr_convert(
                        swrCtx, outData, 4096,
                        (const uint8_t**)aFrame->extended_data, aFrame->nb_samples
                    );

                    int samplesRemaining = actualSamples;
                    int srcOffset = 0;
                    while (samplesRemaining > 0 && !m_stopRequested.load() && !g_appExiting.load()) {
                        int space = SAMPLES_PER_AUDIO_BUF - currentAudioSamples;
                        int toCopy = std::min(samplesRemaining, space);
                        memcpy(
                            audioWaveBufs[currentAudioBuf].data_pcm16 + currentAudioSamples * 2,
                            converted + srcOffset * 2,
                            toCopy * sizeof(int16_t) * 2
                        );
                        currentAudioSamples += toCopy;
                        srcOffset += toCopy;
                        samplesRemaining -= toCopy;

                        if (currentAudioSamples >= SAMPLES_PER_AUDIO_BUF) {
                            audioWaveBufs[currentAudioBuf].nsamples = SAMPLES_PER_AUDIO_BUF;
                            DSP_FlushDataCache(audioWaveBufs[currentAudioBuf].data_pcm16, SAMPLES_PER_AUDIO_BUF * 4);
                            ndspChnWaveBufAdd(m_audioChannel, &audioWaveBufs[currentAudioBuf]);

                            currentAudioBuf = (currentAudioBuf + 1) % NUM_AUDIO_BUFS;
                            currentAudioSamples = 0;

                            while (audioWaveBufs[currentAudioBuf].status != NDSP_WBUF_DONE && !m_stopRequested.load() && !g_appExiting.load()) {
                                svcSleepThread(2000000); // 2ms
                            }
                        }
                    }
                }
            }
        }
        else if (pkt->stream_index == videoStreamIdx) {
            if (avcodec_send_packet(codecCtx, pkt) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    if (m_stopRequested.load() || g_appExiting.load()) break;

                    // Re-init swsContext if video dimensions changed
                    if (frame->width != srcW || frame->height != srcH) {
                        srcW = frame->width;
                        srcH = frame->height;
                        if (swsCtx) sws_freeContext(swsCtx);
                        swsCtx = sws_getContext(
                            srcW, srcH, (AVPixelFormat)frame->format,
                            dstW, dstH, AV_PIX_FMT_RGB565LE,
                            SWS_FAST_BILINEAR, NULL, NULL, NULL
                        );
                    }

                    if (swsCtx) {
                        sws_scale(swsCtx, frame->data, frame->linesize, 0, srcH, dstData, dstLinesize);
                        tileImageRGB565(rgb565Buf, (uint16_t*)m_videoTex.data, dstW, dstH, 512);
                        GSPGPU_FlushDataCache(m_videoTex.data, m_videoTex.size);
                        m_hasFrame = true;
                    }

                    int64_t pts = frame->best_effort_timestamp;
                    int64_t framePtsMs = (pts != AV_NOPTS_VALUE) ? (int64_t)(pts * av_q2d(vTimeBase) * 1000.0) : -1;

                    if (framePtsMs >= 0) {
                        if (framePtsMs < m_initialOffsetMs.load() - 5000) {
                            m_currentTimeMs = m_initialOffsetMs.load() + framePtsMs;
                        } else {
                            m_currentTimeMs = framePtsMs;
                        }
                        if (playbackStartPtsMs < 0) {
                            playbackStartPtsMs = framePtsMs;
                            playbackStartTick = osGetTime();
                        }
                        int64_t targetTimeMs = framePtsMs - playbackStartPtsMs;
                        int64_t realElapsedMs = (int64_t)(osGetTime() - playbackStartTick);
                        int64_t diff = targetTimeMs - realElapsedMs;

                        if (diff > 0 && diff < 200) {
                            svcSleepThread(diff * 1000000ULL);
                        } else if (diff < -150) {
                            playbackStartTick = osGetTime() - targetTimeMs;
                        } else {
                            svcSleepThread(1000000ULL);
                        }
                    } else {
                        m_currentTimeMs += frameDurationMs;
                        uint64_t now = osGetTime();
                        int64_t elapsed = (int64_t)(now - lastFrameTick);
                        if (elapsed < frameDurationMs) {
                            svcSleepThread((frameDurationMs - elapsed) * 1000000ULL);
                        } else {
                            svcSleepThread(1000000ULL);
                        }
                        lastFrameTick = osGetTime();
                    }
                }
            }
        }

        av_packet_unref(pkt);
    }

    ndspChnReset(m_audioChannel);
    if (audioPcmPool) {
        linearFree(audioPcmPool);
        audioPcmPool = nullptr;
    }
    if (swrCtx) {
        swr_free(&swrCtx);
    }
    if (aCodecCtx) {
        avcodec_free_context(&aCodecCtx);
    }
    av_frame_free(&aFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_free(rgb565Buf);
    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);
    if (avioCtx) avio_context_free(&avioCtx);
    m_isPlaying = false;
}
#endif

int VideoPlayer::readStream(uint8_t* buf, int bufSize) {
    while (m_ringSize.load() < (size_t)bufSize) {
        if (m_stopRequested.load() || g_appExiting.load()) return -1;
        if (m_downloadFinished.load()) {
            size_t available = m_ringSize.load();
            if (available == 0) return 0; // EOF
            bufSize = (int)available;
            break;
        }
#ifdef __3DS__
        svcSleepThread(5000000); // 5ms
#endif
    }

    if (m_stopRequested.load() || g_appExiting.load()) return -1;

    size_t firstPart = std::min((size_t)bufSize, m_ringCap - m_ringTail);
    memcpy(buf, &m_ringBuf[m_ringTail], firstPart);
    if ((size_t)bufSize > firstPart) {
        memcpy(buf + firstPart, &m_ringBuf[0], (size_t)bufSize - firstPart);
    }
    m_ringTail = (m_ringTail + bufSize) % m_ringCap;
    m_ringSize -= bufSize;
    return bufSize;
}

bool VideoPlayer::start(const std::string& streamUrl, int64_t durationMs, int64_t initialOffsetMs) {
    stop();
    m_currentUrl = streamUrl;
    m_durationMs = durationMs;
    m_initialOffsetMs = initialOffsetMs;
    m_currentTimeMs = initialOffsetMs;
    m_isPlaying = true;
    m_isPaused = false;
    m_stopRequested = false;
    m_hasFrame = false;
    m_bytesReceived = 0;
    m_ringHead = 0;
    m_ringTail = 0;
    m_ringSize = 0;
    m_downloadFinished = false;
    m_statusMsg = "Initializing...";
#ifdef __3DS__
    m_connectStartTick = osGetTime();
#else
    m_connectStartTick = (uint64_t)time(nullptr) * 1000;
#endif

    m_isLocalFile = (streamUrl.rfind("http://", 0) != 0 && streamUrl.rfind("https://", 0) != 0);

    if (!m_isLocalFile) {
        int targetSeconds = (int)(initialOffsetMs / 1000);
        size_t offPos = m_currentUrl.find("&offset=");
        if (targetSeconds > 0) {
            if (offPos != std::string::npos) {
                size_t nextAmp = m_currentUrl.find('&', offPos + 8);
                if (nextAmp != std::string::npos) {
                    m_currentUrl.replace(offPos + 8, nextAmp - (offPos + 8), std::to_string(targetSeconds));
                } else {
                    m_currentUrl.replace(offPos + 8, std::string::npos, std::to_string(targetSeconds));
                }
            } else {
                m_currentUrl += "&offset=" + std::to_string(targetSeconds);
            }
        } else if (offPos != std::string::npos) {
            size_t nextAmp = m_currentUrl.find('&', offPos + 8);
            if (nextAmp != std::string::npos) {
                m_currentUrl.erase(offPos, nextAmp - offPos);
            } else {
                m_currentUrl.erase(offPos);
            }
        }
    }

#ifdef __3DS__
    if (!m_isLocalFile) {
        if (!m_ringBuf) {
            m_ringBuf = (uint8_t*)malloc(m_ringCap);
            if (!m_ringBuf) {
                m_isPlaying = false;
                return false;
            }
        }

        // Spawn download thread (priority 0x31)
        m_downloadThread = threadCreate(downloadThreadEntry, this, 64 * 1024, 0x31, -2, false);
        if (!m_downloadThread) {
            m_isPlaying = false;
            return false;
        }
    } else {
        m_downloadFinished = true;
    }

    // Spawn decode thread (priority 0x30, try any core -1, fallback to default core -2)
    m_decodeThread = threadCreate(decodeThreadEntry, this, 128 * 1024, 0x30, -1, false);
    if (!m_decodeThread) {
        m_decodeThread = threadCreate(decodeThreadEntry, this, 128 * 1024, 0x30, -2, false);
    }
    if (!m_decodeThread) {
        m_statusMsg = "Failed to create decode thread";
        stop();
        return false;
    }
#endif
    return true;
}

void VideoPlayer::update() {
    // Thread handles frame processing autonomously
}

void VideoPlayer::pause() {
    m_isPaused = true;
#ifdef __3DS__
    ndspChnSetPaused(m_audioChannel, true);
#endif
}

void VideoPlayer::resume() {
    m_isPaused = false;
#ifdef __3DS__
    ndspChnSetPaused(m_audioChannel, false);
#endif
}

void VideoPlayer::stop() {
    m_stopRequested = true;
    m_isPaused = false;

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
    ndspChnReset(m_audioChannel);
#endif
    m_isPlaying = false;
    m_hasFrame = false;
    m_statusMsg = "Stopped";
}

void VideoPlayer::seekTo(int targetSeconds) {
    if (!m_isPlaying.load() && !m_hasFrame.load() && m_currentUrl.empty()) return;
    if (targetSeconds < 0) targetSeconds = 0;
    int64_t total = m_durationMs.load();
    if (total > 0 && targetSeconds > (int)(total / 1000)) {
        targetSeconds = (int)(total / 1000);
    }

    if (m_isLocalFile.load()) {
        start(m_currentUrl, total, (int64_t)targetSeconds * 1000);
        return;
    }

    std::string url = m_currentUrl;
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

    start(url, total, (int64_t)targetSeconds * 1000);
}

void VideoPlayer::seek(int deltaSeconds) {
    seekTo(getCurrentSeconds() + deltaSeconds);
}
