#include "pch.h"
#include "AudioDecoder.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace
{
    // AudioSpecificConfig of AirPlay mirroring audio: AOT 39 (ER AAC ELD), 44.1 kHz, stereo,
    // frameLengthFlag = 1 (480 samples), ldSbrPresentFlag = 0
    const uint8_t kEldConfig[] = {0xF8, 0xE8, 0x50, 0x00};

    void PutBE32(uint8_t* p, uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

    int16_t ToPcm16(float v)
    {
        float s = v * 32768.0f;
        return static_cast<int16_t>(std::clamp(s, -32768.0f, 32767.0f));
    }
}

AudioDecoder::~AudioDecoder()
{
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    avcodec_free_context(&m_ctx);
}

bool AudioDecoder::InitAacEld()
{
    return Open(AV_CODEC_ID_AAC, kEldConfig, sizeof(kEldConfig));
}

bool AudioDecoder::InitAlac(uint32_t frameLength)
{
    // 'alac' atom: size, type, version, then the ALACSpecificConfig of AirPlay's stream, as in the
    // RAOP fmtp line "96 352 0 16 40 10 14 2 255 0 0 44100"
    uint8_t atom[36] = {};
    PutBE32(atom, sizeof(atom));
    memcpy(atom + 4, "alac", 4);
    PutBE32(atom + 12, frameLength);
    atom[16] = 0;    // compatible version
    atom[17] = 16;   // bit depth
    atom[18] = 40;   // rice history mult (pb)
    atom[19] = 10;   // rice initial history (mb)
    atom[20] = 14;   // rice limit (kb)
    atom[21] = 2;    // channels
    atom[22] = 0;    // max run 255
    atom[23] = 255;
    PutBE32(atom + 32, 44100);
    return Open(AV_CODEC_ID_ALAC, atom, sizeof(atom));
}

bool AudioDecoder::Open(int codecId, const uint8_t* config, size_t configLen)
{
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codecId));
    if (!codec) return false;
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) return false;
    m_ctx->extradata = static_cast<uint8_t*>(av_mallocz(configLen + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!m_ctx->extradata) return false;
    memcpy(m_ctx->extradata, config, configLen);
    m_ctx->extradata_size = static_cast<int>(configLen);
    m_ctx->sample_rate = 44100;
    av_channel_layout_default(&m_ctx->ch_layout, 2);
    if (avcodec_open2(m_ctx, codec, nullptr) < 0) return false;
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    return m_packet && m_frame;
}

int AudioDecoder::Decode(const uint8_t* data, size_t len, std::vector<int16_t>& pcm)
{
    if (!m_ctx) return AVERROR(EINVAL);
    int ret = av_new_packet(m_packet, static_cast<int>(len));  // zero-padded as FFmpeg requires
    if (ret < 0) return ret;
    memcpy(m_packet->data, data, len);
    ret = avcodec_send_packet(m_ctx, m_packet);
    av_packet_unref(m_packet);
    if (ret < 0) return ret;

    int samples = 0;
    while ((ret = avcodec_receive_frame(m_ctx, m_frame)) == 0)
    {
        int channels = m_frame->ch_layout.nb_channels;
        int n = m_frame->nb_samples;
        size_t base = pcm.size();
        pcm.resize(base + static_cast<size_t>(n) * 2);
        int16_t* out = pcm.data() + base;
        // the decoders output planar samples (AAC: float, ALAC: 16-bit); write interleaved
        // stereo (mono is duplicated)
        if (m_frame->format == AV_SAMPLE_FMT_FLTP)
        {
            const float* l = reinterpret_cast<const float*>(m_frame->extended_data[0]);
            const float* r = reinterpret_cast<const float*>(m_frame->extended_data[channels > 1 ? 1 : 0]);
            for (int i = 0; i < n; i++)
            {
                out[2 * i] = ToPcm16(l[i]);
                out[2 * i + 1] = ToPcm16(r[i]);
            }
        }
        else if (m_frame->format == AV_SAMPLE_FMT_S16P)
        {
            const int16_t* l = reinterpret_cast<const int16_t*>(m_frame->extended_data[0]);
            const int16_t* r = reinterpret_cast<const int16_t*>(m_frame->extended_data[channels > 1 ? 1 : 0]);
            for (int i = 0; i < n; i++)
            {
                out[2 * i] = l[i];
                out[2 * i + 1] = r[i];
            }
        }
        else
        {
            pcm.resize(base);  // not produced for 16-bit AirPlay streams
            n = 0;
        }
        samples += n;
        av_frame_unref(m_frame);
    }
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? samples : ret;
}
