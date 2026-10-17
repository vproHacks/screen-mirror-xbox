#include "pch.h"
#include "CastPlayer.h"
#include "Log.h"
#include "HlsAudio.h"

#include <winrt/Windows.Web.Http.Headers.h>

#include <set>
#include <sstream>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Media::Streaming::Adaptive;
using namespace winrt::Windows::Storage::Streams;

namespace
{
    // the library rewrites HLS playlist URIs to http://localhost:<port>/<original path>
    bool LocalPath(const std::string& uri, std::string& path)
    {
        static const char prefix[] = "http://localhost:";
        if (uri.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
        size_t slash = uri.find('/', sizeof(prefix) - 1);
        if (slash == std::string::npos) return false;
        path = uri.substr(slash);
        return true;
    }

    std::string AttrValue(const std::string& line, const char* name)
    {
        size_t p = line.find(name);
        if (p == std::string::npos) return {};
        p += strlen(name);
        if (p < line.size() && line[p] == '"')
        {
            size_t e = line.find('"', p + 1);
            return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
        }
        size_t e = line.find(',', p);
        return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }

    // The original Xbox One decodes H.264 up to 1080p in hardware; drop variants it cannot play
    // (VP9/AV1/HEVC or taller than 1080 lines) so adaptive playback never switches to them.
    bool KeepVariant(const std::string& info)
    {
        std::string codecs = AttrValue(info, "CODECS=");
        if (!codecs.empty() && codecs.find("avc1") == std::string::npos) return false;
        std::string res = AttrValue(info, "RESOLUTION=");
        size_t x = res.find('x');
        if (x != std::string::npos && atoi(res.c_str() + x + 1) > 1080) return false;
        return true;
    }

    std::string FilterMasterPlaylist(const std::string& master)
    {
        std::istringstream in(master);
        std::string line, out;
        int kept = 0, dropped = 0;
        bool skipNextUri = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (skipNextUri && !line.empty() && line[0] != '#')
            {
                skipNextUri = false;
                continue;
            }
            if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0)
            {
                if (!KeepVariant(line))
                {
                    skipNextUri = true;
                    dropped++;
                    continue;
                }
                kept++;
            }
            else if (line.rfind("#EXT-X-I-FRAME-STREAM-INF:", 0) == 0 && !KeepVariant(line))
            {
                continue;
            }
            out += line + "\n";
        }
        LOGI("cast: master playlist has %d playable variant(s), %d removed", kept, dropped);
        return kept > 0 ? out : master;
    }

    // "scheme://host/first/path/segment" of a URI, without its query and the rest of its path, for logs
    std::string ShortUri(const std::string& uri)
    {
        std::string s = uri.substr(0, uri.find('?'));
        size_t scheme = s.find("://");
        size_t host = scheme == std::string::npos ? 0 : s.find('/', scheme + 3);
        if (host == std::string::npos) return s;
        size_t second = s.find('/', host + 1);
        std::string tail = s.substr(s.rfind('/') + 1);
        return s.substr(0, second == std::string::npos ? s.size() : second) + "/.../" + tail.substr(0, 40);
    }

    // Logs the structure of the HLS playlists of a cast (tags and counts, no full URLs), to see what
    // the platform player is given when a stream does not play, and returns the segment URIs listed
    // in the audio renditions' playlists (see FetchAudioSegment)
    std::shared_ptr<std::set<std::string>> ScanPlaylists(const std::string& master, const CastPlayer::PlaylistProvider& provider)
    {
        auto audioSegments = std::make_shared<std::set<std::string>>();
        std::istringstream in(master);
        std::string line;
        std::vector<std::pair<std::string, bool>> uris;  // media playlist, is an audio rendition
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("#EXT-X-MEDIA:", 0) == 0)
            {
                std::string type = AttrValue(line, "TYPE="), uri = AttrValue(line, "URI=");
                LOGI("cast: rendition TYPE=%s GROUP-ID=%s LANGUAGE=%s DEFAULT=%s CODECS=%s%s", type.c_str(),
                     AttrValue(line, "GROUP-ID=").c_str(), AttrValue(line, "LANGUAGE=").c_str(),
                     AttrValue(line, "DEFAULT=").c_str(), AttrValue(line, "CODECS=").c_str(),
                     uri.empty() ? " (muxed, no URI)" : "");
                if (!uri.empty()) uris.emplace_back(uri, type == "AUDIO");
            }
            else if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0)
            {
                LOGI("cast: variant CODECS=%s RESOLUTION=%s AUDIO=%s BANDWIDTH=%s", AttrValue(line, "CODECS=").c_str(),
                     AttrValue(line, "RESOLUTION=").c_str(), AttrValue(line, "AUDIO=").c_str(),
                     AttrValue(line, "BANDWIDTH=").c_str());
            }
            else if (!line.empty() && line[0] != '#')
                uris.emplace_back(line, false);
        }
        for (const auto& [uri, audio] : uris)
        {
            std::string path;
            if (!LocalPath(uri, path)) continue;
            std::string text = provider(path);
            std::istringstream pl(text);
            int segments = 0;
            std::string tags, first;
            while (std::getline(pl, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("#EXTINF", 0) == 0) segments++;
                else if (line.rfind("#EXT-X-", 0) == 0 && tags.size() < 300)
                    tags += " " + line.substr(0, std::min<size_t>(line.size(), 60));
                else if (!line.empty() && line[0] != '#')
                {
                    if (first.empty()) first = ShortUri(line);
                    if (audio && line.rfind("http", 0) == 0) audioSegments->insert(line);
                }
            }
            LOGI("cast: %s playlist %s: %d segments, %zu bytes, first %s; tags:%s", audio ? "audio" : "media", path.c_str(),
                 segments, text.size(), first.c_str(), tags.c_str());
        }
        return audioSegments;
    }

    Buffer ToBuffer(const uint8_t* data, size_t size)
    {
        Buffer b(static_cast<uint32_t>(size));
        memcpy(b.data(), data, size);
        b.Length(static_cast<uint32_t>(size));
        return b;
    }

    // Audio-rendition segments are fetched here rather than by the player: HLS "packed audio" (an
    // ID3 tag then raw AAC frames, which YouTube uses for its audio tracks and the platform player
    // cannot read) is rewrapped as MPEG-TS; anything else is handed over unchanged
    winrt::fire_and_forget FetchAudioSegment(AdaptiveMediaSourceDownloadRequestedEventArgs args,
                                             winrt::Windows::Web::Http::HttpClient http, std::shared_ptr<std::atomic<int>> converted)
    {
        auto deferral = args.GetDeferral();
        try
        {
            winrt::Windows::Web::Http::HttpRequestMessage request(winrt::Windows::Web::Http::HttpMethod::Get(), args.ResourceUri());
            if (auto offset = args.ResourceByteRangeOffset())
            {
                auto length = args.ResourceByteRangeLength();
                std::wstring range = L"bytes=" + std::to_wstring(offset.Value()) + L"-" +
                                     (length ? std::to_wstring(offset.Value() + length.Value() - 1) : std::wstring());
                request.Headers().TryAppendWithoutValidation(L"Range", range);
            }
            auto response = co_await http.SendRequestAsync(request);
            if (response.IsSuccessStatusCode())
            {
                IBuffer body = co_await response.Content().ReadAsBufferAsync();
                std::vector<uint8_t> ts;
                if (HlsAudio::IsPackedAudio(body.data(), body.Length())) ts = HlsAudio::PackedAudioToTs(body.data(), body.Length());
                if (!ts.empty())
                {
                    args.Result().Buffer(ToBuffer(ts.data(), ts.size()));
                    args.Result().ContentType(L"video/mp2t");
                    if ((*converted)++ == 0)
                        LOGI("cast: audio segments are HLS packed audio (%u bytes -> %zu bytes of MPEG-TS): converting them",
                             body.Length(), ts.size());
                }
                else
                    args.Result().Buffer(body);
            }
            else
                LOGW("cast: audio segment request failed: HTTP %d", static_cast<int>(response.StatusCode()));
        }
        catch (hresult_error const& e)
        {
            LOGW("cast: audio segment request failed: %ls", e.message().c_str());
        }
        deferral.Complete();
    }

    Buffer ToBuffer(const std::string& s)
    {
        Buffer b(static_cast<uint32_t>(s.size()));
        memcpy(b.data(), s.data(), s.size());
        b.Length(static_cast<uint32_t>(s.size()));
        return b;
    }
}

CastPlayer::CastPlayer()
{
    m_http = winrt::Windows::Web::Http::HttpClient();
    m_player.AutoPlay(true);
    m_player.CommandManager().IsEnabled(false);
    m_player.MediaOpened([this](MediaPlayer const& player, IInspectable const&) {
        float start;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_loading) return;
            m_opened = true;
            start = m_startSeconds;
        }
        auto session = player.PlaybackSession();
        LOGI("cast: video opened (%ux%u, duration %.1f s), starting at %.1f s", session.NaturalVideoWidth(),
             session.NaturalVideoHeight(), session.NaturalDuration().count() / 1e7, start);
        if (start > 0.5f) session.Position(std::chrono::duration_cast<TimeSpan>(std::chrono::duration<float>(start)));
        player.Play();
        SetActive(true);
    });
    m_player.MediaEnded([this](MediaPlayer const& player, IInspectable const&) {
        // Not the end of the session: YouTube plays ads and the video as separate items, and after
        // one ends the client sends the next. It sees the item at its end and paused, as an Apple TV
        // reports it, and the picture stays until then.
        double duration = player.PlaybackSession().NaturalDuration().count() / 1e7;
        LOGI("cast: video ended at %.1f s; waiting for the client", duration);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_finished = true;
            m_finishedDuration = duration;
        }
        if (Finished) Finished();
    });
    m_player.MediaFailed([this](MediaPlayer const&, MediaPlayerFailedEventArgs const& e) {
        LOGE("cast: playback failed: 0x%08x %ls", static_cast<uint32_t>(e.ExtendedErrorCode().value), e.ErrorMessage().c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ended = true;
    });
}

void CastPlayer::Play(const std::string& location, float startSeconds, PlaylistProvider provider)
{
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        generation = ++m_generation;
        m_loading = true;
        m_opened = false;
        m_ended = false;
        m_finished = false;
        m_startSeconds = startSeconds;
    }
    LOGI("cast: play %s from %.1f s", location.c_str(), startSeconds);
    OpenAsync(generation, location, startSeconds, std::move(provider));
}

winrt::fire_and_forget CastPlayer::OpenAsync(uint64_t generation, std::string location, float startSeconds, PlaylistProvider provider)
{
    (void) startSeconds;
    co_await winrt::resume_background();
    try
    {
        MediaSource source{nullptr};
        AdaptiveMediaSource adaptive{nullptr};
        std::string path;
        if (LocalPath(location, path))
        {
            std::string master = provider(path);
            if (master.empty()) throw hresult_error(E_FAIL, L"the HLS master playlist is not available");
            master = FilterMasterPlaylist(master);
            auto audioSegments = ScanPlaylists(master, provider);
            auto converted = std::make_shared<std::atomic<int>>(0);

            InMemoryRandomAccessStream stream;
            DataWriter writer(stream);
            writer.WriteBuffer(ToBuffer(master));
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
            auto result = co_await AdaptiveMediaSource::CreateFromStreamAsync(stream, Uri(to_hstring(location)),
                                                                              L"application/vnd.apple.mpegurl", m_http);
            if (result.Status() != AdaptiveMediaSourceCreationStatus::Success)
            {
                throw hresult_error(E_FAIL, L"adaptive source creation failed, status " +
                                                to_hstring(static_cast<int>(result.Status())));
            }
            adaptive = result.MediaSource();
            // media playlists come from the library's store, not from the network; audio-rendition
            // segments are fetched (and converted if need be) by FetchAudioSegment
            auto http = m_http;
            adaptive.DownloadRequested([provider, audioSegments, converted, http](AdaptiveMediaSource const&,
                                                                                 AdaptiveMediaSourceDownloadRequestedEventArgs const& args) {
                std::string uri = to_string(args.ResourceUri().RawUri());
                std::string p;
                if (!LocalPath(uri, p))
                {
                    if (args.ResourceType() == AdaptiveMediaSourceResourceType::MediaSegment && audioSegments->count(uri))
                        FetchAudioSegment(args, http, converted);
                    return;  // other segments: the player downloads them itself
                }
                std::string text = provider(p);
                if (text.empty())
                {
                    LOGW("cast: no stored playlist for %s", p.c_str());
                    return;
                }
                args.Result().Buffer(ToBuffer(text));
                args.Result().ContentType(L"application/vnd.apple.mpegurl");
            });

            adaptive.DownloadFailed([](AdaptiveMediaSource const&, AdaptiveMediaSourceDownloadFailedEventArgs const& args) {
                std::string type = "?";
                uint64_t length = 0;
                int status = 0;
                if (auto response = args.HttpResponseMessage())
                {
                    status = static_cast<int>(response.StatusCode());
                    if (auto content = response.Content())
                    {
                        auto headers = content.Headers();
                        if (headers.ContentType()) type = to_string(headers.ContentType().MediaType());
                        if (headers.ContentLength()) length = headers.ContentLength().Value();
                    }
                }
                LOGW("cast: download failed (resource type %d, HTTP %d, %s, %llu bytes, error 0x%08x, %llu bytes received): %s",
                     static_cast<int>(args.ResourceType()), status, type.c_str(), (unsigned long long) length,
                     static_cast<uint32_t>(args.ExtendedError().value), (unsigned long long) args.Statistics().ContentBytesReceivedCount(),
                     ShortUri(to_string(args.ResourceUri().RawUri())).c_str());
            });
            source = MediaSource::CreateFromAdaptiveMediaSource(adaptive);
        }
        else
        {
            source = MediaSource::CreateFromUri(Uri(to_hstring(location)));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation != m_generation) co_return;  // stopped or replaced meanwhile
            m_adaptive = adaptive;
        }
        m_player.Source(source);
    }
    catch (hresult_error const& e)
    {
        LOGE("cast: could not open %s: %ls", location.c_str(), e.message().c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        if (generation == m_generation) m_ended = true;
    }
}

void CastPlayer::Seek(float seconds)
{
    LOGI("cast: seek to %.1f s", seconds);
    try
    {
        m_player.PlaybackSession().Position(std::chrono::duration_cast<TimeSpan>(std::chrono::duration<float>(seconds)));
    }
    catch (...)
    {
    }
}

void CastPlayer::SetRate(float rate)
{
    LOGI("cast: rate %.2f", rate);
    try
    {
        if (rate == 0.0f)
        {
            m_player.Pause();
        }
        else
        {
            m_player.PlaybackSession().PlaybackRate(rate);
            m_player.Play();
        }
    }
    catch (...)
    {
    }
}

float CastPlayer::Remove()
{
    float position = 0;
    try
    {
        m_player.Pause();
        position = static_cast<float>(m_player.PlaybackSession().Position().count() / 1e7);
    }
    catch (...)
    {
    }
    LOGI("cast: item removed at %.1f s", position);
    return position;
}

void CastPlayer::Stop()
{
    bool wasLoading;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        wasLoading = m_loading;
        m_generation++;
        m_loading = m_opened = m_ended = m_finished = false;
        m_adaptive = nullptr;
    }
    if (wasLoading)
    {
        LOGI("cast: stopped");
        try
        {
            m_player.Source(nullptr);
        }
        catch (...)
        {
        }
    }
    SetActive(false);
}

void CastPlayer::SetVolume(double gain)
{
    try
    {
        m_player.Volume(gain);
    }
    catch (...)
    {
    }
}

CastPlayer::Info CastPlayer::GetInfo()
{
    Info info;
    bool opened;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_loading) return info;
        if (m_ended)
        {
            info.ended = true;
            return info;
        }
        if (m_finished)
        {
            info.available = info.finished = info.ready = info.likelyToKeepUp = true;
            info.duration = info.position = m_finishedDuration;
            info.rate = 0.0f;
            return info;
        }
        opened = m_opened;
    }
    if (!opened) return info;
    try
    {
        auto session = m_player.PlaybackSession();
        auto state = session.PlaybackState();
        info.available = true;
        info.duration = session.NaturalDuration().count() / 1e7;
        info.position = session.Position().count() / 1e7;
        info.rate = state == MediaPlaybackState::Paused ? 0.0f : 1.0f;
        info.ready = true;
        info.bufferEmpty = state == MediaPlaybackState::Buffering;
        info.likelyToKeepUp = state == MediaPlaybackState::Playing;
    }
    catch (...)
    {
        info.available = false;
    }
    return info;
}

void CastPlayer::SetActive(bool active)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active == active) return;
        m_active = active;
    }
    if (ActiveChanged) ActiveChanged(active);
}
