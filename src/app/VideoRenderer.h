#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

// Plays the mirrored H.264 stream (Annex-B access units, one per AirPlay packet) through a
// live MediaStreamSource, so decoding uses the platform (hardware) H.264 decoder.
class VideoRenderer
{
public:
    VideoRenderer();

    winrt::Windows::Media::Playback::MediaPlayer const& Player() const { return m_player; }

    void SetSize(uint32_t width, uint32_t height);
    void Start();                                    // a new mirroring stream begins (SPS/PPS follows)
    void PushFrame(const uint8_t* data, size_t len); // one Annex-B access unit
    void Flush();
    void Stop();

    // true once the first frame of a stream has been handed to the decoder, false after Stop()
    std::function<void(bool active)> ActiveChanged;

    struct Stats
    {
        uint64_t received = 0;
        uint64_t delivered = 0;
        uint64_t rendered = 0;  // frames the pipeline reports as shown (SampleRendered)
        uint64_t dropped = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    Stats GetStats();
    // logs frame counts, our newest timestamp and the player's clock/state (call off the UI thread)
    void LogDiagnostics();

private:
    struct Frame
    {
        std::vector<uint8_t> data;
        int64_t arrival = 0;  // 100 ns units since the stream started
        bool keyframe = false;
    };

    void OpenSourceLocked();
    void OnSampleRequested(winrt::Windows::Media::Core::MediaStreamSource const& sender,
                           winrt::Windows::Media::Core::MediaStreamSourceSampleRequestedEventArgs const& args);
    winrt::Windows::Media::Core::MediaStreamSample MakeSample(Frame& frame);
    int64_t Now100ns() const;

    winrt::Windows::Media::Playback::MediaPlayer m_player;

    std::mutex m_mutex;
    winrt::Windows::Media::Core::MediaStreamSource m_source{nullptr};
    winrt::Windows::Media::Core::MediaStreamSourceSampleRequest m_pending{nullptr};
    winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral m_pendingDeferral{nullptr};
    std::deque<Frame> m_queue;
    bool m_started = false;
    bool m_haveKeyframe = false;
    bool m_announcedActive = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int64_t m_t0 = 0;
    int64_t m_lastTimestamp = -1;
    LARGE_INTEGER m_qpcFrequency{};
    Stats m_stats;
    // per diagnostics interval
    uint64_t m_lastLoggedReceived = 0;
    uint64_t m_lastLoggedRendered = 0;
    int64_t m_maxLag = 0;           // 100 ns: worst lateness of a rendered frame
    size_t m_maxFrameBytes = 0;
    uint32_t m_keyframes = 0;
    uint32_t m_maxSlices = 0;       // VCL NAL units in one access unit
    uint32_t m_nalTypes = 0;        // bit n set: NAL type n was seen
    uint64_t m_bSlices = 0;         // B slices seen in this stream (none expected from iOS)
    std::vector<uint8_t> m_sps;     // last SPS seen, to log changes
};
