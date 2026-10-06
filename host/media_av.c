/*
 * media.decode/1 on the host, sound and video (docs/MEDIA_DECODE.md): any
 * audio FFmpeg reads (FLAC, Ogg Vorbis, Opus, MP3, AAC, ALAC, WMA, WAV...)
 * answered as 16-bit big-endian PCM at the rate and channels asked for, and
 * any video (H.264, HEVC, AV1, VP9, MPEG-4, WMV...) kept open and answered
 * frame by frame in 256 colours or 24-bit, scaled to fit.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#include "media_decode.h"
#include "media_av.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>

/* Longer sounds are refused: 2^28 frames is 1 GB of 16-bit stereo. */
#define MAX_FRAMES (1u << 28)

struct mem {
    const uint8_t *data;
    int64_t size, pos;
};

static int mem_read(void *opaque, uint8_t *buf, int n)
{
    struct mem *m = opaque;
    int64_t left = m->size - m->pos;
    if (left <= 0)
        return AVERROR_EOF;
    if (n > left)
        n = (int)left;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence)
{
    struct mem *m = opaque;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return m->size;
    case SEEK_SET: break;
    case SEEK_CUR: off += m->pos; break;
    case SEEK_END: off += m->size; break;
    default: return -1;
    }
    if (off < 0 || off > m->size)
        return -1;
    return m->pos = off;
}

struct sound {
    struct mem mem;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream;
    uint32_t format, rate, channels;     /* the answer's */
};

static void sound_close(struct sound *s)
{
    av_frame_free(&s->frame);
    av_packet_free(&s->pkt);
    swr_free(&s->swr);
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
    if (s->io) {
        av_freep(&s->io->buffer);
        avio_context_free(&s->io);
    }
}

static uint32_t fourcc(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_FLAC: return MD_FORMAT_FLAC;
    case AV_CODEC_ID_VORBIS: return MD_FORMAT_VORBIS;
    case AV_CODEC_ID_OPUS: return MD_FORMAT_OPUS;
    case AV_CODEC_ID_MP3: return MD_FORMAT_MP3;
    case AV_CODEC_ID_AAC: return MD_FORMAT_AAC;
    case AV_CODEC_ID_ALAC: return MD_FORMAT_ALAC;
    case AV_CODEC_ID_WMAV1: case AV_CODEC_ID_WMAV2: case AV_CODEC_ID_WMAPRO: case AV_CODEC_ID_WMALOSSLESS:
        return MD_FORMAT_WMA;
    default: return MD_FORMAT_SOUND;
    }
}

/* Opens the best audio stream; the answer has at most maxch channels and a
 * rate of at most maxrate (0: the file's), halving the file's rate until it fits. */
static int sound_open(struct sound *s, const uint8_t *d, uint32_t n, uint32_t maxch, uint32_t maxrate)
{
    const AVCodec *codec = NULL;
    AVChannelLayout out_layout;
    uint8_t *iobuf;
    int st;

    memset(s, 0, sizeof *s);
    av_log_set_level(AV_LOG_ERROR);
    s->mem.data = d;
    s->mem.size = n;
    if (!(iobuf = av_malloc(65536)))
        return MD_HOSTERROR;
    if (!(s->io = avio_alloc_context(iobuf, 65536, 0, &s->mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return MD_HOSTERROR;
    }
    if (!(s->fmt = avformat_alloc_context()))
        return MD_HOSTERROR;
    s->fmt->pb = s->io;
    if (avformat_open_input(&s->fmt, NULL, NULL, NULL) < 0)
        return MD_BADREQUEST;                 /* frees fmt */
    if (avformat_find_stream_info(s->fmt, NULL) < 0)
        return MD_BADREQUEST;
    if ((s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) < 0 || !codec)
        return MD_BADREQUEST;
    if (!(s->dec = avcodec_alloc_context3(codec)))
        return MD_HOSTERROR;
    if (avcodec_parameters_to_context(s->dec, s->fmt->streams[s->stream]->codecpar) < 0 ||
        avcodec_open2(s->dec, codec, NULL) < 0 || s->dec->sample_rate <= 0 || s->dec->ch_layout.nb_channels <= 0)
        return MD_BADREQUEST;

    s->format = !strcmp(s->fmt->iformat->name, "libopenmpt") ? MD_FORMAT_MODULE
              : !strcmp(s->fmt->iformat->name, "libgme") ? MD_FORMAT_CHIPTUNE : fourcc(codec->id);
    s->channels = s->dec->ch_layout.nb_channels;
    if (maxch && s->channels > maxch)
        s->channels = maxch;
    if (s->channels > 2)
        s->channels = 2;
    s->rate = s->dec->sample_rate;
    while (maxrate && s->rate > maxrate && s->rate >= 2000)
        s->rate /= 2;
    if (maxrate && s->rate > maxrate)
        s->rate = maxrate;

    av_channel_layout_default(&out_layout, s->channels);
    st = swr_alloc_set_opts2(&s->swr, &out_layout, AV_SAMPLE_FMT_S16, s->rate,
                             &s->dec->ch_layout, s->dec->sample_fmt, s->dec->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (st < 0 || swr_init(s->swr) < 0)
        return MD_HOSTERROR;
    if (!(s->pkt = av_packet_alloc()) || !(s->frame = av_frame_alloc()))
        return MD_HOSTERROR;
    return MD_OK;
}

/* Where decoded frames go: frames before skip are dropped; the rest are
 * written to out (big-endian) until room frames are written, and counted. */
struct sink {
    uint64_t skip, room, pos, written;
    uint8_t *out;
    uint32_t channels;
};

static void sink_put(struct sink *k, const int16_t *pcm, int n)
{
    int i;
    uint32_t c;

    for (i = 0; i < n; i++, k->pos++) {
        if (k->pos < k->skip || !k->out || k->written >= k->room)
            continue;
        for (c = 0; c < k->channels; c++) {
            int16_t v = pcm[i * k->channels + c];
            uint8_t *o = k->out + (k->written * k->channels + c) * 2;
            o[0] = (uint8_t)((uint16_t)v >> 8);
            o[1] = (uint8_t)v;
        }
        k->written++;
    }
}

/* Runs in (NULL: flushes) through the resampler into the sink. */
static int resample(struct sound *s, struct sink *k, const uint8_t **in, int n)
{
    int room = swr_get_out_samples(s->swr, n), got;
    int16_t *pcm;

    if (room <= 0)
        return 0;
    if (!(pcm = av_malloc((size_t)room * s->channels * 2)))
        return -1;
    got = swr_convert(s->swr, (uint8_t **)&pcm, room, in, n);
    if (got > 0)
        sink_put(k, pcm, got);
    av_free(pcm);
    return got < 0 ? -1 : 0;
}

static void drain(struct sound *s, struct sink *k)
{
    while (avcodec_receive_frame(s->dec, s->frame) == 0) {
        resample(s, k, (const uint8_t **)s->frame->extended_data, s->frame->nb_samples);
        av_frame_unref(s->frame);
    }
}

/* Decodes the whole stream through the resampler into k. */
static void sound_run(struct sound *s, struct sink *k)
{
    k->channels = s->channels;
    while (av_read_frame(s->fmt, s->pkt) >= 0 && k->pos < MAX_FRAMES + k->skip + 1) {
        if (s->pkt->stream_index == s->stream && avcodec_send_packet(s->dec, s->pkt) >= 0)
            drain(s, k);
        av_packet_unref(s->pkt);
    }
    avcodec_send_packet(s->dec, NULL);
    drain(s, k);
    resample(s, k, NULL, 0);
}

int md_sound_probe(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                   uint32_t *result, uint32_t *aux)
{
    struct sound s;
    struct sink k = { 0 };
    uint64_t frames;
    int st = sound_open(&s, file->in, file->length, extra[0], extra[1]);

    if (st == MD_OK) {
        sound_run(&s, &k);
        frames = k.pos;
        if (frames > MAX_FRAMES)
            st = MD_BADREQUEST;
        else {
            md_put32(info, MD_KIND_SOUND);
            md_put32(info + 4, s.format);
            md_put32(info + 8, 0);
            md_put32(info + 12, (uint32_t)frames);
            md_put32(info + 16, s.rate);
            md_put32(info + 20, s.channels);
            *result = (uint32_t)frames;
            *aux = s.rate;
        }
    }
    sound_close(&s);
    return st;
}

int md_sound_decode(const struct md_buffer *file, uint32_t first, const uint32_t extra[4], struct md_buffer *out,
                    uint32_t *result, uint32_t *aux)
{
    struct sound s;
    struct sink k = { 0 };
    int st = sound_open(&s, file->in, file->length, extra[0], extra[1]);

    if (st == MD_OK) {
        if (out->length < s.channels * 2)
            st = MD_TOOSMALL;
        else {
            k.skip = first;
            k.out = out->out;
            k.room = out->length / (s.channels * 2);
            sound_run(&s, &k);
            out->written = (uint32_t)(k.written * s.channels * 2);
            *result = (uint32_t)k.written;
            *aux = s.rate;
        }
    }
    sound_close(&s);
    return st;
}

/* 1 when FFmpeg recognises the file's first bytes. */
static const AVInputFormat *probe(const uint8_t *d, uint32_t n)
{
    const AVInputFormat *f;
    AVProbeData pd = { 0 };
    uint8_t *buf;
    int score = 0;

    av_log_set_level(AV_LOG_ERROR);
    n = n > 65536 ? 65536 : n;
    if (!(buf = av_mallocz(n + AVPROBE_PADDING_SIZE)))
        return NULL;
    memcpy(buf, d, n);
    pd.filename = "";
    pd.buf = buf;
    pd.buf_size = n;
    f = av_probe_input_format3(&pd, 1, &score);
    av_free(buf);
    return score >= AVPROBE_SCORE_MAX / 4 ? f : NULL;
}

/* A still picture: one of FFmpeg's piped image readers (jpegxl_pipe, ...). */
static int is_still(const AVInputFormat *f)
{
    size_t len = strlen(f->name);
    /* GIF and APNG answer PROBE and DECODE with their first frame, an icon
     * file with its first icon. */
    return (len > 5 && !strcmp(f->name + len - 5, "_pipe")) || !strcmp(f->name, "gif") || !strcmp(f->name, "apng")
           || !strcmp(f->name, "ico");
}

int md_is_av(const uint8_t *d, uint32_t n)
{
    const AVInputFormat *f = probe(d, n);
    return !f ? MD_AV_NONE : is_still(f) ? MD_AV_STILL : MD_AV_MEDIA;
}

/* ---- still pictures ------------------------------------------------------ */

static uint32_t still_fourcc(const char *name)
{
    static const struct { const char *name; uint32_t fourcc; } map[] = {
        { "jpeg_pipe", MD_FORMAT_JPEG }, { "png_pipe", MD_FORMAT_PNG }, { "gif_pipe", MD_FORMAT_GIF }, { "gif", MD_FORMAT_GIF },
        { "apng", MD_FORMAT_PNG }, { "ico", MD_FORMAT_ICO },
        { "jpegxl_pipe", MD_FORMAT_JXL }, { "exr_pipe", MD_FORMAT_EXR }, { "hdr_pipe", MD_FORMAT_HDR },
        { "psd_pipe", MD_FORMAT_PSD }, { "qoi_pipe", MD_FORMAT_QOI }, { "dds_pipe", MD_FORMAT_DDS },
        { "j2k_pipe", MD_FORMAT_J2K }, { "tiff_pipe", MD_FORMAT_TIFF }, { "webp_pipe", MD_FORMAT_WEBP },
        { "dpx_pipe", MD_FORMAT_DPX }, { "pcx_pipe", MD_FORMAT_PCX }, { "sgi_pipe", MD_FORMAT_SGI },
    };
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(name, map[i].name))
            return map[i].fourcc;
    return MD_FORMAT_STILL;
}

int md_still_load(const uint8_t *d, uint32_t n, int decode, uint32_t *format, uint32_t *flags,
                  uint32_t *width, uint32_t *height, uint8_t **argb)
{
    const AVInputFormat *f = probe(d, n);
    struct mem mem = { d, n, 0 };
    AVIOContext *io = NULL;
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    struct SwsContext *sws = NULL;
    uint8_t *iobuf;
    int st = MD_BADREQUEST, stream, got = 0;

    *argb = NULL;
    if (!f || !is_still(f))
        return MD_BADREQUEST;
    if (!(iobuf = av_malloc(65536)))
        return MD_HOSTERROR;
    if (!(io = avio_alloc_context(iobuf, 65536, 0, &mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return MD_HOSTERROR;
    }
    if (!(fmt = avformat_alloc_context()))
        goto out;
    fmt->pb = io;
    if (avformat_open_input(&fmt, NULL, f, NULL) < 0)
        goto out;
    /* No avformat_find_stream_info: it decodes the picture once just to look. */
    if ((stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0)
        goto out;
    /* An icon file holds the same icon at several sizes: the largest. */
    if (!strcmp(f->name, "ico")) {
        unsigned i;
        for (i = 0; i < fmt->nb_streams; i++) {
            const AVCodecParameters *a = fmt->streams[i]->codecpar, *b = fmt->streams[stream]->codecpar;
            const AVCodec *c;
            if ((int64_t)a->width * a->height > (int64_t)b->width * b->height && (c = avcodec_find_decoder(a->codec_id))) {
                stream = i;
                codec = c;
            }
        }
    }
    if (!(dec = avcodec_alloc_context3(codec))
        || avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0
        || avcodec_open2(dec, codec, NULL) < 0)
        goto out;
    *format = still_fourcc(f->name);
    *flags = 0;
    *width = dec->width;
    *height = dec->height;
    if (!decode && *width && *height) {
        const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(dec->pix_fmt);
        if (pd && (pd->flags & AV_PIX_FMT_FLAG_ALPHA))
            *flags = MD_FLAG_ALPHA;
        st = MD_OK;
        goto out;
    }
    if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc()))
        goto out;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt) >= 0)
            got = avcodec_receive_frame(dec, frame) >= 0;
        av_packet_unref(pkt);
    }
    if (!got && avcodec_send_packet(dec, NULL) >= 0)
        got = avcodec_receive_frame(dec, frame) >= 0;
    if (!got || frame->width <= 0 || frame->height <= 0
        || (uint64_t)frame->width * frame->height > (1u << 28))
        goto out;
    {
        const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(frame->format);
        int stride = frame->width * 4;
        *width = frame->width;
        *height = frame->height;
        *flags = pd && (pd->flags & AV_PIX_FMT_FLAG_ALPHA) ? MD_FLAG_ALPHA : 0;
        if (!decode) {
            st = MD_OK;
            goto out;
        }
        if (!(sws = sws_getContext(frame->width, frame->height, frame->format, frame->width, frame->height,
                                   AV_PIX_FMT_ARGB, SWS_POINT, NULL, NULL, NULL))
            || !(*argb = malloc((size_t)stride * frame->height))) {
            st = MD_HOSTERROR;
            goto out;
        }
        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, argb, &stride);
        if (!*flags) {
            size_t i, px = (size_t)frame->width * frame->height;
            for (i = 0; i < px; i++)
                (*argb)[i * 4] = 255;
        }
        st = MD_OK;
    }
out:
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    return st;
}

/* ---- video --------------------------------------------------------------- */

/* Open videos, by handle (index + 1). A video stays open, with its own copy
 * of the file, until VCLOSE or until MAX_VIDEOS newer ones push it out. */
#define MAX_VIDEOS 8

struct video {
    int used;
    uint32_t serial;           /* for pushing out the oldest */
    uint8_t *file;
    struct mem mem;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    struct SwsContext *sws;
    AVPacket *pkt;
    AVFrame *frame, *rgb;
    int stream;
    int64_t *pts;              /* each frame's time stamp, in presentation order */
    uint32_t frames, ow, oh;
    int64_t next;              /* the frame the decoder gives next; -1: unknown (seek) */
};

static struct video videos[MAX_VIDEOS];
static uint32_t video_serial;
static pthread_mutex_t video_lock = PTHREAD_MUTEX_INITIALIZER;

static void video_free(struct video *v)
{
    av_frame_free(&v->frame);
    av_frame_free(&v->rgb);
    av_packet_free(&v->pkt);
    sws_freeContext(v->sws);
    avcodec_free_context(&v->dec);
    avformat_close_input(&v->fmt);
    if (v->io) {
        av_freep(&v->io->buffer);
        avio_context_free(&v->io);
    }
    free(v->pts);
    free(v->file);
    memset(v, 0, sizeof *v);
}

static uint32_t video_fourcc(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return MD_FORMAT_H264;
    case AV_CODEC_ID_HEVC: return MD_FORMAT_HEVC;
    case AV_CODEC_ID_AV1: return MD_FORMAT_AV1;
    case AV_CODEC_ID_VP8: return MD_FORMAT_VP8;
    case AV_CODEC_ID_VP9: return MD_FORMAT_VP9;
    case AV_CODEC_ID_MPEG4: return MD_FORMAT_MPEG4;
    case AV_CODEC_ID_MPEG1VIDEO: case AV_CODEC_ID_MPEG2VIDEO: return MD_FORMAT_MPEG2;
    case AV_CODEC_ID_WMV1: case AV_CODEC_ID_WMV2: case AV_CODEC_ID_WMV3: case AV_CODEC_ID_VC1: return MD_FORMAT_WMV;
    case AV_CODEC_ID_MJPEG: return MD_FORMAT_MJPEG;
    case AV_CODEC_ID_THEORA: return MD_FORMAT_THEORA;
    default: return MD_FORMAT_VIDEO;
    }
}

static int cmp64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static int video_open(struct video *v, const uint8_t *d, uint32_t n, uint32_t maxw, uint32_t maxh, uint32_t *fps1000)
{
    const AVCodec *codec = NULL;
    AVStream *st;
    uint8_t *iobuf;
    uint32_t cap = 0;
    AVRational rate;

    av_log_set_level(AV_LOG_ERROR);
    if (!(v->file = malloc(n)))
        return MD_HOSTERROR;
    memcpy(v->file, d, n);
    v->mem.data = v->file;
    v->mem.size = n;
    if (!(iobuf = av_malloc(65536)))
        return MD_HOSTERROR;
    if (!(v->io = avio_alloc_context(iobuf, 65536, 0, &v->mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return MD_HOSTERROR;
    }
    if (!(v->fmt = avformat_alloc_context()))
        return MD_HOSTERROR;
    v->fmt->pb = v->io;
    if (avformat_open_input(&v->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(v->fmt, NULL) < 0)
        return MD_BADREQUEST;
    if ((v->stream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0 || !codec)
        return MD_BADREQUEST;
    st = v->fmt->streams[v->stream];
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        return MD_BADREQUEST;                 /* cover art in a sound file */
    if (!(v->dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(v->dec, st->codecpar) < 0)
        return MD_HOSTERROR;
    v->dec->thread_count = 4;
    if (avcodec_open2(v->dec, codec, NULL) < 0 || v->dec->width <= 0 || v->dec->height <= 0)
        return MD_BADREQUEST;
    if (!(v->pkt = av_packet_alloc()) || !(v->frame = av_frame_alloc()) || !(v->rgb = av_frame_alloc()))
        return MD_HOSTERROR;

    /* Every frame's time stamp, from the packets alone (no decoding). */
    while (av_read_frame(v->fmt, v->pkt) >= 0) {
        if (v->pkt->stream_index == v->stream) {
            int64_t t = v->pkt->pts != AV_NOPTS_VALUE ? v->pkt->pts : v->pkt->dts;
            if (v->frames == cap) {
                int64_t *more = realloc(v->pts, (cap = cap ? cap * 2 : 1024) * sizeof *more);
                if (!more) {
                    av_packet_unref(v->pkt);
                    return MD_HOSTERROR;
                }
                v->pts = more;
            }
            v->pts[v->frames] = t == AV_NOPTS_VALUE ? (int64_t)v->frames : t;
            v->frames++;
        }
        av_packet_unref(v->pkt);
    }
    if (!v->frames)
        return MD_BADREQUEST;
    qsort(v->pts, v->frames, sizeof *v->pts, cmp64);
    v->next = -1;

    md_fit(v->dec->width, v->dec->height, maxw, maxh, &v->ow, &v->oh);
    if (!(v->sws = sws_getContext(v->dec->width, v->dec->height, v->dec->pix_fmt, v->ow, v->oh, AV_PIX_FMT_RGB24,
                                  SWS_AREA, NULL, NULL, NULL)))
        return MD_HOSTERROR;
    v->rgb->format = AV_PIX_FMT_RGB24;
    v->rgb->width = v->ow;
    v->rgb->height = v->oh;
    if (av_frame_get_buffer(v->rgb, 0) < 0)
        return MD_HOSTERROR;
    rate = av_guess_frame_rate(v->fmt, st, NULL);
    *fps1000 = rate.num > 0 && rate.den > 0 ? (uint32_t)((int64_t)rate.num * 1000 / rate.den) : 25000;
    return MD_OK;
}

/* The 6x6x6 colour cube with 4x4 ordered dithering: index i < 216 is red
 * (i / 36), green (i / 6 % 6) and blue (i % 6), each step 51, as
 * openamigaimage's webm.datatype draws. */
static void dither(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, uint8_t *out)
{
    static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)y * pitch;
        for (x = 0; x < w; x++, s += 3) {
            int dd = bayer[y & 3][x & 3] * 16;
            *out++ = (uint8_t)(((s[0] * 5 + dd) >> 8) * 36 + ((s[1] * 5 + dd) >> 8) * 6 + ((s[2] * 5 + dd) >> 8));
        }
    }
}

/* Decodes up to frame index want (by its time stamp) and leaves it in v->rgb. */
static int video_seek_decode(struct video *v, uint32_t want)
{
    int64_t target = v->pts[want];
    int eof = 0;

    if (v->next < 0 || want < v->next || want > v->next + 50) {
        avcodec_flush_buffers(v->dec);
        if (av_seek_frame(v->fmt, v->stream, target, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(v->fmt, v->stream, v->pts[0], AVSEEK_FLAG_BACKWARD);
        v->next = -1;
    }
    for (;;) {
        int r = avcodec_receive_frame(v->dec, v->frame);
        if (r == 0) {
            int64_t t = v->frame->best_effort_timestamp;
            if (t == AV_NOPTS_VALUE || t >= target) {
                sws_scale(v->sws, (const uint8_t *const *)v->frame->data, v->frame->linesize, 0, v->dec->height,
                          v->rgb->data, v->rgb->linesize);
                av_frame_unref(v->frame);
                v->next = want + 1;
                return MD_OK;
            }
            av_frame_unref(v->frame);
            continue;
        }
        if (r == AVERROR_EOF || (r != AVERROR(EAGAIN)))
            return MD_BADREQUEST;
        if (eof)
            return MD_BADREQUEST;
        if (av_read_frame(v->fmt, v->pkt) < 0) {
            avcodec_send_packet(v->dec, NULL);
            eof = 1;
            continue;
        }
        if (v->pkt->stream_index == v->stream)
            avcodec_send_packet(v->dec, v->pkt);
        av_packet_unref(v->pkt);
    }
}

int md_video_open(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                  uint32_t *result, uint32_t *aux)
{
    struct video *v = NULL;
    uint32_t i, fps = 0;
    int st, audio;

    pthread_mutex_lock(&video_lock);
    for (i = 0; i < MAX_VIDEOS && !v; i++)
        if (!videos[i].used)
            v = &videos[i];
    if (!v) {                                  /* push out the oldest */
        v = &videos[0];
        for (i = 1; i < MAX_VIDEOS; i++)
            if (videos[i].serial < v->serial)
                v = &videos[i];
        video_free(v);
    }
    v->used = 1;
    v->serial = ++video_serial;
    st = video_open(v, file->in, file->length, extra[0], extra[1], &fps);
    if (st != MD_OK) {
        video_free(v);
        pthread_mutex_unlock(&video_lock);
        return st;
    }
    audio = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0;
    md_put32(info, MD_KIND_ANIMATION);
    md_put32(info + 4, video_fourcc(v->dec->codec_id));
    md_put32(info + 8, audio ? MD_FLAG_SOUND : 0);
    md_put32(info + 12, v->frames);
    md_put32(info + 16, v->ow);
    md_put32(info + 20, v->oh);
    *result = (uint32_t)(v - videos) + 1;
    *aux = fps;
    pthread_mutex_unlock(&video_lock);
    return MD_OK;
}

int md_video_frame(uint32_t handle, const uint32_t extra[4], struct md_buffer *out, uint32_t *result, uint32_t *aux)
{
    struct video *v;
    uint32_t want = extra[0];
    int st;

    if (!handle || handle > MAX_VIDEOS)
        return MD_BADREQUEST;
    pthread_mutex_lock(&video_lock);
    v = &videos[handle - 1];
    if (!v->used) {
        pthread_mutex_unlock(&video_lock);
        return MD_BADREQUEST;
    }
    if (want >= v->frames)
        want = v->frames - 1;
    if (!out->out || out->length < v->ow * v->oh) {
        pthread_mutex_unlock(&video_lock);
        return MD_TOOSMALL;
    }
    if ((st = video_seek_decode(v, want)) == MD_OK) {
        if (extra[1] == 1) {                       /* 24-bit RGB */
            uint32_t y;
            if (out->length < v->ow * v->oh * 3)
                st = MD_TOOSMALL;
            else {
                for (y = 0; y < v->oh; y++)
                    memcpy(out->out + (size_t)y * v->ow * 3, v->rgb->data[0] + (size_t)y * v->rgb->linesize[0], v->ow * 3);
                out->written = v->ow * v->oh * 3;
            }
        } else {
            dither(v->rgb->data[0], v->rgb->linesize[0], v->ow, v->oh, out->out);
            out->written = v->ow * v->oh;
        }
        *result = want;
        *aux = 0;
    }
    pthread_mutex_unlock(&video_lock);
    return st;
}

int md_video_close(uint32_t handle)
{
    if (!handle || handle > MAX_VIDEOS)
        return MD_BADREQUEST;
    pthread_mutex_lock(&video_lock);
    if (videos[handle - 1].used)
        video_free(&videos[handle - 1]);
    pthread_mutex_unlock(&video_lock);
    return MD_OK;
}
