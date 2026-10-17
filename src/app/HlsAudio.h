#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// HLS "packed audio" segments (an ID3 tag carrying the segment's MPEG-2 timestamp, then raw ADTS
// AAC frames; RFC 8216 section 3.4) rewrapped as MPEG-TS. YouTube's separate audio renditions use
// this format (with .ts names), and the platform's adaptive streaming player only reads MPEG-TS
// and fragmented MP4 segments. Kept free of Windows APIs so that test/hlsaudio can check it on the
// desktop against FFmpeg.
namespace HlsAudio
{
    // true if the data starts like packed audio (an ID3 tag or an ADTS frame header)
    bool IsPackedAudio(const uint8_t* data, size_t len);

    // The segment as MPEG-TS (PAT, PMT and one PES per AAC frame with its PTS), or an empty vector if
    // the data holds no ADTS frames. *pts receives the timestamp read from the ID3 tag (90 kHz).
    std::vector<uint8_t> PackedAudioToTs(const uint8_t* data, size_t len, uint64_t* pts = nullptr);
}
