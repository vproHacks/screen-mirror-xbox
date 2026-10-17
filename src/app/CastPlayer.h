#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// Plays "AirPlay video" (URL casting): either a direct http(s) media URL (e.g. Safari's AirPlay
// button) or an HLS master playlist that the library collected from the client (YouTube), which
// is then served to the platform's adaptive-streaming player straight from memory.
class CastPlayer
{
public:
    // Returns the playlist for a local HLS path ("/master.m3u8" or a media playlist path), or "".
    using PlaylistProvider = std::function<std::string(const std::string& pathAndQuery)>;

    CastPlayer();

    winrt::Windows::Media::Playback::MediaPlayer const& Player() const { return m_player; }

    void Play(const std::string& location, float startSeconds, PlaylistProvider provider);
    void Seek(float seconds);
    void SetRate(float rate);
    float Remove();  // the client removed the item: pause, return the position
    void Stop();
    void SetVolume(double gain);

    struct Info
    {
        bool available = false;  // a video is loaded and its position is known
        bool ended = false;      // playback failed: the client should stop
        bool finished = false;   // played to the end (reported as paused at the end, like an Apple TV)
        double duration = 0, position = 0;
        float rate = 0;
        bool ready = false, bufferEmpty = false, likelyToKeepUp = false;
    };
    Info GetInfo();

    // true when a cast video is showing, false when it has stopped
    std::function<void(bool active)> ActiveChanged;
    // the video played to its end (called on a media thread)
    std::function<void()> Finished;

private:
    winrt::fire_and_forget OpenAsync(uint64_t generation, std::string location, float startSeconds, PlaylistProvider provider);
    void SetActive(bool active);

    winrt::Windows::Media::Playback::MediaPlayer m_player;
    // fetches audio-rendition segments (see FetchAudioSegment in the .cpp)
    winrt::Windows::Web::Http::HttpClient m_http{nullptr};
    std::mutex m_mutex;
    uint64_t m_generation = 0;
    bool m_loading = false;
    bool m_opened = false;
    bool m_ended = false;
    bool m_finished = false;  // played to the end; the client decides what comes next
    double m_finishedDuration = 0;
    bool m_active = false;
    float m_startSeconds = 0;
    winrt::Windows::Media::Streaming::Adaptive::AdaptiveMediaSource m_adaptive{nullptr};
};
