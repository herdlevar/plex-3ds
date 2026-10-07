#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

// Forward declare only the demuxers and decoders used by Plex3DS
extern const struct AVInputFormat ff_matroska_demuxer;
extern const struct AVInputFormat ff_mov_demuxer;
extern const struct AVInputFormat ff_mp3_demuxer;

static const struct AVInputFormat * const s_demuxer_list[] = {
    &ff_matroska_demuxer,
    &ff_mov_demuxer,
    &ff_mp3_demuxer,
    NULL
};

const struct AVInputFormat *av_demuxer_iterate(void **opaque) {
    uintptr_t i = (uintptr_t)*opaque;
    if (i < sizeof(s_demuxer_list) / sizeof(s_demuxer_list[0]) - 1) {
        const struct AVInputFormat *f = s_demuxer_list[i];
        *opaque = (void*)(i + 1);
        return f;
    }
    return NULL;
}

const struct AVOutputFormat *av_muxer_iterate(void **opaque) {
    (void)opaque;
    return NULL;
}

void avpriv_register_devices(const void *m, const void *d) {
    (void)m; (void)d;
}

extern const struct AVCodec ff_h264_decoder;
extern const struct AVCodec ff_aac_decoder;
extern const struct AVCodec ff_mp3_decoder;

static const struct AVCodec * const s_codec_list[] = {
    &ff_h264_decoder,
    &ff_aac_decoder,
    &ff_mp3_decoder,
    NULL
};

const struct AVCodec *av_codec_iterate(void **opaque) {
    uintptr_t i = (uintptr_t)*opaque;
    if (i < sizeof(s_codec_list) / sizeof(s_codec_list[0]) - 1) {
        const struct AVCodec *c = s_codec_list[i];
        *opaque = (void*)(i + 1);
        return c;
    }
    return NULL;
}

const struct AVCodec *avcodec_find_decoder(enum AVCodecID id) {
    for (int i = 0; s_codec_list[i]; i++) {
        if (s_codec_list[i]->id == id) {
            return s_codec_list[i];
        }
    }
    return NULL;
}

const struct AVCodec *avcodec_find_decoder_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; s_codec_list[i]; i++) {
        if (s_codec_list[i]->name && strcmp(s_codec_list[i]->name, name) == 0) {
            return s_codec_list[i];
        }
    }
    return NULL;
}

const struct AVCodec *avcodec_find_encoder(enum AVCodecID id) {
    (void)id;
    return NULL;
}

const struct AVCodec *avcodec_find_encoder_by_name(const char *name) {
    (void)name;
    return NULL;
}

// ntohl
uint32_t ntohl(uint32_t netlong) {
    return __builtin_bswap32(netlong);
}

// LAME stubs
void* lame_init(void) { return NULL; }
int lame_set_num_channels(void* gfp, int c) { (void)gfp; (void)c; return 0; }
int lame_set_mode(void* gfp, int m) { (void)gfp; (void)m; return 0; }
int lame_set_in_samplerate(void* gfp, int r) { (void)gfp; (void)r; return 0; }
int lame_set_out_samplerate(void* gfp, int r) { (void)gfp; (void)r; return 0; }
int lame_set_quality(void* gfp, int q) { (void)gfp; (void)q; return 0; }
int lame_set_VBR(void* gfp, int v) { (void)gfp; (void)v; return 0; }
int lame_set_VBR_quality(void* gfp, float q) { (void)gfp; (void)q; return 0; }
int lame_set_VBR_mean_bitrate_kbps(void* gfp, int b) { (void)gfp; (void)b; return 0; }
int lame_set_brate(void* gfp, int b) { (void)gfp; (void)b; return 0; }
int lame_set_lowpassfreq(void* gfp, int f) { (void)gfp; (void)f; return 0; }
int lame_set_bWriteVbrTag(void* gfp, int t) { (void)gfp; (void)t; return 0; }
int lame_set_disable_reservoir(void* gfp, int d) { (void)gfp; (void)d; return 0; }
int lame_set_copyright(void* gfp, int c) { (void)gfp; (void)c; return 0; }
int lame_set_original(void* gfp, int o) { (void)gfp; (void)o; return 0; }
int lame_init_params(void* gfp) { (void)gfp; return 0; }
int lame_get_encoder_delay(void* gfp) { (void)gfp; return 0; }
int lame_get_framesize(void* gfp) { (void)gfp; return 0; }
int lame_encode_buffer(void* gfp, const short int* l, const short int* r, int s, unsigned char* m, int ms) {
    (void)gfp; (void)l; (void)r; (void)s; (void)m; (void)ms; return -1;
}
int lame_encode_buffer_float(void* gfp, const float* l, const float* r, int s, unsigned char* m, int ms) {
    (void)gfp; (void)l; (void)r; (void)s; (void)m; (void)ms; return -1;
}
int lame_encode_buffer_int(void* gfp, const int* l, const int* r, int s, unsigned char* m, int ms) {
    (void)gfp; (void)l; (void)r; (void)s; (void)m; (void)ms; return -1;
}
int lame_encode_flush(void* gfp, unsigned char* m, int s) { (void)gfp; (void)m; (void)s; return 0; }
int lame_close(void* gfp) { (void)gfp; return 0; }

// x264 stubs
const void* x264_levels = NULL;
int x264_encoder_delayed_frames(void* h) { (void)h; return 0; }
int x264_encoder_encode(void* h, void** pp_nal, int* pi_nal, void* pic_in, void* pic_out) {
    (void)h; (void)pp_nal; (void)pi_nal; (void)pic_in; (void)pic_out; return -1;
}
void x264_picture_init(void* pic) { (void)pic; }
int x264_encoder_reconfig(void* h, void* param) { (void)h; (void)param; return 0; }
void x264_param_cleanup(void* param) { (void)param; }
void x264_encoder_close(void* h) { (void)h; }
int x264_param_parse(void* p, const char* name, const char* value) { (void)p; (void)name; (void)value; return 0; }
void x264_param_default(void* param) { (void)param; }
int x264_param_default_preset(void* param, const char* preset, const char* tune) {
    (void)param; (void)preset; (void)tune; return 0;
}
int x264_param_apply_fastfirstpass(void* param) { (void)param; return 0; }
int x264_param_apply_profile(void* param, const char* profile) { (void)param; (void)profile; return 0; }
void* x264_encoder_open_164(void* param) { (void)param; return NULL; }
int x264_encoder_headers(void* h, void** pp_nal, int* pi_nal) { (void)h; (void)pp_nal; (void)pi_nal; return 0; }
int x264_encoder_maximum_delayed_frames(void* h) { (void)h; return 0; }

// dav1d stubs
void dav1d_data_unref(void* data) { (void)data; }
void dav1d_flush(void* c) { (void)c; }
int dav1d_data_wrap(void* data, const uint8_t* ptr, size_t sz, void* free_cb, void* cookie) {
    (void)data; (void)ptr; (void)sz; (void)free_cb; (void)cookie; return -1;
}
int dav1d_data_wrap_user_data(void* data, const uint8_t* ptr, size_t sz, void* free_cb, void* cookie) {
    (void)data; (void)ptr; (void)sz; (void)free_cb; (void)cookie; return -1;
}
int dav1d_send_data(void* c, void* data) { (void)c; (void)data; return -1; }
int dav1d_get_picture(void* c, void* pic) { (void)c; (void)pic; return -1; }
void dav1d_picture_unref(void* pic) { (void)pic; }
int dav1d_get_event_flags(void* c, int* flags) { (void)c; (void)flags; return 0; }
void dav1d_close(void** c) { (void)c; }
int dav1d_parse_sequence_header(void* h, const uint8_t* ptr, size_t sz) {
    (void)h; (void)ptr; (void)sz; return -1;
}
const char* dav1d_version(void) { return "stub"; }
void dav1d_default_settings(void* s) { (void)s; }
int dav1d_open(void** c, const void* s) { (void)c; (void)s; return -1; }
int dav1d_get_frame_delay(const void* s) { (void)s; return 0; }

// H.274 Film Grain stubs (saves ~702 KB from h274.o)
int ff_h274_apply_film_grain(void *out_frame, const void *in_frame, const void *params) {
    (void)out_frame; (void)in_frame; (void)params;
    return 0;
}
void ff_h274_hash_freep(void *hash) {
    (void)hash;
}
int ff_h274_hash_init(void *hash, int type) {
    (void)hash; (void)type;
    return 0;
}
int ff_h274_hash_verify(void *hash, const void *frame) {
    (void)hash; (void)frame;
    return 0;
}

// AVTX double and int32 FFT codelets stubs (saves ~916 KB from tx_double.o and tx_int32.o)
const void * const ff_tx_codelet_list_double_c[] = { NULL };
const void * const ff_tx_codelet_list_int32_c[] = { NULL };
