#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// H.264 sequence parameter set helpers for the mirrored stream (VideoRenderer). Kept free of
// Windows APIs so that test/h264sps can check them on the desktop against FFmpeg's decoder.
namespace H264
{
    // The SPS fields that decide how a decoder buffers and outputs frames, and where the VUI flag
    // or the bitstream_restriction flag sits (for RewriteSps)
    struct SpsInfo
    {
        std::string text;
        bool ok = false;
        uint32_t refFrames = 0;
        bool hasVui = false;
        bool hasRestriction = false;
        uint32_t reorderFrames = 0;
        size_t vuiFlagBit = 0;          // position of vui_parameters_present_flag in the RBSP
        size_t restrictionFlagBit = 0;  // position of bitstream_restriction_flag (when hasVui)
    };

    // nal: one SPS NAL unit including its header byte, without a start code
    SpsInfo ParseSps(const uint8_t* nal, size_t len);

    // Returns the SPS NAL with a bitstream restriction that declares no frame reordering, or an
    // empty vector to keep it as it is (see the .cpp). A reorderFrames other than 0 is for tests.
    std::vector<uint8_t> RewriteSps(const uint8_t* nal, size_t len, const SpsInfo& info, uint32_t reorderFrames = 0);

    // slice_type of a slice NAL modulo 5 (0 P, 1 B, 2 I, 3 SP, 4 SI), or -1
    int SliceType(const uint8_t* nal, size_t len);
}
