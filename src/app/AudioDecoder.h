#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

// AirPlay audio decoder (44.1 kHz stereo) on FFmpeg's native LGPL decoders, as UxPlay does
// through GStreamer's avdec_aac / avdec_alac:
//   AAC-ELD: screen mirroring audio, 480-sample frames, no LD-SBR
//   ALAC:    AirPlay speaker audio, 352-sample frames
// FFmpeg checks every frame against the stream configuration, so a damaged or hostile packet is
// rejected instead of being decoded past the end of a buffer.
class AudioDecoder
{
public:
    AudioDecoder() = default;
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool InitAacEld();
    bool InitAlac(uint32_t frameLength);
    // Decodes one packet, appending interleaved 16-bit stereo to pcm; returns samples per
    // channel, 0 if no frame was produced, or a negative FFmpeg error code.
    int Decode(const uint8_t* data, size_t len, std::vector<int16_t>& pcm);

private:
    bool Open(int codecId, const uint8_t* config, size_t configLen);

    AVCodecContext* m_ctx = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
};
