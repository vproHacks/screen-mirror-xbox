#include "HlsAudio.h"

#include <cstring>
#include <string>

// (no precompiled header: also built into the desktop test in test/hlsaudio)

namespace
{
    constexpr uint16_t kPmtPid = 0x1000;
    constexpr uint16_t kAudioPid = 0x101;
    constexpr size_t kPacket = 188;

    uint32_t Crc32Mpeg(const uint8_t* p, size_t n)
    {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < n; i++)
        {
            crc ^= static_cast<uint32_t>(p[i]) << 24;
            for (int b = 0; b < 8; b++) crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        }
        return crc;
    }

    struct TsWriter
    {
        std::vector<uint8_t> out;
        uint8_t ccPat = 0, ccPmt = 0, ccAudio = 0;

        // one PSI section (PAT or PMT) in a single packet
        void Section(uint16_t pid, uint8_t& cc, std::vector<uint8_t> section)
        {
            uint32_t crc = Crc32Mpeg(section.data(), section.size());
            for (int i = 3; i >= 0; i--) section.push_back(static_cast<uint8_t>(crc >> (8 * i)));
            size_t start = out.size();
            out.resize(start + kPacket, 0xFF);
            uint8_t* p = out.data() + start;
            p[0] = 0x47;
            p[1] = static_cast<uint8_t>(0x40 | (pid >> 8));  // payload unit start
            p[2] = static_cast<uint8_t>(pid);
            p[3] = static_cast<uint8_t>(0x10 | (cc++ & 0x0F));
            p[4] = 0;  // pointer field
            memcpy(p + 5, section.data(), section.size());
        }

        void Tables()
        {
            // PAT: program 1 -> PMT
            Section(0, ccPat, {0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01,
                               static_cast<uint8_t>(0xE0 | (kPmtPid >> 8)), static_cast<uint8_t>(kPmtPid & 0xFF)});
            // PMT: one AAC (ADTS) stream, which also carries the PCR
            Section(kPmtPid, ccPmt, {0x02, 0xB0, 18, 0x00, 0x01, 0xC1, 0x00, 0x00,
                                     static_cast<uint8_t>(0xE0 | (kAudioPid >> 8)), static_cast<uint8_t>(kAudioPid & 0xFF),
                                     0xF0, 0x00, 0x0F, static_cast<uint8_t>(0xE0 | (kAudioPid >> 8)),
                                     static_cast<uint8_t>(kAudioPid & 0xFF), 0xF0, 0x00});
        }

        // one PES packet over as many TS packets as needed; the first carries a PCR
        void Pes(const uint8_t* frame, size_t len, uint64_t pts)
        {
            std::vector<uint8_t> pes = {0x00, 0x00, 0x01, 0xC0, 0, 0, 0x80, 0x80, 0x05};
            size_t pesLength = 3 + 5 + len;
            pes[4] = static_cast<uint8_t>(pesLength >> 8);
            pes[5] = static_cast<uint8_t>(pesLength);
            pes.push_back(static_cast<uint8_t>(0x21 | ((pts >> 29) & 0x0E)));
            pes.push_back(static_cast<uint8_t>(pts >> 22));
            pes.push_back(static_cast<uint8_t>(0x01 | ((pts >> 14) & 0xFE)));
            pes.push_back(static_cast<uint8_t>(pts >> 7));
            pes.push_back(static_cast<uint8_t>(0x01 | ((pts << 1) & 0xFE)));
            pes.insert(pes.end(), frame, frame + len);

            uint64_t pcr = pts >= 9000 ? pts - 9000 : 0;  // 100 ms ahead of the presentation time
            size_t pos = 0;
            bool first = true;
            while (pos < pes.size())
            {
                size_t remaining = pes.size() - pos;
                size_t adaptation = first ? 8 : 0;  // length byte + flags + 6-byte PCR
                if (remaining < kPacket - 4 - adaptation) adaptation = kPacket - 4 - remaining;  // stuffing
                size_t payload = kPacket - 4 - adaptation;
                size_t start = out.size();
                out.resize(start + kPacket, 0xFF);
                uint8_t* p = out.data() + start;
                p[0] = 0x47;
                p[1] = static_cast<uint8_t>((first ? 0x40 : 0x00) | (kAudioPid >> 8));
                p[2] = static_cast<uint8_t>(kAudioPid & 0xFF);
                p[3] = static_cast<uint8_t>((adaptation ? 0x30 : 0x10) | (ccAudio++ & 0x0F));
                if (adaptation)
                {
                    p[4] = static_cast<uint8_t>(adaptation - 1);
                    if (adaptation > 1)
                    {
                        p[5] = first ? 0x10 : 0x00;  // PCR flag
                        if (first && adaptation >= 8)
                        {
                            p[6] = static_cast<uint8_t>(pcr >> 25);
                            p[7] = static_cast<uint8_t>(pcr >> 17);
                            p[8] = static_cast<uint8_t>(pcr >> 9);
                            p[9] = static_cast<uint8_t>(pcr >> 1);
                            p[10] = static_cast<uint8_t>(((pcr & 1) << 7) | 0x7E);
                            p[11] = 0x00;
                        }
                        else
                            p[5] = 0x00;
                    }
                }
                memcpy(p + 4 + adaptation, pes.data() + pos, payload);
                pos += payload;
                first = false;
            }
        }
    };

    uint32_t SyncSafe(const uint8_t* p)
    {
        return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F);
    }

    // Skips the ID3 tag(s) at the start; reads the HLS timestamp from its PRIV frame
    size_t SkipId3(const uint8_t* data, size_t len, uint64_t& pts)
    {
        static const char kOwner[] = "com.apple.streaming.transportStreamTimestamp";
        size_t pos = 0;
        while (pos + 10 <= len && memcmp(data + pos, "ID3", 3) == 0)
        {
            int version = data[pos + 3];
            size_t size = SyncSafe(data + pos + 6);
            size_t end = pos + 10 + size + ((data[pos + 5] & 0x10) ? 10 : 0);
            if (end > len) return len;
            size_t f = pos + 10;
            while (f + 10 <= pos + 10 + size)
            {
                size_t frameSize = version >= 4 ? SyncSafe(data + f + 4)
                                                : (static_cast<size_t>(data[f + 4]) << 24 | data[f + 5] << 16 |
                                                   data[f + 6] << 8 | data[f + 7]);
                if (frameSize == 0 || f + 10 + frameSize > pos + 10 + size) break;
                const uint8_t* body = data + f + 10;
                if (memcmp(data + f, "PRIV", 4) == 0 && frameSize >= sizeof(kOwner) + 8 &&
                    memcmp(body, kOwner, sizeof(kOwner)) == 0)
                {
                    const uint8_t* ts = body + sizeof(kOwner);
                    pts = 0;
                    for (int i = 0; i < 8; i++) pts = (pts << 8) | ts[i];
                    pts &= 0x1FFFFFFFFULL;
                }
                f += 10 + frameSize;
            }
            pos = end;
        }
        return pos;
    }

    bool IsAdts(const uint8_t* p, size_t len)
    {
        return len >= 7 && p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
    }
}

namespace HlsAudio
{
    bool IsPackedAudio(const uint8_t* data, size_t len)
    {
        return (len >= 3 && memcmp(data, "ID3", 3) == 0) || IsAdts(data, len);
    }

    std::vector<uint8_t> PackedAudioToTs(const uint8_t* data, size_t len, uint64_t* ptsOut)
    {
        static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        uint64_t pts = 0;
        size_t pos = SkipId3(data, len, pts);
        if (ptsOut) *ptsOut = pts;
        TsWriter ts;
        ts.Tables();
        int frames = 0;
        uint64_t samples = 0;  // timestamps from the running sample count, so that rounding never adds up
        while (pos + 7 <= len)
        {
            if (!IsAdts(data + pos, len - pos))
            {
                pos++;  // resynchronize
                continue;
            }
            const uint8_t* p = data + pos;
            size_t frameLength = (static_cast<size_t>(p[3] & 0x03) << 11) | (p[4] << 3) | (p[5] >> 5);
            int rateIndex = (p[2] >> 2) & 0x0F;
            if (frameLength < 7 || pos + frameLength > len || rateIndex >= 13) break;
            ts.Pes(p, frameLength, (pts + samples * 90000 / kRates[rateIndex]) & 0x1FFFFFFFFULL);
            samples += ((p[6] & 0x03) + 1) * 1024;
            pos += frameLength;
            frames++;
        }
        if (frames == 0) return {};
        return std::move(ts.out);
    }
}
