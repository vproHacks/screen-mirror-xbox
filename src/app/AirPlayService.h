#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct raop_s;
struct dnssd_s;

class VideoRenderer;
class AudioRenderer;
class CastPlayer;

// Owns the UxPlay protocol library (RTSP/HTTP server, pairing, FairPlay, mirroring, audio RTP,
// HLS/URL video casting, built-in mDNS responder) and routes what it receives to the renderers.
class AirPlayService
{
public:
    struct Status
    {
        bool running = false;
        std::string name;
        std::string address;       // IPv4 address advertised over mDNS
        unsigned short port = 0;   // RTSP/AirPlay TCP port
        int connections = 0;
        bool mirroring = false;    // screen mirroring stream is running
        bool casting = false;      // an AirPlay video (URL/HLS) is loaded
        bool speaker = false;      // audio-only (ALAC) streaming
        std::string client;        // name of the connected device, if any
        std::string error;         // last fatal error, if any
    };

    // "Now playing" information sent with audio-only streaming
    struct NowPlaying
    {
        std::string title, artist, album;
        std::vector<uint8_t> coverArt;  // JPEG or PNG
        uint64_t coverArtVersion = 0;   // incremented whenever coverArt changes
        double position = -1;           // seconds, at progressTick; -1 if unknown
        double duration = 0;
        uint64_t progressTick = 0;      // GetTickCount64() when position was reported
    };

    AirPlayService(VideoRenderer& video, AudioRenderer& audio, CastPlayer& cast);
    ~AirPlayService();

    // stateFolder: writable folder for the device id.
    bool Start(const std::string& name, const std::wstring& stateFolder);
    void Stop();

    Status GetStatus();
    NowPlaying GetNowPlaying();
    std::function<void()> StatusChanged;
    std::function<void()> NowPlayingChanged;

    // UxPlay library callbacks (invoked on library threads through C trampolines)
    void OnConnInit();
    void OnConnDestroy();
    void OnConnReset(int reason);
    void OnConnFeedback();
    void OnAudioProcess(const unsigned char* data, int len, unsigned char ct, uint64_t ntpLocal);
    void OnVideoProcess(const unsigned char* data, int len, bool isH265, int nalCount);
    void OnAudioFlush();
    void OnVideoFlush();
    void OnVideoPause();
    void OnVideoResume();
    double OnGetClientVolume();
    void OnSetVolume(float volume);
    void OnAudioFormat(unsigned char ct, unsigned short spf, bool usingScreen, bool isMedia, uint64_t format);
    void OnVideoSize(float widthSource, float heightSource, float width, float height);
    void OnClientRequest(const char* deviceId, const char* model, const char* name, bool* admit);
    void OnVideoReset(int resetType);
    int OnVideoSetCodec(int codec);
    void OnMirrorRunning(bool running);
    void OnMetadata(const unsigned char* data, int len);
    void OnCoverArt(const unsigned char* data, int len);
    void OnStopCoverArt();
    void OnProgress(uint32_t start, uint32_t current, uint32_t end);
    void OnVideoPlay(const char* location, float startPosition);
    void OnVideoScrub(float position);
    void OnVideoRate(float rate);
    void OnVideoStop();
    void OnPlaybackInfo(void* playbackInfo);
    float OnPlaylistRemove();

private:
    void ControlLoop();
    void NotifyStatus();
    void NotifyNowPlaying();
    void EndSpeakerMode();
    std::string LoadOrCreateDeviceId(const std::wstring& folder);
    std::string HlsPlaylist(const std::string& path);

    VideoRenderer& m_video;
    AudioRenderer& m_audio;
    CastPlayer& m_cast;

    raop_s* m_raop = nullptr;
    dnssd_s* m_dnssd = nullptr;
    std::mutex m_raopMutex;          // guards m_raop for the cast player's playlist requests
    std::string m_langSystem;        // must outlive m_raop (raop_set_lang keeps the pointer)

    std::mutex m_mutex;
    Status m_status;
    NowPlaying m_nowPlaying;
    double m_volumeDb = 0.0;

    // control thread: client heartbeat watchdog and deferred server resets
    std::thread m_control;
    std::condition_variable m_controlCv;
    bool m_stopControl = false;
    bool m_resetHttpd = false;
    int m_missedFeedback = 0;
    unsigned m_tick = 0;
};
