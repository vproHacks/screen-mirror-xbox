#include "pch.h"
#include "VideoRenderer.h"
#include "Log.h"
#include "H264Sps.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::Playback;

namespace
{
    constexpr size_t kMaxQueuedFrames = 120;  // ~2 s at 60 fps; beyond this the decoder is not keeping up

    // true if the Annex-B access unit contains an IDR slice (NAL type 5) or an SPS (type 7)
    bool IsKeyframe(const uint8_t* p, size_t len)
    {
        for (size_t i = 0; i + 4 < len; i++)
        {
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
            {
                int type = p[i + 3] & 0x1f;
                if (type == 5 || type == 7) return true;
                i += 2;
            }
        }
        return false;
    }

}

VideoRenderer::VideoRenderer()
{
    QueryPerformanceFrequency(&m_qpcFrequency);
    m_player.RealTimePlayback(true);  // low-latency mode for a live source
    m_player.AutoPlay(true);
    m_player.IsLoopingEnabled(false);
    m_player.CommandManager().IsEnabled(false);  // don't show up in the system media controls
    m_player.MediaFailed([](MediaPlayer const&, MediaPlayerFailedEventArgs const& e) {
        LOGE("video playback failed: 0x%08x %ls", static_cast<uint32_t>(e.ExtendedErrorCode().value),
             e.ErrorMessage().c_str());
    });
    m_player.MediaOpened([](MediaPlayer const&, IInspectable const&) { LOGI("video pipeline opened"); });
    m_player.PlaybackSession().PlaybackStateChanged([](MediaPlaybackSession const& s, IInspectable const&) {
        static const char* states[] = {"None", "Opening", "Buffering", "Playing", "Paused"};
        int st = static_cast<int>(s.PlaybackState());
        LOGI("video player state: %s (position %.2f s)", (st >= 0 && st < 5) ? states[st] : "?", s.Position().count() / 1e7);
    });
}

int64_t VideoRenderer::Now100ns() const
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<int64_t>((now.QuadPart * 10000000.0) / m_qpcFrequency.QuadPart);
}

VideoRenderer::Stats VideoRenderer::GetStats()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats s = m_stats;
    s.width = m_width;
    s.height = m_height;
    return s;
}

void VideoRenderer::LogDiagnostics()
{
    uint64_t received, delivered, newReceived, rendered, newRendered;
    size_t queued, maxBytes;
    int64_t lastTs, sinceStart, maxLag;
    uint32_t keyframes, maxSlices, nalTypes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;
        received = m_stats.received;
        delivered = m_stats.delivered;
        rendered = m_stats.rendered;
        newReceived = received - m_lastLoggedReceived;
        newRendered = rendered - m_lastLoggedRendered;
        m_lastLoggedReceived = received;
        m_lastLoggedRendered = rendered;
        queued = m_queue.size();
        lastTs = m_lastTimestamp;
        sinceStart = Now100ns() - m_t0;
        maxLag = m_maxLag;
        maxBytes = m_maxFrameBytes;
        keyframes = m_keyframes;
        maxSlices = m_maxSlices;
        nalTypes = m_nalTypes;
        m_maxLag = 0;
        m_maxFrameBytes = 0;
        m_keyframes = m_maxSlices = m_nalTypes = 0;
    }
    try
    {
        auto session = m_player.PlaybackSession();
        int64_t pos = session.Position().count();
        static const char* states[] = {"None", "Opening", "Buffering", "Playing", "Paused"};
        int st = static_cast<int>(session.PlaybackState());
        LOGI("video diag: rx %llu (+%llu) dlv %llu q %zu rend %llu (+%llu, max lag %.0f ms) | lastTs %.2f now %.2f pos %.2f "
             "(pos-lastTs %+.2f s) | %s %ux%u | max frame %zu B, slices %u, IDR %u, NAL types 0x%x",
             (unsigned long long) received, (unsigned long long) newReceived, (unsigned long long) delivered, queued,
             (unsigned long long) rendered, (unsigned long long) newRendered, maxLag / 1e4, lastTs / 1e7, sinceStart / 1e7,
             pos / 1e7, (pos - lastTs) / 1e7, (st >= 0 && st < 5) ? states[st] : "?", session.NaturalVideoWidth(),
             session.NaturalVideoHeight(), maxBytes, maxSlices, keyframes, nalTypes);
    }
    catch (winrt::hresult_error const& e)
    {
        LOGW("video diag: player query failed: %ls", e.message().c_str());
    }
}

void VideoRenderer::SetSize(uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_width = width;
    m_height = height;
}

void VideoRenderer::Start()
{
    MediaStreamSource source{nullptr};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started) return;  // a format change within one stream (e.g. rotation) keeps the source
        m_started = true;
        m_haveKeyframe = false;
        m_announcedActive = false;
        m_queue.clear();
        m_stats = Stats{};
        m_lastLoggedReceived = m_lastLoggedRendered = 0;
        m_maxLag = 0;
        m_maxFrameBytes = 0;
        m_keyframes = m_maxSlices = m_nalTypes = 0;
        m_sps.clear();
        m_bSlices = 0;
        m_lastTimestamp = -1;
        m_t0 = Now100ns();
        OpenSourceLocked();
        source = m_source;
    }
    // outside the lock: the player may call back into OnSampleRequested
    m_player.Source(MediaSource::CreateFromMediaStreamSource(source));
    m_player.Play();
}

void VideoRenderer::OpenSourceLocked()
{
    VideoEncodingProperties props = VideoEncodingProperties::CreateH264();
    if (m_width && m_height)
    {
        props.Width(m_width);
        props.Height(m_height);
    }
    VideoStreamDescriptor descriptor(props);
    MediaStreamSource source(descriptor);
    source.CanSeek(false);
    source.BufferTime(TimeSpan{0});
    source.IsLive(true);
    source.Starting([](MediaStreamSource const&, MediaStreamSourceStartingEventArgs const& args) {
        args.Request().SetActualStartPosition(TimeSpan{0});
    });
    source.SampleRequested({this, &VideoRenderer::OnSampleRequested});
    source.SampleRendered([this](MediaStreamSource const& sender, MediaStreamSourceSampleRenderedEventArgs const& args) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (sender != m_source) return;
        m_stats.rendered++;
        m_maxLag = std::max(m_maxLag, static_cast<int64_t>(args.SampleLag().count()));
    });
    source.Closed([](MediaStreamSource const&, MediaStreamSourceClosedEventArgs const& args) {
        LOGI("video source closed (reason %d)", static_cast<int>(args.Request().Reason()));
    });
    m_source = source;
    LOGI("opening H.264 video source %ux%u", m_width, m_height);
}

MediaStreamSample VideoRenderer::MakeSample(Frame& frame)
{
    // the presentation clock starts with the first sample, so use arrival time (relative to the
    // first delivered frame) as the timestamp; it must be strictly increasing
    int64_t ts = frame.arrival;
    if (ts <= m_lastTimestamp) ts = m_lastTimestamp + 1;
    m_lastTimestamp = ts;

    winrt::Windows::Storage::Streams::Buffer buffer(static_cast<uint32_t>(frame.data.size()));
    memcpy(buffer.data(), frame.data.data(), frame.data.size());
    buffer.Length(static_cast<uint32_t>(frame.data.size()));
    MediaStreamSample sample = MediaStreamSample::CreateFromBuffer(buffer, TimeSpan{ts});
    sample.KeyFrame(frame.keyframe);
    m_stats.delivered++;
    return sample;
}

void VideoRenderer::OnSampleRequested(MediaStreamSource const& sender, MediaStreamSourceSampleRequestedEventArgs const& args)
{
    bool announce = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto request = args.Request();
        if (sender != m_source)
        {
            request.Sample(nullptr);  // stale source: end its stream
            return;
        }
        if (!m_queue.empty())
        {
            Frame frame = std::move(m_queue.front());
            m_queue.pop_front();
            request.Sample(MakeSample(frame));
            if (!m_announcedActive)
            {
                m_announcedActive = true;
                announce = true;
            }
        }
        else
        {
            m_pending = request;
            m_pendingDeferral = request.GetDeferral();
        }
    }
    if (announce && ActiveChanged) ActiveChanged(true);
}

void VideoRenderer::PushFrame(const uint8_t* data, size_t len)
{
    bool announce = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started)
        {
            // frames before the SPS/codec packet (or after Stop) cannot be decoded
            m_stats.dropped++;
            return;
        }
        m_stats.received++;
        // stream statistics for the diagnostics line (NAL types, slices, keyframes), SPS changes,
        // and the SPS rewrite that lets the decoder show frames without waiting (see RewriteSps)
        uint32_t slices = 0;
        size_t spsStart = 0, spsEnd = 0;
        std::vector<uint8_t> newSps;
        m_maxFrameBytes = std::max(m_maxFrameBytes, len);
        for (size_t i = 0; i + 4 < len; i++)
        {
            if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
            size_t start = i + 3;
            int type = data[start] & 0x1f;
            m_nalTypes |= 1u << type;
            if (type == 1 || type == 5)
            {
                slices++;
                if (H264::SliceType(data + start, len - start) == 1 && m_bSlices++ == 0)
                    LOGW("the stream has B slices: frames are reordered, so the no-reordering SPS rewrite is now off");
            }
            if (type == 5) m_keyframes++;
            if (type == 7)
            {
                size_t end = start;
                while (end + 3 < len && !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 || data[end + 2] == 0)))
                    end++;
                if (end + 3 >= len) end = len;
                H264::SpsInfo info = H264::ParseSps(data + start, end - start);
                if (m_bSlices == 0) newSps = H264::RewriteSps(data + start, end - start, info);
                if (m_sps.size() != end - start || memcmp(m_sps.data(), data + start, end - start) != 0)
                {
                    m_sps.assign(data + start, data + end);
                    LOGI("H.264 SPS: %s%s", info.text.c_str(),
                         newSps.empty() ? "" : "; rewritten to declare no frame reordering");
                }
                spsStart = start;
                spsEnd = end;
            }
            i = start;
        }
        m_maxSlices = std::max(m_maxSlices, slices);
        Frame frame;
        frame.keyframe = IsKeyframe(data, len);
        if (!m_haveKeyframe)
        {
            if (!frame.keyframe)
            {
                m_stats.dropped++;
                return;
            }
            m_haveKeyframe = true;
            m_t0 = Now100ns();
        }
        if (newSps.empty())
            frame.data.assign(data, data + len);
        else
        {
            frame.data.reserve(len + newSps.size());
            frame.data.assign(data, data + spsStart);
            frame.data.insert(frame.data.end(), newSps.begin(), newSps.end());
            frame.data.insert(frame.data.end(), data + spsEnd, data + len);
        }
        frame.arrival = Now100ns() - m_t0;

        if (m_pending)
        {
            auto request = m_pending;
            auto deferral = m_pendingDeferral;
            m_pending = nullptr;
            m_pendingDeferral = nullptr;
            request.Sample(MakeSample(frame));
            deferral.Complete();
            if (!m_announcedActive)
            {
                m_announcedActive = true;
                announce = true;
            }
        }
        else
        {
            if (m_queue.size() >= kMaxQueuedFrames)
            {
                // the pipeline has stalled: keep only from the newest keyframe on
                LOGW("video queue overflow (%zu frames); discarding backlog", m_queue.size());
                m_stats.dropped += m_queue.size();
                m_queue.clear();
                m_haveKeyframe = frame.keyframe;
                if (!frame.keyframe)
                {
                    m_stats.dropped++;
                    return;
                }
            }
            m_queue.push_back(std::move(frame));
        }
    }
    if (announce && ActiveChanged) ActiveChanged(true);
}

void VideoRenderer::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
}

void VideoRenderer::Stop()
{
    bool wasActive;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;
        m_started = false;
        wasActive = m_announcedActive;
        m_announcedActive = false;
        m_queue.clear();
        if (m_pending)
        {
            m_pending.Sample(nullptr);  // end of stream
            m_pendingDeferral.Complete();
            m_pending = nullptr;
            m_pendingDeferral = nullptr;
        }
        m_source = nullptr;
        LOGI("video stopped: %llu frames received, %llu delivered, %llu dropped",
             (unsigned long long) m_stats.received, (unsigned long long) m_stats.delivered,
             (unsigned long long) m_stats.dropped);
    }
    try
    {
        m_player.Source(nullptr);
    }
    catch (winrt::hresult_error const& e)
    {
        LOGW("clearing the video source failed: %ls", e.message().c_str());
    }
    if (wasActive && ActiveChanged) ActiveChanged(false);
}
