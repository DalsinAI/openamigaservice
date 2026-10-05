/*
 * media.decode/1 on the host, sound (docs/MEDIA_DECODE.md): any audio file
 * FFmpeg reads (FLAC, Ogg Vorbis, Opus, MP3, AAC, ALAC, WMA, WAV...)
 * answered as 16-bit big-endian PCM at the rate and channels asked for.
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
#include <libswresample/swresample.h>

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

    s->format = fourcc(codec->id);
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
int md_is_av(const uint8_t *d, uint32_t n)
{
    AVProbeData pd = { 0 };
    uint8_t *buf;
    int score = 0;

    n = n > 65536 ? 65536 : n;
    if (!(buf = av_mallocz(n + AVPROBE_PADDING_SIZE)))
        return 0;
    memcpy(buf, d, n);
    pd.filename = "";
    pd.buf = buf;
    pd.buf_size = n;
    av_probe_input_format3(&pd, 1, &score);
    av_free(buf);
    return score >= AVPROBE_SCORE_MAX / 4;
}
