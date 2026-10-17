// Checks src/app/HlsAudio.cpp against FFmpeg: builds HLS packed-audio segments like YouTube's (an
// ID3 tag with the transport stream timestamp, then ADTS AAC frames), converts them to MPEG-TS and
// reads the result back with FFmpeg's MPEG-TS demuxer and AAC decoder.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include "HlsAudio.h"

using Bytes = std::vector<uint8_t>;

static int g_failures = 0;
static void Check(bool ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

// AAC-LC frames of a 440 Hz tone (44.1 kHz stereo), each with an ADTS header
static std::vector<Bytes> EncodeAdts(int frames)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = 44100;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->bit_rate = 96000;
    av_channel_layout_default(&ctx->ch_layout, 2);
    avcodec_open2(ctx, codec, nullptr);
    AVFrame* f = av_frame_alloc();
    f->nb_samples = ctx->frame_size;
    f->format = ctx->sample_fmt;
    av_channel_layout_copy(&f->ch_layout, &ctx->ch_layout);
    av_frame_get_buffer(f, 0);
    AVPacket* pkt = av_packet_alloc();
    std::vector<Bytes> out;
    double phase = 0;
    auto drain = [&] {
        while (avcodec_receive_packet(ctx, pkt) == 0)
        {
            int len = pkt->size + 7;
            Bytes adts = {0xFF, 0xF1, (uint8_t) ((1 << 6) | (4 << 2) | (2 >> 2)), (uint8_t) (((2 & 3) << 6) | (len >> 11)),
                          (uint8_t) ((len >> 3) & 0xFF), (uint8_t) (((len & 7) << 5) | 0x1F), 0xFC};
            adts.insert(adts.end(), pkt->data, pkt->data + pkt->size);
            out.push_back(adts);
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < frames + 2; i++)
    {
        av_frame_make_writable(f);
        for (int s = 0; s < f->nb_samples; s++)
        {
            float v = (float) (0.3 * sin(phase));
            phase += 2 * 3.14159265358979 * 440 / 44100;
            ((float*) f->data[0])[s] = v;
            ((float*) f->data[1])[s] = v;
        }
        f->pts = (int64_t) i * f->nb_samples;
        avcodec_send_frame(ctx, f);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    if ((int) out.size() > frames) out.resize(frames);
    return out;
}

static void PutSyncSafe(Bytes& b, uint32_t v)
{
    b.push_back((v >> 21) & 0x7F);
    b.push_back((v >> 14) & 0x7F);
    b.push_back((v >> 7) & 0x7F);
    b.push_back(v & 0x7F);
}

// the ID3 tag HLS puts before packed audio: a PRIV frame with the 33-bit MPEG-2 timestamp
static Bytes Id3(int version, uint64_t pts)
{
    static const char owner[] = "com.apple.streaming.transportStreamTimestamp";
    Bytes body(owner, owner + sizeof(owner));
    for (int i = 7; i >= 0; i--) body.push_back((uint8_t) (pts >> (8 * i)));
    Bytes frame = {'P', 'R', 'I', 'V'};
    if (version >= 4) PutSyncSafe(frame, (uint32_t) body.size());
    else
        for (int i = 3; i >= 0; i--) frame.push_back((uint8_t) (body.size() >> (8 * i)));
    frame.push_back(0);
    frame.push_back(0);
    frame.insert(frame.end(), body.begin(), body.end());
    Bytes tag = {'I', 'D', '3', (uint8_t) version, 0, 0};
    PutSyncSafe(tag, (uint32_t) frame.size());
    tag.insert(tag.end(), frame.begin(), frame.end());
    return tag;
}

struct Reader
{
    const Bytes* data;
    size_t pos = 0;
};

static int ReadPacket(void* opaque, uint8_t* buf, int size)
{
    Reader* r = (Reader*) opaque;
    size_t n = std::min((size_t) size, r->data->size() - r->pos);
    if (n == 0) return AVERROR_EOF;
    memcpy(buf, r->data->data() + r->pos, n);
    r->pos += n;
    return (int) n;
}

static void Demux(const Bytes& ts, int expectFrames, uint64_t expectPts)
{
    Reader reader{&ts};
    uint8_t* iobuf = (uint8_t*) av_malloc(4096);
    AVIOContext* io = avio_alloc_context(iobuf, 4096, 0, &reader, ReadPacket, nullptr, nullptr);
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->pb = io;
    int rc = avformat_open_input(&fmt, nullptr, av_find_input_format("mpegts"), nullptr);
    Check(rc == 0, "FFmpeg opens it as MPEG-TS");
    if (rc != 0) return;
    avformat_find_stream_info(fmt, nullptr);
    Check(fmt->nb_streams == 1 && fmt->streams[0]->codecpar->codec_id == AV_CODEC_ID_AAC, "one AAC stream");
    AVCodecParameters* par = fmt->streams[0]->codecpar;
    printf("  stream: %s, %d Hz, %d channels\n", avcodec_get_name(par->codec_id), par->sample_rate, par->ch_layout.nb_channels);
    Check(par->sample_rate == 44100 && par->ch_layout.nb_channels == 2, "44.1 kHz stereo");

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    avcodec_open2(dec, codec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int packets = 0, decoded = 0;
    int64_t firstPts = AV_NOPTS_VALUE, lastPts = AV_NOPTS_VALUE;
    bool increasing = true;
    while (av_read_frame(fmt, pkt) >= 0)
    {
        if (firstPts == AV_NOPTS_VALUE) firstPts = pkt->pts;
        if (lastPts != AV_NOPTS_VALUE && pkt->pts <= lastPts) increasing = false;
        lastPts = pkt->pts;
        packets++;
        if (avcodec_send_packet(dec, pkt) == 0)
            while (avcodec_receive_frame(dec, frame) == 0) decoded++;
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, nullptr);
    while (avcodec_receive_frame(dec, frame) == 0) decoded++;
    printf("  %d packets, %d decoded frames, PTS %lld .. %lld (expected start %llu)\n", packets, decoded,
           (long long) firstPts, (long long) lastPts, (unsigned long long) expectPts);
    Check(packets == expectFrames, "every AAC frame comes out as a packet");
    Check(decoded >= expectFrames - 2, "the frames decode");
    Check(firstPts == (int64_t) expectPts, "timestamps start at the ID3 timestamp");
    Check(increasing, "timestamps increase");
    int64_t expectLast = (int64_t) expectPts + (int64_t) (expectFrames - 1) * 1024 * 90000 / 44100;
    Check(std::llabs(lastPts - expectLast) <= 2, "1024 samples per frame at 44.1 kHz");
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_freep(&io->buffer);
    avio_context_free(&io);
}

static void RunCase(const char* name, const Bytes& segment, int frames, uint64_t pts)
{
    printf("%s (%zu bytes)\n", name, segment.size());
    Check(HlsAudio::IsPackedAudio(segment.data(), segment.size()), "recognized as packed audio");
    uint64_t readPts = 0;
    Bytes ts = HlsAudio::PackedAudioToTs(segment.data(), segment.size(), &readPts);
    Check(!ts.empty() && ts.size() % 188 == 0, "whole 188-byte TS packets");
    bool sync = !ts.empty();
    for (size_t i = 0; i < ts.size(); i += 188) sync = sync && ts[i] == 0x47;
    Check(sync, "every packet starts with the sync byte");
    Check(readPts == pts, "ID3 timestamp read");
    if (!ts.empty()) Demux(ts, frames, pts);
}

int main()
{
    av_log_set_level(AV_LOG_ERROR);
    const int frames = 280;  // ~6.5 s, like a YouTube audio segment
    std::vector<Bytes> adts = EncodeAdts(frames);
    Bytes body;
    for (auto& f : adts) body.insert(body.end(), f.begin(), f.end());
    const uint64_t pts = 0x1F0000000ULL + 12345;  // near the 33-bit limit

    for (int version : {3, 4})
    {
        Bytes segment = Id3(version, pts);
        segment.insert(segment.end(), body.begin(), body.end());
        std::string name = "-- ID3v2." + std::to_string(version) + " + ADTS";
        RunCase(name.c_str(), segment, (int) adts.size(), pts);
    }
    RunCase("-- ADTS without ID3", body, (int) adts.size(), 0);

    printf("-- not packed audio\n");
    Bytes tsLike(188 * 4, 0);
    tsLike[0] = 0x47;
    Check(!HlsAudio::IsPackedAudio(tsLike.data(), tsLike.size()), "MPEG-TS is left alone");
    Bytes junk(5000);
    for (size_t i = 0; i < junk.size(); i++) junk[i] = (uint8_t) (i * 37 + 11);
    Check(HlsAudio::PackedAudioToTs(junk.data(), junk.size()).empty(), "garbage gives nothing");
    Bytes cut(body.begin(), body.begin() + body.size() / 2 + 3);  // a truncated last frame
    Check(!HlsAudio::PackedAudioToTs(cut.data(), cut.size()).empty(), "a truncated segment still converts");

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
