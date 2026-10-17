// fakesender: plays the part of an iPhone in an AirPlay screen-mirroring session, so the
// receiver can be tested without Apple hardware.
//
// It performs the same RTSP exchange as iOS (GET /info, pair-setup, pair-verify, fp-setup,
// SETUP/RECORD), answers the receiver's NTP timing requests, then streams H.264 video (a frame
// counter or a picture, encoded with openh264) over the encrypted mirroring channel and,
// optionally, ALAC audio (a tone or an audio file decoded with FFmpeg) over RTP.
//
// An iPhone sends mirroring audio as AAC-ELD. There is no free AAC-ELD encoder that can be
// combined with this GPL code (FFmpeg only encodes it through the Fraunhofer library), so the
// mirroring audio here is ALAC, which the receiver accepts in either mode; AAC-ELD decoding
// is exercised with a real iPhone.
//
// FairPlay: only the receiver side of the FairPlay handshake is public. The sender sends a
// random key message and a random encrypted key; it then runs the receiver's own
// playfair_decrypt() on the same bytes to learn the AES key the receiver will derive.
//
// Usage: fakesender <receiver-ip> [options]
//   --port N            RTSP port (default 7000)
//   --name NAME         device name shown by the receiver (default "Fake iPhone (test sender)")
//   --seconds N         stream duration (default 20)
//   --fps N             frame rate (default 30)
//   --size WxH          picture size (default 1280x720)
//   --image FILE        mirror a picture (PNG/JPEG...) scaled to the picture width, instead of the counter
//   --scroll N          scroll a picture taller than the screen by N pixels per second
//   --pause-at S --pause-for S   stop sending video at second S for S seconds (screen idle)
//   --rotate-at S       swap width/height at second S (new SPS/PPS + IDR), like rotating the phone
//   --no-audio          do not set up the audio stream
//   --audio-file FILE   play an audio file (any format FFmpeg reads) instead of a tone
//   --corrupt-every N   send every Nth audio frame damaged (the receiver must reject it, not crash)
//   --sessions N        run N sessions back to back (tests reconnects)
//   --speaker           AirPlay speaker instead of mirroring: ALAC audio with metadata and progress
//     --title T --artist A --album A   metadata (default: the audio file's tags, or test values)
//     --cover-art FILE  cover art (default: the audio file's embedded picture)
//     --start-at S      start S seconds into the audio file
//   --cast-url URL      AirPlay video of a direct media URL (like Safari)
//   --cast-hls URL      AirPlay video of an HLS master playlist relayed over FCUP (like YouTube)
//     --start-at S      start position; --just-play: no seek/pause/resume test
//     --separate-audio  (HLS) keep only audio renditions with their own playlist
//   --discover          only browse mDNS for _airplay._tcp and print what answers

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <plist/plist.h>
#include <wels/codec_api.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include "crypto.h"
#include "logger.h"
#include "mirror_buffer.h"
#include "playfair.h"
}

#pragma comment(lib, "ws2_32.lib")

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- utilities

static double g_qpcFreq = 0;
static LARGE_INTEGER g_qpcStart;

static double Seconds()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_qpcStart.QuadPart) / g_qpcFreq;
}

// the sender's "boot clock" in NTP form (seconds since 1900, as iOS reports it)
static constexpr uint64_t kNtpEpochOffset = 2208988800ULL;
static constexpr double kClockBase = 100000.0;  // pretend the phone booted ~28 h ago

static uint64_t NtpNow(bool include1900)
{
    double s = Seconds() + kClockBase + (include1900 ? (double) kNtpEpochOffset : 0.0);
    uint64_t secs = (uint64_t) s;
    uint64_t frac = (uint64_t) ((s - (double) secs) * 4294967296.0);
    return (secs << 32) | frac;
}

static void PutBE16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t) v; }
static void PutBE32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t) (v >> (24 - 8 * i)); }
static void PutBE64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (56 - 8 * i)); }
static void PutLE32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void PutLE64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static void PutFloat(uint8_t* p, float v) { memcpy(p, &v, 4); }

static Bytes RandomBytes(size_t n)
{
    static std::mt19937_64 rng(std::random_device{}());
    Bytes b(n);
    for (auto& x : b) x = (uint8_t) rng();
    return b;
}

static Bytes Sha512Prefix(const char* salt, const uint8_t* data, size_t len, size_t outLen)
{
    uint8_t hash[64];
    sha_ctx_t* ctx = sha_init();
    sha_update(ctx, (const uint8_t*) salt, (int) strlen(salt));
    sha_update(ctx, data, (int) len);
    sha_final(ctx, hash, nullptr);
    sha_destroy(ctx);
    return Bytes(hash, hash + outLen);
}

static std::string PlistToBin(plist_t node)
{
    char* bin = nullptr;
    uint32_t len = 0;
    plist_to_bin(node, &bin, &len);
    std::string s(bin, len);
    plist_mem_free(bin);
    return s;
}

static uint64_t PlistUint(plist_t dict, const char* key)
{
    uint64_t v = 0;
    plist_t n = plist_dict_get_item(dict, key);
    if (n) plist_get_uint_val(n, &v);
    return v;
}

// ---------------------------------------------------------------- RTSP client

struct Response
{
    int status = 0;
    std::map<std::string, std::string> headers;
    Bytes body;
};

class Rtsp
{
public:
    bool Connect(const std::string& ip, int port)
    {
        m_ip = ip;
        m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short) port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        if (connect(m_sock, (sockaddr*) &a, sizeof(a)) != 0)
        {
            printf("connect to %s:%d failed (%d)\n", ip.c_str(), port, WSAGetLastError());
            return false;
        }
        DWORD timeout = 10000;
        setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout));
        sockaddr_in local{};
        int len = sizeof(local);
        getsockname(m_sock, (sockaddr*) &local, &len);
        char buf[64];
        inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
        m_localIp = buf;
        return true;
    }

    void Close()
    {
        if (m_sock != INVALID_SOCKET) closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }

    const std::string& LocalIp() const { return m_localIp; }
    SOCKET Socket() const { return m_sock; }
    void SetHttpSession(const std::string& id) { m_sessionId = id; }

    bool Request(const std::string& method, const std::string& uri, const std::string& contentType,
                 const std::string& body, Response& resp, bool quiet = false)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string req;
        if (m_sessionId.empty())
        {
            req = method + " " + uri + " RTSP/1.0\r\n";
            req += "CSeq: " + std::to_string(m_cseq++) + "\r\n";
        }
        else
        {
            // AirPlay video requests are plain HTTP/1.1, identified by the session id instead of CSeq
            req = method + " " + uri + " HTTP/1.1\r\n";
            req += "X-Apple-Session-ID: " + m_sessionId + "\r\n";
        }
        req += "X-Apple-ProtocolVersion: 1\r\n";
        req += "User-Agent: AirPlay/980.77.1\r\n";
        req += "DACP-ID: 0123456789ABCDEF\r\n";
        req += "Active-Remote: 1234567890\r\n";
        req += "X-Apple-Device-ID: 0x" + std::string("aabbccddeeff") + "\r\n";
        if (!contentType.empty()) req += "Content-Type: " + contentType + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        req += body;
        if (send(m_sock, req.data(), (int) req.size(), 0) != (int) req.size()) return false;

        // read headers
        std::string head;
        char c;
        while (head.size() < 65536)
        {
            int r = recv(m_sock, &c, 1, 0);
            if (r <= 0)
            {
                printf("%s %s: connection closed while reading the response (%d)\n", method.c_str(), uri.c_str(),
                       WSAGetLastError());
                return false;
            }
            head.push_back(c);
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        resp = Response{};
        size_t sp = head.find(' ');
        resp.status = atoi(head.c_str() + sp + 1);
        size_t pos = head.find("\r\n") + 2;
        while (pos < head.size())
        {
            size_t end = head.find("\r\n", pos);
            if (end == std::string::npos || end == pos) break;
            std::string line = head.substr(pos, end - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                std::string k = line.substr(0, colon), v = line.substr(colon + 1);
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                for (auto& ch : k) ch = (char) tolower(ch);
                resp.headers[k] = v;
            }
            pos = end + 2;
        }
        size_t clen = resp.headers.count("content-length") ? (size_t) atoi(resp.headers["content-length"].c_str()) : 0;
        resp.body.resize(clen);
        size_t got = 0;
        while (got < clen)
        {
            int r = recv(m_sock, (char*) resp.body.data() + got, (int) (clen - got), 0);
            if (r <= 0) return false;
            got += r;
        }
        if (!quiet) printf("  %-14s %-40s -> %d (%zu bytes)\n", method.c_str(), uri.c_str(), resp.status, clen);
        return true;
    }

private:
    SOCKET m_sock = INVALID_SOCKET;
    std::string m_ip, m_localIp, m_sessionId;
    int m_cseq = 0;
    std::mutex m_mutex;
};

// ---------------------------------------------------------------- NTP timing responder

class TimingServer
{
public:
    int Start()
    {
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        bind(m_sock, (sockaddr*) &a, sizeof(a));
        int len = sizeof(a);
        getsockname(m_sock, (sockaddr*) &a, &len);
        DWORD timeout = 200;
        setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout));
        m_run = true;
        m_thread = std::thread([this] { Loop(); });
        return ntohs(a.sin_port);
    }
    void Stop()
    {
        m_run = false;
        if (m_thread.joinable()) m_thread.join();
        closesocket(m_sock);
    }
    int Requests() const { return m_requests; }

private:
    void Loop()
    {
        uint8_t buf[128];
        while (m_run)
        {
            sockaddr_in from{};
            int flen = sizeof(from);
            int n = recvfrom(m_sock, (char*) buf, sizeof(buf), 0, (sockaddr*) &from, &flen);
            if (n < 32) continue;
            uint64_t recvTime = NtpNow(true);
            uint8_t out[32] = {0x80, 0xd3, 0x00, 0x07};
            memcpy(out + 8, buf + 24, 8);  // originate = the receiver's transmit time
            PutBE64(out + 16, recvTime);
            PutBE64(out + 24, NtpNow(true));
            sendto(m_sock, (const char*) out, sizeof(out), 0, (sockaddr*) &from, flen);
            m_requests++;
        }
    }
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_run{false};
    std::atomic<int> m_requests{0};
    std::thread m_thread;
};

// ---------------------------------------------------------------- synthetic video

static const uint8_t kSegments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

static void FillRect(uint8_t* y, int stride, int w, int h, int x0, int y0, int rw, int rh, uint8_t v)
{
    for (int yy = std::max(0, y0); yy < std::min(h, y0 + rh); yy++)
        memset(y + yy * stride + std::max(0, x0), v, std::max(0, std::min(w, x0 + rw) - std::max(0, x0)));
}

static void DrawDigit(uint8_t* y, int stride, int w, int h, int x, int top, int d, int scale)
{
    int sw = 10 * scale, sl = 50 * scale;  // segment thickness and length
    uint8_t m = kSegments[d];
    if (m & 0x01) FillRect(y, stride, w, h, x + sw, top, sl, sw, 235);                 // a
    if (m & 0x02) FillRect(y, stride, w, h, x + sw + sl, top + sw, sw, sl, 235);       // b
    if (m & 0x04) FillRect(y, stride, w, h, x + sw + sl, top + 2 * sw + sl, sw, sl, 235); // c
    if (m & 0x08) FillRect(y, stride, w, h, x + sw, top + 2 * sw + 2 * sl, sl, sw, 235);  // d
    if (m & 0x10) FillRect(y, stride, w, h, x, top + 2 * sw + sl, sw, sl, 235);        // e
    if (m & 0x20) FillRect(y, stride, w, h, x, top + sw, sw, sl, 235);                 // f
    if (m & 0x40) FillRect(y, stride, w, h, x + sw, top + sw + sl, sl, sw, 235);       // g
}

// I420 test picture: dark background, a bar sweeping across, and the frame number in big digits
static void DrawFrame(std::vector<uint8_t>& yuv, int w, int h, int frame, int fps)
{
    uint8_t* Y = yuv.data();
    uint8_t* U = Y + w * h;
    uint8_t* V = U + (w / 2) * (h / 2);
    memset(Y, 40, w * h);
    memset(U, 150, (w / 2) * (h / 2));
    memset(V, 110, (w / 2) * (h / 2));
    int barX = (frame * 8) % w;
    FillRect(Y, w, w, h, barX, 0, 24, h, 200);
    int scale = std::max(1, std::min(w, h) / 360);
    std::string s = std::to_string(frame);
    int digitW = 80 * scale;
    int x = (w - (int) s.size() * digitW) / 2, top = h / 2 - 60 * scale;
    for (char ch : s)
    {
        DrawDigit(Y, w, w, h, x, top, ch - '0', scale);
        x += digitW;
    }
    // seconds marker blocks along the bottom
    int secs = frame / fps;
    for (int i = 0; i < (secs % 20); i++) FillRect(Y, w, w, h, 20 + i * 30 * scale, h - 40 * scale, 20 * scale, 20 * scale, 180);
}

struct H264Encoder
{
    ISVCEncoder* enc = nullptr;
    int w = 0, h = 0;
    bool Init(int width, int height, int fps, int bitrate = 4000000)
    {
        Close();
        w = width;
        h = height;
        if (WelsCreateSVCEncoder(&enc) != 0) return false;
        SEncParamExt p;
        enc->GetDefaultParams(&p);
        p.iUsageType = SCREEN_CONTENT_REAL_TIME;
        p.iPicWidth = w;
        p.iPicHeight = h;
        p.iTargetBitrate = bitrate;
        p.iMaxBitrate = bitrate * 2;
        p.iRCMode = RC_BITRATE_MODE;
        p.fMaxFrameRate = (float) fps;
        p.iSpatialLayerNum = 1;
        p.iTemporalLayerNum = 1;
        p.uiIntraPeriod = 0;  // like iOS: an IDR only at the start (and after a format change)
        p.bEnableFrameSkip = false;
        p.iMultipleThreadIdc = 1;
        p.eSpsPpsIdStrategy = CONSTANT_ID;
        p.sSpatialLayers[0].iVideoWidth = w;
        p.sSpatialLayers[0].iVideoHeight = h;
        p.sSpatialLayers[0].fFrameRate = (float) fps;
        p.sSpatialLayers[0].iSpatialBitrate = bitrate;
        p.sSpatialLayers[0].iMaxSpatialBitrate = bitrate * 2;
        p.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
        p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
        // BT.709, limited range, as the pictures are converted (and as iOS signals it)
        p.sSpatialLayers[0].bVideoSignalTypePresent = true;
        p.sSpatialLayers[0].uiVideoFormat = VF_UNDEF;
        p.sSpatialLayers[0].bFullRange = false;
        p.sSpatialLayers[0].bColorDescriptionPresent = true;
        p.sSpatialLayers[0].uiColorPrimaries = CP_BT709;
        p.sSpatialLayers[0].uiTransferCharacteristics = TRC_BT709;
        p.sSpatialLayers[0].uiColorMatrix = CM_BT709;
        return enc->InitializeExt(&p) == 0;
    }
    // returns Annex-B NAL units of one access unit
    std::vector<Bytes> Encode(std::vector<uint8_t>& yuv, int64_t tsMs)
    {
        SSourcePicture pic{};
        pic.iColorFormat = videoFormatI420;
        pic.iPicWidth = w;
        pic.iPicHeight = h;
        pic.iStride[0] = w;
        pic.iStride[1] = pic.iStride[2] = w / 2;
        pic.pData[0] = yuv.data();
        pic.pData[1] = yuv.data() + w * h;
        pic.pData[2] = pic.pData[1] + (w / 2) * (h / 2);
        pic.uiTimeStamp = tsMs;
        SFrameBSInfo info{};
        std::vector<Bytes> nals;
        if (enc->EncodeFrame(&pic, &info) != cmResultSuccess || info.eFrameType == videoFrameTypeSkip) return nals;
        for (int l = 0; l < info.iLayerNum; l++)
        {
            const SLayerBSInfo& layer = info.sLayerInfo[l];
            uint8_t* p = layer.pBsBuf;
            for (int n = 0; n < layer.iNalCount; n++)
            {
                int len = layer.pNalLengthInByte[n];
                int sc = (p[2] == 1) ? 3 : 4;  // strip the start code
                nals.emplace_back(p + sc, p + len);
                p += len;
            }
        }
        return nals;
    }
    void Close()
    {
        if (enc)
        {
            enc->Uninitialize();
            WelsDestroySVCEncoder(enc);
            enc = nullptr;
        }
    }
};

// A picture (anything WIC decodes) scaled to a given width, as I420 rows
struct Picture
{
    int w = 0, h = 0;
    std::vector<uint8_t> yuv;  // w x h I420

    bool Load(const std::string& path, int width)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* wic = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;
        std::wstring wpath(path.begin(), path.end());
        bool ok = false;
        UINT iw = 0, ih = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&iw, &ih)) && iw && ih)
        {
            w = width & ~1;
            h = (int) ((uint64_t) ih * w / iw) & ~1;
            std::vector<uint8_t> bgra((size_t) w * h * 4);
            ok = SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                 SUCCEEDED(scaler->Initialize(frame, w, h, WICBitmapInterpolationModeHighQualityCubic)) &&
                 SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                 SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom)) &&
                 SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT) bgra.size(), bgra.data()));
            if (ok) ToI420(bgra);
        }
        for (IUnknown* u : {(IUnknown*) conv, (IUnknown*) scaler, (IUnknown*) frame, (IUnknown*) dec, (IUnknown*) wic})
            if (u) u->Release();
        if (!ok) printf("  cannot read picture %s\n", path.c_str());
        return ok;
    }

    // BT.709 limited range
    void ToI420(const std::vector<uint8_t>& bgra)
    {
        yuv.assign((size_t) w * h * 3 / 2, 0);
        uint8_t* Y = yuv.data();
        uint8_t* U = Y + w * h;
        uint8_t* V = U + (w / 2) * (h / 2);
        auto px = [&](int x, int y, int c) { return (double) bgra[((size_t) y * w + x) * 4 + c]; };
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                Y[y * w + x] = (uint8_t) lround(16 + (0.2126 * px(x, y, 2) + 0.7152 * px(x, y, 1) + 0.0722 * px(x, y, 0)) * 219 / 255);
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w / 2; x++)
            {
                double r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                    {
                        r += px(2 * x + dx, 2 * y + dy, 2) / 4;
                        g += px(2 * x + dx, 2 * y + dy, 1) / 4;
                        b += px(2 * x + dx, 2 * y + dy, 0) / 4;
                    }
                U[y * (w / 2) + x] = (uint8_t) lround(128 + (-0.1146 * r - 0.3854 * g + 0.5 * b) * 224 / 255);
                V[y * (w / 2) + x] = (uint8_t) lround(128 + (0.5 * r - 0.4542 * g - 0.0458 * b) * 224 / 255);
            }
    }

    // copies the rows starting at `top` into a w x frameH I420 frame (black below the picture)
    void Draw(std::vector<uint8_t>& out, int frameH, int top) const
    {
        uint8_t* Y = out.data();
        uint8_t* U = Y + w * frameH;
        uint8_t* V = U + (w / 2) * (frameH / 2);
        memset(Y, 16, (size_t) w * frameH);
        memset(U, 128, (size_t) (w / 2) * (frameH / 2));
        memset(V, 128, (size_t) (w / 2) * (frameH / 2));
        top &= ~1;
        int rows = std::max(0, std::min(frameH, h - top));
        const uint8_t* sY = yuv.data();
        const uint8_t* sU = sY + w * h;
        const uint8_t* sV = sU + (w / 2) * (h / 2);
        memcpy(Y, sY + (size_t) top * w, (size_t) rows * w);
        memcpy(U, sU + (size_t) (top / 2) * (w / 2), (size_t) (rows / 2) * (w / 2));
        memcpy(V, sV + (size_t) (top / 2) * (w / 2), (size_t) (rows / 2) * (w / 2));
    }
};

// ---------------------------------------------------------------- audio input

// An audio file decoded with FFmpeg to 44.1 kHz 16-bit stereo, with its tags and embedded picture
struct AudioFile
{
    std::vector<int16_t> pcm;  // interleaved stereo
    std::string title, artist, album, picture;
    size_t pos = 0;            // next stereo sample to play

    double Duration() const { return pcm.size() / 2 / 44100.0; }

    bool Load(const std::string& path)
    {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        {
            printf("  cannot open %s\n", path.c_str());
            return false;
        }
        avformat_find_stream_info(fmt, nullptr);
        auto tag = [&](const char* key) {
            AVDictionaryEntry* e = av_dict_get(fmt->metadata, key, nullptr, 0);
            return e ? std::string(e->value) : std::string();
        };
        title = tag("title");
        artist = tag("artist");
        album = tag("album");
        for (unsigned i = 0; i < fmt->nb_streams; i++)
            if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
                picture.assign((const char*) fmt->streams[i]->attached_pic.data, fmt->streams[i]->attached_pic.size);

        int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        const AVCodec* codec = si >= 0 ? avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id) : nullptr;
        AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
        SwrContext* swr = nullptr;
        AVChannelLayout stereo{};
        av_channel_layout_default(&stereo, 2);
        bool ok = dec && avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar) >= 0 &&
                  avcodec_open2(dec, codec, nullptr) >= 0;
        if (ok && dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&dec->ch_layout, dec->ch_layout.nb_channels);
        ok = ok && swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 44100, &dec->ch_layout, dec->sample_fmt,
                                       dec->sample_rate, 0, nullptr) >= 0 && swr_init(swr) >= 0;
        if (ok)
        {
            AVPacket* pkt = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            auto convert = [&](const uint8_t** in, int count) {
                int room = swr_get_out_samples(swr, count);
                size_t at = pcm.size();
                pcm.resize(at + (size_t) room * 2);
                uint8_t* out = (uint8_t*) (pcm.data() + at);
                int n = swr_convert(swr, &out, room, in, count);
                pcm.resize(at + (size_t) std::max(n, 0) * 2);
            };
            auto drain = [&] {
                while (avcodec_receive_frame(dec, frame) == 0) convert((const uint8_t**) frame->extended_data, frame->nb_samples);
            };
            while (av_read_frame(fmt, pkt) >= 0)
            {
                if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) == 0) drain();
                av_packet_unref(pkt);
            }
            avcodec_send_packet(dec, nullptr);
            drain();
            convert(nullptr, 0);
            av_frame_free(&frame);
            av_packet_free(&pkt);
        }
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        if (pcm.empty())
        {
            printf("  no audio decoded from %s\n", path.c_str());
            return false;
        }
        printf("  audio file: %.1f s, \"%s\" / \"%s\" / \"%s\"%s\n", Duration(), title.c_str(), artist.c_str(), album.c_str(),
               picture.empty() ? "" : ", with a picture");
        return true;
    }

    // next `samples` stereo samples, looping at the end
    void Read(int16_t* out, int samples)
    {
        size_t total = pcm.size() / 2;
        for (int i = 0; i < samples; i++)
        {
            out[2 * i] = pcm[2 * pos];
            out[2 * i + 1] = pcm[2 * pos + 1];
            pos = (pos + 1) % total;
        }
    }
};

// Apple Lossless frames of 352 16-bit stereo samples (AirPlay's format), written as "escape"
// frames: the samples are stored uncompressed, which is valid ALAC for any decoder and needs no
// encoder library. (FFmpeg's ALAC encoder only makes 4096-sample frames.)
struct AlacFrames
{
    static constexpr int kSamples = 352;
    int Encode(const int16_t* pcm, uint8_t* out)
    {
        size_t bit = 0;
        auto put = [&](uint32_t v, int n) {
            for (int i = n - 1; i >= 0; i--, bit++)
            {
                if ((bit & 7) == 0) out[bit >> 3] = 0;
                if ((v >> i) & 1) out[bit >> 3] |= (uint8_t) (0x80 >> (bit & 7));
            }
        };
        put(1, 3);   // element: channel pair
        put(0, 4);   // element instance
        put(0, 12);  // unused
        put(0, 1);   // no explicit sample count: a full frame
        put(0, 2);   // no shifted bytes
        put(1, 1);   // escape: uncompressed samples follow, interleaved
        for (int i = 0; i < kSamples * 2; i++) put((uint16_t) pcm[i], 16);
        put(7, 3);   // end of frame
        return (int) ((bit + 7) / 8);
    }
};

// ---------------------------------------------------------------- mDNS browse

static void Discover()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(s, (sockaddr*) &local, sizeof(local));
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout));
    // query: _airplay._tcp.local PTR, QU bit set (unicast response)
    uint8_t q[64] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    int n = 12;
    for (const char* label : {"_airplay", "_tcp", "local"})
    {
        q[n++] = (uint8_t) strlen(label);
        memcpy(q + n, label, strlen(label));
        n += (int) strlen(label);
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 12;    // PTR
    q[n++] = 0x80; q[n++] = 1;  // QU, IN
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    sendto(s, (const char*) q, n, 0, (sockaddr*) &to, sizeof(to));
    printf("mDNS query for _airplay._tcp.local sent; answers:\n");
    uint8_t buf[2048];
    double end = Seconds() + 3;
    while (Seconds() < end)
    {
        sockaddr_in from{};
        int flen = sizeof(from);
        int r = recvfrom(s, (char*) buf, sizeof(buf), 0, (sockaddr*) &from, &flen);
        if (r <= 0) break;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        // print printable runs (names, TXT keys) for a quick look
        std::string text;
        for (int i = 12; i < r; i++) text += (buf[i] >= 32 && buf[i] < 127) ? (char) buf[i] : '.';
        printf("  from %s (%d bytes): %s\n", ip, r, text.c_str());
    }
    closesocket(s);
}

// ---------------------------------------------------------------- the session

struct Options
{
    std::string ip;
    std::string name = "Fake iPhone (test sender)";  // device name the receiver shows
    int port = 7000;
    double seconds = 20;
    int fps = 30;
    int width = 1280, height = 720;
    double pauseAt = -1, pauseFor = 0;
    double rotateAt = -1;
    bool audio = true;
    int sessions = 1;
    bool speaker = false;          // audio-only ALAC stream with metadata (AirPlay speaker)
    std::string castUrl;           // AirPlay video: a direct media URL (Safari style)
    std::string castHls;           // AirPlay video: an HLS master playlist relayed like YouTube does
    std::string coverArt;          // image file sent as cover art in speaker mode
    std::string image;             // picture mirrored instead of the frame counter
    double scroll = 0;             // pixels per second the picture scrolls by
    std::string audioFile;         // audio played instead of a tone
    std::string title, artist, album;  // speaker metadata (default: the audio file's tags)
    double startAt = 0;            // speaker: start this many seconds into the audio file; casting: start position
    bool justPlay = false;         // casting: no seek/pause/resume test
    int corruptEvery = 0;          // damage every Nth audio frame (decoder robustness)
    bool separateAudio = false;    // casting: drop muxed audio renditions from the master playlist
};

static bool SendAll(SOCKET s, const uint8_t* p, size_t n)
{
    while (n)
    {
        int r = send(s, (const char*) p, (int) n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

struct Keys
{
    uint8_t aeskey[16] = {};  // as derived by the receiver (FairPlay key hashed with the ECDH secret)
    Bytes eiv;
    std::string url;          // rtsp://<receiver>/<session>
};

// GET /info, pair-setup, pair-verify, fp-setup x2, SETUP (keys + timing), RECORD
static int Handshake(Rtsp& rtsp, const Options& o, bool screen, TimingServer& timing, Keys& k)
{
    Response r;
    {
        plist_t root = plist_new_dict();
        plist_t arr = plist_new_array();
        plist_array_append_item(arr, plist_new_string("txtAirPlay"));
        plist_dict_set_item(root, "qualifier", arr);
        std::string body = PlistToBin(root);
        plist_free(root);
        if (!rtsp.Request("GET", "/info", "application/x-apple-binary-plist", body, r) || r.status != 200) return 2;
    }
    if (!rtsp.Request("GET", "/info", "", "", r) || r.status != 200) return 2;
    {
        plist_t info = nullptr;
        plist_from_bin((const char*) r.body.data(), (uint32_t) r.body.size(), &info);
        char* name = nullptr;
        plist_t n = plist_dict_get_item(info, "name");
        if (n) plist_get_string_val(n, &name);
        printf("  receiver name: %s, features 0x%llx\n", name ? name : "?", (unsigned long long) PlistUint(info, "features"));
        if (name) plist_mem_free(name);
        plist_free(info);
    }

    // pairing (legacy "transient" pair-setup + pair-verify)
    int result = 0;
    ed25519_key_t* edOurs = ed25519_key_generate("fakesender-device", "", &result);
    uint8_t edOursRaw[32];
    ed25519_key_get_raw(edOursRaw, edOurs);
    if (!rtsp.Request("POST", "/pair-setup", "application/octet-stream", std::string((char*) edOursRaw, 32), r) ||
        r.body.size() != 32)
        return 3;
    ed25519_key_t* edTheirs = ed25519_key_from_raw(r.body.data());

    x25519_key_t* xOurs = x25519_key_generate();
    uint8_t xOursRaw[32];
    x25519_key_get_raw(xOursRaw, xOurs);
    std::string pv1 = std::string("\x01\x00\x00\x00", 4) + std::string((char*) xOursRaw, 32) + std::string((char*) edOursRaw, 32);
    if (!rtsp.Request("POST", "/pair-verify", "application/octet-stream", pv1, r) || r.body.size() != 96) return 4;
    x25519_key_t* xTheirs = x25519_key_from_raw(r.body.data());
    uint8_t secret[32];
    x25519_derive_secret(secret, xOurs, xTheirs);
    Bytes key = Sha512Prefix("Pair-Verify-AES-Key", secret, 32, 16);
    Bytes iv = Sha512Prefix("Pair-Verify-AES-IV", secret, 32, 16);
    aes_ctx_t* ctr = aes_ctr_init(key.data(), iv.data());
    uint8_t theirSig[64];
    aes_ctr_encrypt(ctr, r.body.data() + 32, theirSig, 64);
    uint8_t msg[64];
    memcpy(msg, r.body.data(), 32);
    memcpy(msg + 32, xOursRaw, 32);
    if (!ed25519_verify(theirSig, 64, msg, 64, edTheirs)) printf("  WARNING: receiver's pair-verify signature did not verify\n");
    memcpy(msg, xOursRaw, 32);
    memcpy(msg + 32, r.body.data(), 32);
    uint8_t ourSig[64], encSig[64];
    ed25519_sign(ourSig, 64, msg, 64, edOurs);
    aes_ctr_encrypt(ctr, ourSig, encSig, 64);
    aes_ctr_destroy(ctr);
    std::string pv2 = std::string("\x00\x00\x00\x00", 4) + std::string((char*) encSig, 64);
    if (!rtsp.Request("POST", "/pair-verify", "application/octet-stream", pv2, r) || r.status != 200) return 5;

    // FairPlay
    uint8_t fp1[16] = {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0xbb};
    if (!rtsp.Request("POST", "/fp-setup", "application/octet-stream", std::string((char*) fp1, 16), r) || r.body.size() != 142)
        return 6;
    Bytes keymsg = RandomBytes(164);
    const uint8_t fp2hdr[12] = {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x98};
    memcpy(keymsg.data(), fp2hdr, 12);
    keymsg[12] = 0x00;  // FairPlay mode (0-3); must match the mode of the first fp-setup message
    if (!rtsp.Request("POST", "/fp-setup", "application/octet-stream", std::string((char*) keymsg.data(), 164), r) ||
        r.body.size() != 32)
        return 7;
    Bytes ekey = RandomBytes(72);
    k.eiv = RandomBytes(16);
    {
        Bytes km = keymsg, ek = ekey;  // playfair_decrypt may scribble on its inputs
        playfair_decrypt(km.data(), ek.data(), k.aeskey);
    }
    {
        uint8_t hash[64];
        sha_ctx_t* ctx = sha_init();
        sha_update(ctx, k.aeskey, 16);
        sha_update(ctx, secret, 32);
        sha_final(ctx, hash, nullptr);
        sha_destroy(ctx);
        memcpy(k.aeskey, hash, 16);
    }

    int timingPort = timing.Start();
    k.url = "rtsp://" + o.ip + "/" + std::to_string(1000000000ULL + (uint64_t) (Seconds() * 1000));
    {
        plist_t root = plist_new_dict();
        plist_dict_set_item(root, "ekey", plist_new_data((const char*) ekey.data(), 72));
        plist_dict_set_item(root, "eiv", plist_new_data((const char*) k.eiv.data(), 16));
        plist_dict_set_item(root, "deviceID", plist_new_string("AA:BB:CC:DD:EE:FF"));
        plist_dict_set_item(root, "macAddress", plist_new_string("AA:BB:CC:DD:EE:FF"));
        plist_dict_set_item(root, "model", plist_new_string("iPhone14,3"));
        plist_dict_set_item(root, "name", plist_new_string(o.name.c_str()));
        plist_dict_set_item(root, "osName", plist_new_string("iPhone OS"));
        plist_dict_set_item(root, "sourceVersion", plist_new_string("980.77.1"));
        plist_dict_set_item(root, "timingProtocol", plist_new_string("NTP"));
        plist_dict_set_item(root, "timingPort", plist_new_uint(timingPort));
        if (screen) plist_dict_set_item(root, "isScreenMirroringSession", plist_new_bool(1));
        plist_dict_set_item(root, "sessionUUID", plist_new_string("8D2B4AD1-5C4F-4F69-9A85-0E6A1D7B9C11"));
        std::string body = PlistToBin(root);
        plist_free(root);
        if (!rtsp.Request("SETUP", k.url, "application/x-apple-binary-plist", body, r) || r.status != 200) return 8;
    }
    if (!rtsp.Request("RECORD", k.url, "", "", r) || r.status != 200) return 9;
    return 0;
}

// SETUP of one audio stream; returns the receiver's data and control ports
static int SetupAudio(Rtsp& rtsp, const Keys& k, int ct, int spf, uint64_t audioFormat, bool usingScreen, SOCKET sock,
                      uint16_t& dataPort, uint16_t& controlPort)
{
    sockaddr_in a{};
    int alen = sizeof(a);
    getsockname(sock, (sockaddr*) &a, &alen);
    plist_t root = plist_new_dict();
    plist_t streams = plist_new_array();
    plist_t s = plist_new_dict();
    plist_dict_set_item(s, "type", plist_new_uint(96));
    plist_dict_set_item(s, "ct", plist_new_uint(ct));
    plist_dict_set_item(s, "spf", plist_new_uint(spf));
    plist_dict_set_item(s, "sr", plist_new_uint(44100));
    plist_dict_set_item(s, "audioFormat", plist_new_uint(audioFormat));
    plist_dict_set_item(s, "controlPort", plist_new_uint(ntohs(a.sin_port)));
    plist_dict_set_item(s, "isMedia", plist_new_bool(1));
    plist_dict_set_item(s, "usingScreen", plist_new_bool(usingScreen ? 1 : 0));
    plist_array_append_item(streams, s);
    plist_dict_set_item(root, "streams", streams);
    std::string body = PlistToBin(root);
    plist_free(root);
    Response r;
    if (!rtsp.Request("SETUP", k.url, "application/x-apple-binary-plist", body, r) || r.status != 200) return 11;
    plist_t resp = nullptr;
    plist_from_bin((const char*) r.body.data(), (uint32_t) r.body.size(), &resp);
    plist_t rs = plist_array_get_item(plist_dict_get_item(resp, "streams"), 0);
    dataPort = (uint16_t) PlistUint(rs, "dataPort");
    controlPort = (uint16_t) PlistUint(rs, "controlPort");
    plist_free(resp);
    printf("  audio (ct=%d) data port %u, control port %u\n", ct, dataPort, controlPort);
    return 0;
}

static void Teardown(Rtsp& rtsp, const Keys& k, int streamType)
{
    Response r;
    plist_t root = plist_new_dict();
    plist_t streams = plist_new_array();
    plist_t s = plist_new_dict();
    plist_dict_set_item(s, "type", plist_new_uint(streamType));
    plist_array_append_item(streams, s);
    plist_dict_set_item(root, "streams", streams);
    std::string body = PlistToBin(root);
    plist_free(root);
    rtsp.Request("TEARDOWN", k.url, "application/x-apple-binary-plist", body, r);
}

static constexpr uint32_t kFirstRtp = 50000;  // RTP timestamp of the first audio frame

// Sends RTP audio packets (AES-CBC encrypted like iOS does) plus a sync packet every second.
// encode(frameIndex, out) fills one compressed frame and returns its size (0 = skip).
static void StreamAudio(const Options& o, const Keys& k, SOCKET sock, uint16_t dataPort, uint16_t controlPort, int spf,
                        std::atomic<bool>& running, const std::function<int(int, uint8_t*)>& encode)
{
    aes_ctx_t* cbc = aes_cbc_init(k.aeskey, k.eiv.data(), AES_ENCRYPT);
    sockaddr_in dst{}, ctl{};
    dst.sin_family = ctl.sin_family = AF_INET;
    inet_pton(AF_INET, o.ip.c_str(), &dst.sin_addr);
    ctl.sin_addr = dst.sin_addr;
    dst.sin_port = htons(dataPort);
    ctl.sin_port = htons(controlPort);
    uint16_t seq = 1000;
    uint32_t rtp = kFirstRtp;
    double start = Seconds();
    int frames = 0, corrupted = 0;
    double nextSync = 0;
    while (running)
    {
        double now = Seconds() - start;
        if (now >= nextSync)
        {
            uint8_t sync[20] = {(uint8_t) (frames ? 0x80 : 0x90), 0xd4, 0x00, 0x04};
            PutBE32(sync + 4, rtp);
            PutBE64(sync + 8, NtpNow(true));
            PutBE32(sync + 16, rtp + 7497);
            sendto(sock, (const char*) sync, 20, 0, (sockaddr*) &ctl, sizeof(ctl));
            nextSync = now + 1.0;
        }
        if (now < frames * (double) spf / 44100.0)
        {
            Sleep(2);
            continue;
        }
        uint8_t frame[4096];
        int len = encode(frames, frame);
        if (len > 0 && o.corruptEvery > 0 && frames % o.corruptEvery == o.corruptEvery - 1)
        {
            // damaged frames: alternately an ALAC header claiming 4 billion samples, and random bytes
            Bytes junk = RandomBytes(len);
            memcpy(frame, junk.data(), len);
            if ((frames / o.corruptEvery) % 2 == 0)
            {
                const uint8_t huge[] = {0x20, 0x00, 0x13, 0xFF, 0xFF, 0xFF, 0xFE};  // CPE, partial frame (0xFFFFFFFF samples), escape
                memcpy(frame, huge, sizeof(huge));
            }
            corrupted++;
        }
        if (len > 0)
        {
            uint8_t pkt[4096 + 12];
            pkt[0] = 0x80;
            pkt[1] = frames == 0 ? 0xe0 : 0x60;
            PutBE16(pkt + 2, seq++);
            PutBE32(pkt + 4, rtp);
            PutBE32(pkt + 8, 0x12345678);
            int enclen = len / 16 * 16;
            aes_cbc_encrypt(cbc, frame, pkt + 12, enclen);
            aes_cbc_reset(cbc);
            memcpy(pkt + 12 + enclen, frame + enclen, len - enclen);
            sendto(sock, (const char*) pkt, 12 + len, 0, (sockaddr*) &dst, sizeof(dst));
        }
        rtp += spf;
        frames++;
    }
    aes_cbc_destroy(cbc);
    if (corrupted) printf("  audio: %d frames sent, %d of them damaged on purpose\n", frames, corrupted);
    else printf("  audio: %d frames sent\n", frames);
}

static void Tone(int16_t* pcm, int samples, double& phase, double freq, bool on)
{
    for (int i = 0; i < samples; i++)
    {
        int16_t v = on ? (int16_t) (8000 * sin(phase)) : 0;
        phase += 2 * 3.14159265358979 * freq / 44100.0;
        pcm[2 * i] = pcm[2 * i + 1] = v;
    }
}

static SOCKET BoundUdpSocket()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    bind(s, (sockaddr*) &a, sizeof(a));
    return s;
}

// ---------------------------------------------------------------- screen mirroring

static int RunMirror(const Options& o, int index)
{
    printf("=== mirroring session %d: %s:%d, %ds, %dx%d @ %d fps%s\n", index, o.ip.c_str(), o.port, (int) o.seconds,
           o.width, o.height, o.fps, o.audio ? ", with audio" : "");
    Rtsp rtsp;
    if (!rtsp.Connect(o.ip, o.port)) return 1;
    TimingServer timing;
    Keys k;
    int rc = Handshake(rtsp, o, true, timing, k);
    if (rc) return rc;
    Response r;

    // SETUP (mirroring stream)
    uint64_t streamConnectionId = (RandomBytes(8)[0] | 1ULL) * 0x0101010101010101ULL & 0x7fffffffffffffffULL;
    uint16_t dataPort = 0;
    {
        plist_t root = plist_new_dict();
        plist_t streams = plist_new_array();
        plist_t s = plist_new_dict();
        plist_dict_set_item(s, "type", plist_new_uint(110));
        plist_dict_set_item(s, "streamConnectionID", plist_new_uint(streamConnectionId));
        plist_dict_set_item(s, "latencyMs", plist_new_uint(75));
        plist_array_append_item(streams, s);
        plist_dict_set_item(root, "streams", streams);
        std::string body = PlistToBin(root);
        plist_free(root);
        if (!rtsp.Request("SETUP", k.url, "application/x-apple-binary-plist", body, r) || r.status != 200) return 10;
        plist_t resp = nullptr;
        plist_from_bin((const char*) r.body.data(), (uint32_t) r.body.size(), &resp);
        plist_t rs = plist_array_get_item(plist_dict_get_item(resp, "streams"), 0);
        dataPort = (uint16_t) PlistUint(rs, "dataPort");
        plist_free(resp);
        printf("  mirroring data port %u\n", dataPort);
    }

    // SETUP (mirroring audio: ALAC, see the note at the top)
    uint16_t audioDataPort = 0, audioControlPort = 0;
    SOCKET audioSock = INVALID_SOCKET;
    AudioFile song;
    if (o.audio)
    {
        if (!o.audioFile.empty() && !song.Load(o.audioFile)) return 13;
        audioSock = BoundUdpSocket();
        if ((rc = SetupAudio(rtsp, k, 2, AlacFrames::kSamples, 0x40000, true, audioSock, audioDataPort, audioControlPort)) != 0)
            return rc;
    }

    SOCKET ms = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(dataPort);
        inet_pton(AF_INET, o.ip.c_str(), &a.sin_addr);
        if (connect(ms, (sockaddr*) &a, sizeof(a)) != 0)
        {
            printf("  connect to mirroring port failed (%d)\n", WSAGetLastError());
            return 12;
        }
    }
    mirror_buffer_t* enc = mirror_buffer_init(nullptr, k.aeskey);
    mirror_buffer_init_aes(enc, &streamConnectionId);

    std::atomic<bool> running{true};
    std::thread feedback([&] {
        Response fr;
        while (running)
        {
            for (int i = 0; i < 20 && running; i++) Sleep(100);
            if (running) rtsp.Request("POST", "/feedback", "", "", fr, true);
        }
    });

    std::thread audioThread;
    if (o.audio)
    {
        audioThread = std::thread([&] {
            AlacFrames alac;
            std::vector<int16_t> pcm(AlacFrames::kSamples * 2);
            double phase = 0;
            StreamAudio(o, k, audioSock, audioDataPort, audioControlPort, AlacFrames::kSamples, running, [&](int frame, uint8_t* out) {
                if (!song.pcm.empty()) song.Read(pcm.data(), AlacFrames::kSamples);
                else Tone(pcm.data(), AlacFrames::kSamples, phase, 440.0, ((frame * AlacFrames::kSamples / 22050) % 2) == 0);  // beep every half second
                return alac.Encode(pcm.data(), out);
            });
        });
    }

    H264Encoder h264;
    int w = o.width, hgt = o.height;
    // a picture is sharp text and edges: give it the bitrate iOS uses for a busy screen
    int bitrate = o.image.empty() ? 4000000 : 12000000;
    Picture picture;
    if (!o.image.empty() && !picture.Load(o.image, w)) return 14;
    h264.Init(w, hgt, o.fps, bitrate);
    std::vector<uint8_t> yuv(w * hgt * 3 / 2);
    bool needCodecPacket = true, rotated = false, paused = false;
    int frame = 0, sentFrames = 0;
    double start = Seconds();
    while (Seconds() - start < o.seconds)
    {
        double t = Seconds() - start;
        if (frame >= (int) (t * o.fps) + 1)
        {
            Sleep(1);
            continue;
        }
        if (o.rotateAt >= 0 && !rotated && t >= o.rotateAt)
        {
            rotated = true;
            std::swap(w, hgt);
            h264.Init(w, hgt, o.fps, bitrate);
            yuv.assign(w * hgt * 3 / 2, 0);
            if (!o.image.empty()) picture.Load(o.image, w);
            needCodecPacket = true;
            printf("  rotated to %dx%d at frame %d\n", w, hgt, frame);
        }
        bool inPause = o.pauseAt >= 0 && t >= o.pauseAt && t < o.pauseAt + o.pauseFor;
        if (inPause)
        {
            if (!paused) printf("  pausing video at frame %d (t=%.1fs)\n", frame, t);
            paused = true;
            frame = (int) (t * o.fps) + 1;  // keep the counter tied to wall time
            Sleep(5);
            continue;
        }
        if (paused)
        {
            printf("  resuming video at frame %d (t=%.1fs)\n", frame, t);
            paused = false;
        }

        if (picture.yuv.empty()) DrawFrame(yuv, w, hgt, frame, o.fps);
        else picture.Draw(yuv, hgt, std::min((int) (t * o.scroll), std::max(0, picture.h - hgt)));
        std::vector<Bytes> nals = h264.Encode(yuv, (int64_t) (t * 1000));
        frame++;
        if (nals.empty()) continue;

        uint64_t ts = NtpNow(false);
        Bytes sps, pps, avcc;
        bool idr = false;
        for (auto& n : nals)
        {
            int type = n[0] & 0x1f;
            if (type == 7) sps = n;
            else if (type == 8) pps = n;
            else
            {
                if (type == 5) idr = true;
                uint8_t len[4];
                PutBE32(len, (uint32_t) n.size());
                avcc.insert(avcc.end(), len, len + 4);
                avcc.insert(avcc.end(), n.begin(), n.end());
            }
        }
        if ((needCodecPacket || idr) && !sps.empty() && !pps.empty())
        {
            Bytes payload = {0x01, sps[1], sps[2], sps[3], 0xff, 0xe1, 0, 0};
            PutBE16(payload.data() + 6, (uint16_t) sps.size());
            payload.insert(payload.end(), sps.begin(), sps.end());
            payload.push_back(0x01);
            uint8_t pl[2];
            PutBE16(pl, (uint16_t) pps.size());
            payload.insert(payload.end(), pl, pl + 2);
            payload.insert(payload.end(), pps.begin(), pps.end());
            uint8_t hdr[128] = {};
            PutLE32(hdr, (uint32_t) payload.size());
            hdr[4] = 0x01;
            hdr[6] = 0x16;
            hdr[7] = 0x01;
            PutLE64(hdr + 8, ts);
            for (int off : {16, 40, 56}) PutFloat(hdr + off, (float) w);
            for (int off : {20, 44, 60}) PutFloat(hdr + off, (float) hgt);
            if (!SendAll(ms, hdr, 128) || !SendAll(ms, payload.data(), payload.size())) break;
            needCodecPacket = false;
        }
        if (avcc.empty()) continue;
        Bytes cipher(avcc.size());
        Bytes plain = avcc;  // mirror_buffer_decrypt works in place on its input
        mirror_buffer_decrypt(enc, plain.data(), cipher.data(), (int) plain.size());
        uint8_t hdr[128] = {};
        PutLE32(hdr, (uint32_t) cipher.size());
        hdr[4] = 0x00;
        hdr[5] = idr ? 0x10 : 0x00;
        PutLE64(hdr + 8, ts);
        if (!SendAll(ms, hdr, 128) || !SendAll(ms, cipher.data(), cipher.size()))
        {
            printf("  mirroring connection closed by the receiver (%d)\n", WSAGetLastError());
            break;
        }
        sentFrames++;
    }
    printf("  video: %d frames sent (last frame number %d); receiver made %d NTP requests\n", sentFrames, frame - 1,
           timing.Requests());

    running = false;
    feedback.join();
    if (audioThread.joinable()) audioThread.join();

    Teardown(rtsp, k, 110);
    closesocket(ms);
    rtsp.Request("TEARDOWN", k.url, "", "", r);
    rtsp.Close();
    timing.Stop();
    if (audioSock != INVALID_SOCKET) closesocket(audioSock);
    h264.Close();
    mirror_buffer_destroy(enc);
    return 0;
}

// ---------------------------------------------------------------- AirPlay speaker (ALAC)

static std::string Dmap(const char* tag, const std::string& value)
{
    std::string s(tag, 4);
    uint8_t len[4];
    PutBE32(len, (uint32_t) value.size());
    s.append((const char*) len, 4);
    return s + value;
}

static int RunSpeaker(const Options& o)
{
    printf("=== speaker session: %s:%d, %ds of ALAC audio with metadata\n", o.ip.c_str(), o.port, (int) o.seconds);
    AudioFile song;
    if (!o.audioFile.empty())
    {
        if (!song.Load(o.audioFile)) return 13;
        song.pos = std::min((size_t) (o.startAt * 44100), song.pcm.size() / 2 - 1);
    }
    std::string title = !o.title.empty() ? o.title : !song.title.empty() ? song.title : "Test Tone in A";
    std::string artist = !o.artist.empty() ? o.artist : !song.artist.empty() ? song.artist : "The Fake iPhones";
    std::string album = !o.album.empty() ? o.album : !song.album.empty() ? song.album : "Receiver Test Album";
    std::string cover = song.picture;
    if (!o.coverArt.empty())
    {
        FILE* f = fopen(o.coverArt.c_str(), "rb");
        if (f)
        {
            cover.clear();
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) cover.append(buf, n);
            fclose(f);
        }
        else printf("  cannot read %s\n", o.coverArt.c_str());
    }

    Rtsp rtsp;
    if (!rtsp.Connect(o.ip, o.port)) return 1;
    TimingServer timing;
    Keys k;
    int rc = Handshake(rtsp, o, false, timing, k);
    if (rc) return rc;
    SOCKET sock = BoundUdpSocket();
    uint16_t dataPort = 0, controlPort = 0;
    if ((rc = SetupAudio(rtsp, k, 2, AlacFrames::kSamples, 0x40000, false, sock, dataPort, controlPort)) != 0) return rc;

    Response r;
    rtsp.Request("SET_PARAMETER", k.url, "text/parameters", "volume: -11.250000\r\n", r);
    std::string meta = Dmap("mlit", Dmap("minm", title) + Dmap("asar", artist) + Dmap("asal", album));
    rtsp.Request("SET_PARAMETER", k.url, "application/x-dmap-tagged", meta, r);
    if (!cover.empty())
    {
        bool png = cover.size() > 4 && cover.compare(1, 3, "PNG") == 0;
        rtsp.Request("SET_PARAMETER", k.url, png ? "image/png" : "image/jpeg", cover, r);
    }
    // progress: RTP timestamps (44.1 kHz) of the track start, the first frame sent, and the track end.
    // Without a file: a 3:00 track, 0:42 in.
    uint32_t first = kFirstRtp;
    uint32_t elapsed = song.pcm.empty() ? 42 * 44100 : (uint32_t) song.pos;
    uint32_t length = song.pcm.empty() ? 180 * 44100 : (uint32_t) (song.pcm.size() / 2);
    std::string progress = "progress: " + std::to_string(first - elapsed) + "/" + std::to_string(first) + "/" +
                           std::to_string(first - elapsed + length) + "\r\n";
    rtsp.Request("SET_PARAMETER", k.url, "text/parameters", progress, r);

    AlacFrames alac;

    std::atomic<bool> running{true};
    std::thread feedback([&] {
        Response fr;
        while (running)
        {
            for (int i = 0; i < 20 && running; i++) Sleep(100);
            if (running) rtsp.Request("POST", "/feedback", "", "", fr, true);
        }
    });
    std::thread audioThread([&] {
        std::vector<int16_t> pcm(AlacFrames::kSamples * 2);
        double phase = 0;
        static const double notes[] = {440.0, 554.37, 659.25, 880.0};  // A major arpeggio
        StreamAudio(o, k, sock, dataPort, controlPort, AlacFrames::kSamples, running, [&](int frame, uint8_t* outBuf) {
            if (!song.pcm.empty()) song.Read(pcm.data(), AlacFrames::kSamples);
            else Tone(pcm.data(), AlacFrames::kSamples, phase, notes[(frame * AlacFrames::kSamples / 11025) % 4], true);  // a note every 250 ms
            return alac.Encode(pcm.data(), outBuf);
        });
    });
    double start = Seconds();
    while (Seconds() - start < o.seconds) Sleep(50);
    running = false;
    feedback.join();
    audioThread.join();
    Teardown(rtsp, k, 96);
    rtsp.Request("TEARDOWN", k.url, "", "", r);
    rtsp.Close();
    timing.Stop();
    closesocket(sock);
    return 0;
}

// ---------------------------------------------------------------- AirPlay video (casting)

static std::string HttpGet(const std::string& url)
{
    std::string cmd = "curl -s -L --max-time 30 \"" + url + "\"";
    FILE* p = _popen(cmd.c_str(), "rb");
    if (!p) return {};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    _pclose(p);
    return out;
}

static std::string DirOf(const std::string& url) { return url.substr(0, url.rfind('/') + 1); }

static std::string Resolve(const std::string& base, const std::string& ref)
{
    if (ref.find("://") != std::string::npos) return ref;
    if (!ref.empty() && ref[0] == '/')
    {
        size_t host = base.find("://");
        size_t slash = base.find('/', host == std::string::npos ? 0 : host + 3);
        return base.substr(0, slash) + ref;
    }
    return DirOf(base) + ref;
}

// rewrite every URI in a playlist (plain URI lines and URI="..." attributes) with f
static std::string MapUris(const std::string& playlist, const std::function<std::string(const std::string&)>& f)
{
    std::string out, line;
    size_t pos = 0;
    while (pos < playlist.size())
    {
        size_t end = playlist.find('\n', pos);
        if (end == std::string::npos) end = playlist.size();
        line = playlist.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') line = f(line);
        size_t u = line.find("URI=\"");
        if (u != std::string::npos && line[0] == '#')
        {
            size_t q = line.find('"', u + 5);
            if (q != std::string::npos) line = line.substr(0, u + 5) + f(line.substr(u + 5, q - u - 5)) + line.substr(q);
        }
        out += line + "\n";
        pos = end + 1;
    }
    return out;
}

static int RunCast(const Options& o)
{
    bool hls = !o.castHls.empty();
    std::string location = hls ? "mlhls://localhost/master.m3u8" : o.castUrl;
    printf("=== cast session: %s:%d, %s for %ds\n", o.ip.c_str(), o.port, hls ? o.castHls.c_str() : o.castUrl.c_str(),
           (int) o.seconds);
    std::string session = "6E7C0A5D-3D2B-4C1F-9A8E-" + std::to_string(100000000000ULL + (uint64_t) (Seconds() * 1000));
    Response r;

    // reverse ("PTTH") connection: the receiver sends its playlist requests (FCUP) over it
    Rtsp reverse;
    if (!reverse.Connect(o.ip, o.port)) return 1;
    reverse.SetHttpSession(session);
    Rtsp ctl;
    if (!ctl.Connect(o.ip, o.port)) return 1;
    ctl.SetHttpSession(session);
    {
        // POST /reverse with the upgrade headers, sent by hand (the helper has no extra headers)
        std::string req = "POST /reverse HTTP/1.1\r\nUpgrade: PTTH/1.0\r\nConnection: Upgrade\r\nX-Apple-Purpose: event\r\n"
                          "Content-Length: 0\r\nUser-Agent: AirPlay/980.77.1\r\nX-Apple-Session-ID: " + session + "\r\n\r\n";
        send(reverse.Socket(), req.data(), (int) req.size(), 0);
        char buf[1024];
        int n = recv(reverse.Socket(), buf, sizeof(buf) - 1, 0);
        buf[n > 0 ? n : 0] = 0;
        printf("  POST /reverse -> %.*s\n", (int) (strchr(buf, '\r') ? strchr(buf, '\r') - buf : 0), buf);
    }
    if (!ctl.Request("GET", "/server-info", "", "", r) || r.status != 200) return 2;

    std::atomic<bool> running{true};
    std::atomic<int> fcupServed{0};
    std::string lastEvent;  // last playback state event from the receiver (fcup thread)
    std::thread fcup([&] {
        // handle POST /event (FCUP requests) arriving on the reverse connection
        SOCKET s = reverse.Socket();
        DWORD timeout = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout));
        std::string pending;
        while (running)
        {
            char buf[8192];
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                if (WSAGetLastError() == WSAETIMEDOUT) continue;
                break;
            }
            pending.append(buf, n);
            size_t hdrEnd;
            while ((hdrEnd = pending.find("\r\n\r\n")) != std::string::npos)
            {
                size_t cl = pending.find("Content-Length:");
                size_t len = cl != std::string::npos && cl < hdrEnd ? (size_t) atoi(pending.c_str() + cl + 15) : 0;
                if (pending.size() < hdrEnd + 4 + len) break;
                std::string body = pending.substr(hdrEnd + 4, len);
                pending.erase(0, hdrEnd + 4 + len);
                std::string ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
                send(s, ok.data(), (int) ok.size(), 0);

                plist_t req = nullptr;
                plist_from_xml(body.c_str(), (uint32_t) body.size(), &req);
                if (plist_t st = plist_dict_get_item(req, "state"))
                {
                    // a playback state event (e.g. "stopped" when the item has played to its end)
                    char* state = nullptr;
                    plist_get_string_val(st, &state);
                    printf("  event from the receiver: state \"%s\"\n", state ? state : "?");
                    if (state)
                    {
                        lastEvent = state;
                        plist_mem_free(state);
                    }
                    plist_free(req);
                    continue;
                }
                plist_t fr = plist_dict_get_item(req, "request");
                char* url = nullptr;
                plist_get_string_val(plist_dict_get_item(fr, "FCUP_Response_URL"), &url);
                uint64_t requestId = PlistUint(fr, "FCUP_Response_RequestID");
                std::string u = url ? url : "";
                if (url) plist_mem_free(url);
                plist_free(req);

                // fetch what the receiver asked for from the real server, like the YouTube app does
                std::string data;
                const std::string prefix = "mlhls://localhost/";
                if (u.size() >= 12 && u.compare(u.size() - 12, 12, "/master.m3u8") == 0)
                {
                    data = MapUris(HttpGet(o.castHls), [&](const std::string& ref) {
                        std::string abs = Resolve(o.castHls, ref);
                        return prefix + abs.substr(abs.find("://") + 3);  // keep host/path, under the mlhls prefix
                    });
                    if (o.separateAudio)
                    {
                        // keep only audio renditions with their own playlist, so the receiver must play the
                        // separate audio track (like YouTube's, which is HLS packed audio)
                        std::string kept, line;
                        std::istringstream in(data);
                        while (std::getline(in, line))
                            if (!(line.rfind("#EXT-X-MEDIA:", 0) == 0 && line.find("TYPE=AUDIO") != std::string::npos &&
                                  line.find("URI=") == std::string::npos))
                                kept += line + "\n";
                        data = kept;
                    }
                }
                else if (u.compare(0, prefix.size(), prefix) == 0)
                {
                    std::string real = "https://" + u.substr(prefix.size());
                    data = MapUris(HttpGet(real), [&](const std::string& ref) { return Resolve(real, ref); });
                }
                printf("  FCUP request %llu: %s -> %zu bytes\n", (unsigned long long) requestId, u.c_str(), data.size());

                plist_t root = plist_new_dict();
                plist_dict_set_item(root, "type", plist_new_string("unhandledURLResponse"));
                plist_t params = plist_new_dict();
                plist_dict_set_item(params, "FCUP_Response_ClientInfo", plist_new_uint(1));
                plist_dict_set_item(params, "FCUP_Response_ClientRef", plist_new_uint(40030004));
                plist_dict_set_item(params, "FCUP_Response_Data", plist_new_data(data.data(), data.size()));
                plist_dict_set_item(params, "FCUP_Response_Headers", plist_new_dict());
                plist_dict_set_item(params, "FCUP_Response_RequestID", plist_new_uint(requestId));
                plist_dict_set_item(params, "FCUP_Response_StatusCode", plist_new_uint(data.empty() ? 404 : 200));
                plist_dict_set_item(params, "FCUP_Response_URL", plist_new_string(u.c_str()));
                plist_dict_set_item(root, "params", params);
                std::string bin = PlistToBin(root);
                plist_free(root);
                Response ar;
                ctl.Request("POST", "/action", "application/x-apple-binary-plist", bin, ar, true);
                fcupServed++;
            }
        }
    });

    {
        plist_t root = plist_new_dict();
        plist_dict_set_item(root, "Content-Location", plist_new_string(location.c_str()));
        plist_dict_set_item(root, "Start-Position-Seconds", plist_new_real(o.startAt));
        {
            // a fresh item id per run: the receiver resumes items it has seen before (by uuid)
            char uuid[48];
            Bytes rb = RandomBytes(6);
            snprintf(uuid, sizeof(uuid), "A1B2C3D4-0000-4000-8000-%02X%02X%02X%02X%02X%02X", rb[0], rb[1], rb[2], rb[3], rb[4], rb[5]);
            plist_dict_set_item(root, "uuid", plist_new_string(uuid));
        }
        if (hls) plist_dict_set_item(root, "clientProcName", plist_new_string("YouTube"));
        std::string body = PlistToBin(root);
        plist_free(root);
        if (!ctl.Request("POST", "/play", "application/x-apple-binary-plist", body, r) || r.status != 200) return 3;
    }

    // poll like iOS does, and (unless --just-play) exercise seek / pause / resume half-way
    double start = Seconds();
    bool sought = false, paused = false, resumed = false;
    while (Seconds() - start < o.seconds)
    {
        Sleep(1000);
        double t = Seconds() - start;
        if (!ctl.Request("GET", "/playback-info", "", "", r, true)) break;
        plist_t info = nullptr;
        if (!r.body.empty()) plist_from_xml((const char*) r.body.data(), (uint32_t) r.body.size(), &info);
        double pos = -1, dur = -1, rate = -1;
        if (info)
        {
            plist_t n;
            if ((n = plist_dict_get_item(info, "position"))) plist_get_real_val(n, &pos);
            if ((n = plist_dict_get_item(info, "duration"))) plist_get_real_val(n, &dur);
            if ((n = plist_dict_get_item(info, "rate"))) plist_get_real_val(n, &rate);
            plist_free(info);
        }
        printf("  t=%4.1fs playback-info: position %.1f / %.1f, rate %.0f\n", t, pos, dur, rate);
        if (o.justPlay) continue;
        if (!sought && t > o.seconds * 0.4 && dur > 60)
        {
            sought = true;
            ctl.Request("POST", "/scrub?position=60.000000", "", "", r);
        }
        if (!paused && t > o.seconds * 0.6)
        {
            paused = true;
            ctl.Request("POST", "/rate?value=0.000000", "", "", r);
        }
        if (!resumed && t > o.seconds * 0.75)
        {
            resumed = true;
            ctl.Request("POST", "/rate?value=1.000000", "", "", r);
        }
    }
    ctl.Request("POST", "/stop", "", "", r);
    running = false;
    fcup.join();
    printf("  FCUP requests served: %d\n", fcupServed.load());
    ctl.Close();
    reverse.Close();
    return 0;
}

int main(int argc, char** argv)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double) f.QuadPart;
    QueryPerformanceCounter(&g_qpcStart);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    setvbuf(stdout, nullptr, _IONBF, 0);

    Options o;
    bool discover = false;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--port") o.port = atoi(next());
        else if (a == "--name") o.name = next();
        else if (a == "--seconds") o.seconds = atof(next());
        else if (a == "--fps") o.fps = atoi(next());
        else if (a == "--size") sscanf(next(), "%dx%d", &o.width, &o.height);
        else if (a == "--pause-at") o.pauseAt = atof(next());
        else if (a == "--pause-for") o.pauseFor = atof(next());
        else if (a == "--rotate-at") o.rotateAt = atof(next());
        else if (a == "--no-audio") o.audio = false;
        else if (a == "--sessions") o.sessions = atoi(next());
        else if (a == "--speaker") o.speaker = true;
        else if (a == "--cover-art") o.coverArt = next();
        else if (a == "--image") o.image = next();
        else if (a == "--scroll") o.scroll = atof(next());
        else if (a == "--audio-file") o.audioFile = next();
        else if (a == "--title") o.title = next();
        else if (a == "--artist") o.artist = next();
        else if (a == "--album") o.album = next();
        else if (a == "--start-at") o.startAt = atof(next());
        else if (a == "--just-play") o.justPlay = true;
        else if (a == "--separate-audio") o.separateAudio = true;
        else if (a == "--corrupt-every") o.corruptEvery = atoi(next());
        else if (a == "--cast-url") o.castUrl = next();
        else if (a == "--cast-hls") o.castHls = next();
        else if (a == "--discover") discover = true;
        else if (a[0] != '-') o.ip = a;
    }
    if (discover)
    {
        Discover();
        return 0;
    }
    if (o.ip.empty())
    {
        printf("usage: fakesender <receiver-ip> [--port N] [--seconds N] [--name NAME]\n"
               "  mirroring: [--fps N] [--size WxH] [--image FILE [--scroll N]] [--pause-at S --pause-for S] [--rotate-at S]\n"
               "             [--no-audio | --audio-file FILE] [--sessions N]\n"
               "  speaker:   --speaker [--audio-file FILE [--start-at S]] [--title T] [--artist A] [--album A]\n"
               "             [--cover-art file.jpg|png]\n"
               "  casting:   --cast-url <media url> | --cast-hls <HLS master playlist url> [--start-at S] [--just-play]\n"
               "             [--separate-audio]\n"
               "  discovery: --discover\n");
        return 1;
    }
    int rc = 0;
    if (o.speaker) rc = RunSpeaker(o);
    else if (!o.castUrl.empty() || !o.castHls.empty()) rc = RunCast(o);
    else
    {
        for (int s = 1; s <= o.sessions && rc == 0; s++)
        {
            rc = RunMirror(o, s);
            if (s < o.sessions) Sleep(3000);
        }
    }
    printf(rc ? "FAILED at step %d\n" : "OK\n", rc);
    return rc;
}
