/*
 * decode.cc
 * Waveform Seekbar plugin for Audacious
 *
 * See decode.h. The FFmpeg open/decode dance below mirrors
 * src/ffaudio/ffaudio-core.cc's play() (same project, so same idioms), but
 * reads local files directly through FFmpeg's own IO instead of going
 * through a VFSFile-backed AVIOContext -- this plugin only ever decodes
 * local files (see waveform.cc's is-it-worth-building gate), so there's no
 * need for Audacious's transport-plugin abstraction here.
 */

#include "decode.h"

#include <math.h>
#include <stdlib.h>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
}

#define VALUES_PER_SAMPLE 3
#define MAX_CHANNELS 16
// Safety valve against runaway memory use if a file's duration metadata is
// wildly wrong (or absent) -- see the comment on the accumulation loop
// below for why samples are buffered in full rather than bucketed
// incrementally. 50M samples/channel is ~9.6 hours at 44.1kHz.
#define MAX_SAMPLES_PER_CHANNEL 50000000

namespace
{
struct FFContext
{
    AVFormatContext * fmt = nullptr;
    AVCodecContext * codec = nullptr;
    int stream_idx = -1;

    ~FFContext()
    {
        if (codec)
            avcodec_free_context(&codec);
        if (fmt)
            avformat_close_input(&fmt);
    }
};

bool open_ctx(const char * path, FFContext & ctx)
{
    if (avformat_open_input(&ctx.fmt, path, nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(ctx.fmt, nullptr) < 0)
        return false;

    for (unsigned i = 0; i < ctx.fmt->nb_streams; i++)
    {
        if (ctx.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            ctx.stream_idx = (int)i;
            break;
        }
    }
    if (ctx.stream_idx < 0)
        return false;

    AVCodecParameters * par = ctx.fmt->streams[ctx.stream_idx]->codecpar;
    const AVCodec * dec = avcodec_find_decoder(par->codec_id);
    if (!dec)
        return false;

    ctx.codec = avcodec_alloc_context3(dec);
    if (!ctx.codec)
        return false;

    avcodec_parameters_to_context(ctx.codec, par);
    if (avcodec_open2(ctx.codec, dec, nullptr) < 0)
        return false;

    return true;
}

int channel_count(const AVCodecContext * codec)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    return codec->ch_layout.nb_channels;
#else
    return codec->channels;
#endif
}

// Reads one sample for channel `ch`, index `i` within `frame`, normalized
// to [-1, 1]. Handles the packed and planar variants of U8/S16/S32/FLT/DBL
// -- the same practical subset src/ffaudio's convert_format() handles (plus
// double, which it doesn't bother with).
float sample_at(const AVFrame * frame, int channels, int ch, int i)
{
    auto fmt = (AVSampleFormat)frame->format;
    bool planar = av_sample_fmt_is_planar(fmt);
    AVSampleFormat base = av_get_packed_sample_fmt(fmt);
    const uint8_t * buf = planar ? frame->data[ch] : frame->data[0];
    int pos = planar ? i : i * channels + ch;

    switch (base)
    {
    case AV_SAMPLE_FMT_U8:
        return (((const uint8_t *)buf)[pos] - 128) / 128.0f;
    case AV_SAMPLE_FMT_S16:
        return ((const int16_t *)buf)[pos] / 32768.0f;
    case AV_SAMPLE_FMT_S32:
        return ((const int32_t *)buf)[pos] / 2147483648.0f;
    case AV_SAMPLE_FMT_FLT:
        return ((const float *)buf)[pos];
    case AV_SAMPLE_FMT_DBL:
        return (float)((const double *)buf)[pos];
    default:
        return 0.0f;
    }
}
} // namespace

bool waveform_decode_build(const char * path, int num_buckets, WaveData & out)
{
    if (num_buckets <= 0)
        return false;

    FFContext ctx;
    if (!open_ctx(path, ctx))
        return false;

    int channels = channel_count(ctx.codec);
    if (channels <= 0 || channels > MAX_CHANNELS)
        return false;

    // Buffer every normalized sample first, then bucket in a second, purely
    // in-memory pass. This is simpler and more robust than bucketing
    // on the fly (which needs an accurate total-sample-count up front to
    // size each bucket, and container duration metadata is not always
    // trustworthy) at the cost of holding the whole decoded track in memory
    // momentarily; bounded by MAX_SAMPLES_PER_CHANNEL and, in practice, by
    // the caller's max-file-length setting (see waveform.cc).
    std::vector<std::vector<float>> samples(channels);

    AVPacket * pkt = av_packet_alloc();
    AVFrame * frame = av_frame_alloc();
    bool stop = false;

    while (!stop && av_read_frame(ctx.fmt, pkt) >= 0)
    {
        if (pkt->stream_index == ctx.stream_idx)
        {
            if (avcodec_send_packet(ctx.codec, pkt) == 0)
            {
                while (avcodec_receive_frame(ctx.codec, frame) == 0)
                {
                    for (int i = 0; i < frame->nb_samples; i++)
                    {
                        for (int ch = 0; ch < channels; ch++)
                            samples[ch].push_back(sample_at(frame, channels, ch, i));
                    }
                    if ((int)samples[0].size() >= MAX_SAMPLES_PER_CHANNEL)
                    {
                        stop = true;
                        break;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Flush the decoder.
    if (!stop)
    {
        avcodec_send_packet(ctx.codec, nullptr);
        while (avcodec_receive_frame(ctx.codec, frame) == 0)
        {
            for (int i = 0; i < frame->nb_samples; i++)
                for (int ch = 0; ch < channels; ch++)
                    samples[ch].push_back(sample_at(frame, channels, ch, i));
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    size_t nsamples = samples[0].size();
    if (nsamples == 0)
        return false;

    out.channels = channels;
    out.data_len = (size_t)channels * VALUES_PER_SAMPLE * num_buckets;
    out.data = new short[out.data_len];

    const double per_bucket = nsamples / (double)num_buckets;

    for (int ch = 0; ch < channels; ch++)
    {
        const std::vector<float> & chan = samples[ch];
        double start = 0;

        for (int b = 0; b < num_buckets; b++)
        {
            double end = (b + 1 < num_buckets) ? (b + 1) * per_bucket : (double)nsamples;
            size_t i_start = (size_t)start;
            size_t i_end = (size_t)end;
            if (i_end <= i_start)
                i_end = i_start + 1;
            if (i_end > nsamples)
                i_end = nsamples;

            float mn = 1.0f, mx = -1.0f;
            double rms_sum = 0.0;
            size_t count = 0;
            for (size_t i = i_start; i < i_end; i++, count++)
            {
                float s = chan[i];
                if (s > mx)
                    mx = s;
                if (s < mn)
                    mn = s;
                rms_sum += (double)s * s;
            }
            float rms = count ? (float)sqrt(rms_sum / count) : 0.0f;
            if (count == 0)
            {
                mn = 0.0f;
                mx = 0.0f;
            }

            size_t idx = ((size_t)b * channels + ch) * VALUES_PER_SAMPLE;
            out.data[idx] = (short)(mx * 1000);
            out.data[idx + 1] = (short)(mn * 1000);
            out.data[idx + 2] = (short)(rms * 1000);

            start = end;
        }
    }

    return true;
}
