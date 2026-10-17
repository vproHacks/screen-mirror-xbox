// Checks src/app/H264Sps.cpp against FFmpeg's H.264 decoder.
//
// iOS mirroring streams use an SPS without a bitstream restriction, so decoders must assume frame
// reordering and hold frames back. This test encodes a stream with openh264, removes the
// restriction (and, separately, the whole VUI) from its SPS to get iOS-like headers, then checks
// that H264::RewriteSps restores a valid SPS: FFmpeg must decode every frame, report no reordering
// delay, and return each frame as soon as its packet is sent.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <wels/codec_api.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "H264Sps.h"

using Bytes = std::vector<uint8_t>;

static int g_failures = 0;
static void Check(bool ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

// RBSP bit copy helpers for building the iOS-like variants
static Bytes Rbsp(const Bytes& nal)
{
    Bytes out;
    for (size_t i = 1; i < nal.size(); i++)
    {
        if (i + 2 < nal.size() && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3)
        {
            out.push_back(0);
            out.push_back(0);
            i += 2;
            continue;
        }
        out.push_back(nal[i]);
    }
    return out;
}

// the SPS cut at `bit`, followed by a 0 flag and the RBSP stop bit
static Bytes CutSps(const Bytes& nal, size_t bit)
{
    Bytes rbsp = Rbsp(nal), out;
    size_t pos = 0;
    auto put = [&](int v) {
        if (pos % 8 == 0) out.push_back(0);
        if (v) out.back() |= (uint8_t) (0x80 >> (pos % 8));
        pos++;
    };
    for (size_t i = 0; i < bit; i++) put((rbsp[i / 8] >> (7 - i % 8)) & 1);
    put(0);
    put(1);
    while (pos % 8) put(0);
    Bytes nalOut{nal[0]};
    int zeros = 0;
    for (uint8_t b : out)
    {
        if (zeros >= 2 && b <= 3)
        {
            nalOut.push_back(3);
            zeros = 0;
        }
        nalOut.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return nalOut;
}

struct Stream
{
    Bytes sps, pps;
    std::vector<Bytes> slices;  // one access unit each (start codes included)
};

static Stream Encode(int frames, int w, int h, int profile)
{
    Stream s;
    ISVCEncoder* enc = nullptr;
    WelsCreateSVCEncoder(&enc);
    SEncParamExt p;
    enc->GetDefaultParams(&p);
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = w;
    p.iPicHeight = h;
    p.iTargetBitrate = 2000000;
    p.iRCMode = RC_BITRATE_MODE;
    p.fMaxFrameRate = 30;
    p.iSpatialLayerNum = 1;
    p.iNumRefFrame = 4;  // like iOS
    p.uiIntraPeriod = 0;  // one IDR, like iOS
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.iEntropyCodingModeFlag = profile == PRO_HIGH ? 1 : 0;
    p.sSpatialLayers[0].iVideoWidth = w;
    p.sSpatialLayers[0].iVideoHeight = h;
    p.sSpatialLayers[0].fFrameRate = 30;
    p.sSpatialLayers[0].iSpatialBitrate = 2000000;
    p.sSpatialLayers[0].uiProfileIdc = (EProfileIdc) profile;
    p.sSpatialLayers[0].bVideoSignalTypePresent = true;  // gives the SPS a VUI
    p.sSpatialLayers[0].uiVideoFormat = VF_UNDEF;
    p.sSpatialLayers[0].bColorDescriptionPresent = true;
    p.sSpatialLayers[0].uiColorPrimaries = CP_BT709;
    p.sSpatialLayers[0].uiTransferCharacteristics = TRC_BT709;
    p.sSpatialLayers[0].uiColorMatrix = CM_BT709;
    enc->InitializeExt(&p);
    std::vector<uint8_t> yuv(w * h * 3 / 2);
    for (int f = 0; f < frames; f++)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) yuv[y * w + x] = (uint8_t) ((x + y + f * 7) & 0xff);
        memset(yuv.data() + w * h, 128, w * h / 2);
        SSourcePicture pic{};
        pic.iColorFormat = videoFormatI420;
        pic.iPicWidth = w;
        pic.iPicHeight = h;
        pic.iStride[0] = w;
        pic.iStride[1] = pic.iStride[2] = w / 2;
        pic.pData[0] = yuv.data();
        pic.pData[1] = yuv.data() + w * h;
        pic.pData[2] = pic.pData[1] + w * h / 4;
        pic.uiTimeStamp = f * 33;
        SFrameBSInfo info{};
        if (enc->EncodeFrame(&pic, &info) != cmResultSuccess || info.eFrameType == videoFrameTypeSkip) continue;
        Bytes au;
        for (int l = 0; l < info.iLayerNum; l++)
        {
            uint8_t* q = info.sLayerInfo[l].pBsBuf;
            for (int n = 0; n < info.sLayerInfo[l].iNalCount; n++)
            {
                int len = info.sLayerInfo[l].pNalLengthInByte[n];
                int sc = q[2] == 1 ? 3 : 4;
                int type = q[sc] & 0x1f;
                if (type == 7) s.sps.assign(q + sc, q + len);
                else if (type == 8) s.pps.assign(q + sc, q + len);
                else au.insert(au.end(), q, q + len);
                q += len;
            }
        }
        if (!au.empty()) s.slices.push_back(au);
    }
    enc->Uninitialize();
    WelsDestroySVCEncoder(enc);
    return s;
}

// decodes the stream with the given SPS; returns frames out, and the most packets a frame waited
static void Decode(const Stream& s, const Bytes& sps, int& decoded, int& maxDelay, int& reorderDelay)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_open2(ctx, codec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    decoded = 0;
    maxDelay = 0;
    int sent = 0;
    auto receive = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0)
        {
            decoded++;
            maxDelay = std::max(maxDelay, sent - decoded);
            av_frame_unref(frame);
        }
    };
    const uint8_t sc[] = {0, 0, 0, 1};
    for (size_t i = 0; i < s.slices.size(); i++)
    {
        Bytes au;
        if (i == 0)
        {
            au.insert(au.end(), sc, sc + 4);
            au.insert(au.end(), sps.begin(), sps.end());
            au.insert(au.end(), sc, sc + 4);
            au.insert(au.end(), s.pps.begin(), s.pps.end());
        }
        au.insert(au.end(), s.slices[i].begin(), s.slices[i].end());
        av_new_packet(pkt, (int) au.size());
        memcpy(pkt->data, au.data(), au.size());
        if (avcodec_send_packet(ctx, pkt) == 0) sent++;
        av_packet_unref(pkt);
        receive();
    }
    reorderDelay = ctx->has_b_frames;
    avcodec_send_packet(ctx, nullptr);
    receive();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
}

static void RunCase(const char* name, const Stream& s, const Bytes& sps)
{
    printf("%s\n", name);
    H264::SpsInfo before = H264::ParseSps(sps.data(), sps.size());
    printf("  SPS: %s\n", before.text.c_str());
    Bytes rewritten = H264::RewriteSps(sps.data(), sps.size(), before);
    Check(!rewritten.empty(), "rewritten");
    if (rewritten.empty()) return;
    H264::SpsInfo after = H264::ParseSps(rewritten.data(), rewritten.size());
    printf("  new: %s\n", after.text.c_str());
    Check(after.ok && after.hasVui && after.hasRestriction && after.reorderFrames == 0, "declares no reordering");
    Check(after.refFrames == before.refFrames, "keeps the reference frame count");
    size_t prefix = before.text.find(", timing");
    if (prefix == std::string::npos) prefix = before.text.find(", no VUI");
    Check(after.text.compare(0, prefix, before.text, 0, prefix) == 0, "keeps the fields before the VUI");
    Check(H264::RewriteSps(rewritten.data(), rewritten.size(), after).empty(), "leaves a rewritten SPS alone");

    int decodedOld, delayOld, reorderOld, decodedNew, delayNew, reorderNew;
    Decode(s, sps, decodedOld, delayOld, reorderOld);
    Decode(s, rewritten, decodedNew, delayNew, reorderNew);
    printf("  FFmpeg, original SPS:  %d/%zu frames, frames held back up to %d packets (reorder delay %d)\n", decodedOld,
           s.slices.size(), delayOld, reorderOld);
    printf("  FFmpeg, rewritten SPS: %d/%zu frames, frames held back up to %d packets (reorder delay %d)\n", decodedNew,
           s.slices.size(), delayNew, reorderNew);
    Check(decodedNew == (int) s.slices.size(), "every frame decodes with the rewritten SPS");
    Check(delayNew == 0 && reorderNew == 0, "each frame comes out as soon as its packet is sent");

    // the decoder must read the field where it was written: declaring 2 reorder frames must make
    // FFmpeg hold back exactly 2 frames
    Bytes two = H264::RewriteSps(sps.data(), sps.size(), before, 2);
    int decodedTwo, delayTwo, reorderTwo;
    Decode(s, two, decodedTwo, delayTwo, reorderTwo);
    printf("  FFmpeg, SPS declaring 2 reorder frames: %d/%zu frames, held back up to %d packets (reorder delay %d)\n",
           decodedTwo, s.slices.size(), delayTwo, reorderTwo);
    Check(decodedTwo == (int) s.slices.size() && reorderTwo == 2 && delayTwo == 2, "FFmpeg reads the written field");
}

int main()
{
    for (int profile : {PRO_BASELINE, PRO_HIGH})
    {
        Stream s = Encode(90, 500, 1080, profile);
        printf("== openh264 %s, %zu frames, 500x1080\n", profile == PRO_HIGH ? "High" : "Baseline", s.slices.size());
        H264::SpsInfo info = H264::ParseSps(s.sps.data(), s.sps.size());
        printf("  encoder SPS: %s\n", info.text.c_str());
        if (!info.ok || !info.hasVui)
        {
            Check(false, "encoder SPS has a VUI");
            continue;
        }
        RunCase("-- iOS-like: VUI without bitstream restriction", s, CutSps(s.sps, info.restrictionFlagBit));
        RunCase("-- no VUI at all", s, CutSps(s.sps, info.vuiFlagBit));
        printf("-- slice types: first %d (I expected), second %d (P expected)\n",
               H264::SliceType(s.slices[0].data() + 4, s.slices[0].size() - 4),
               H264::SliceType(s.slices[1].data() + 4, s.slices[1].size() - 4));
        Check(H264::SliceType(s.slices[1].data() + 4, s.slices[1].size() - 4) == 0, "P slice type read");
    }
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
