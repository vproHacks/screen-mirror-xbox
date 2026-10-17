#include "pch.h"
#include "AirPlayService.h"
#include "AudioRenderer.h"
#include "CastPlayer.h"
#include "Log.h"
#include "VideoRenderer.h"

#include <cstdio>
#include <random>

#include "msvc_force.h"
extern "C" {
#include "raop.h"
#include "dnssd.h"
#include "logger.h"
}

namespace
{
    constexpr unsigned short kPreferredRtspPort = 7000;
    constexpr int kMissedFeedbackLimit = 15;  // seconds; clients send POST /feedback every 2 s
    constexpr int kIdleCastLimit = 120;       // seconds without a client poll once a cast video is over

    AirPlayService* Self(void* cls) { return static_cast<AirPlayService*>(cls); }

    // ---- C trampolines for raop_callbacks_t ----
    void cb_conn_init(void* cls) { Self(cls)->OnConnInit(); }
    void cb_conn_destroy(void* cls) { Self(cls)->OnConnDestroy(); }
    void cb_conn_reset(void* cls, int reason) { Self(cls)->OnConnReset(reason); }
    void cb_conn_feedback(void* cls) { Self(cls)->OnConnFeedback(); }
    void cb_audio_process(void* cls, raop_ntp_t*, audio_decode_struct* d)
    {
        Self(cls)->OnAudioProcess(d->data, d->data_len, d->ct, d->ntp_time_local);
    }
    void cb_video_process(void* cls, raop_ntp_t*, video_decode_struct* d)
    {
        Self(cls)->OnVideoProcess(d->data, d->data_len, d->is_h265, d->nal_count);
    }
    void cb_audio_flush(void* cls) { Self(cls)->OnAudioFlush(); }
    void cb_video_flush(void* cls) { Self(cls)->OnVideoFlush(); }
    void cb_video_pause(void* cls) { Self(cls)->OnVideoPause(); }
    void cb_video_resume(void* cls) { Self(cls)->OnVideoResume(); }
    double cb_audio_set_client_volume(void* cls) { return Self(cls)->OnGetClientVolume(); }
    void cb_audio_set_volume(void* cls, float volume) { Self(cls)->OnSetVolume(volume); }
    void cb_audio_get_format(void* cls, unsigned char* ct, unsigned short* spf, bool* usingScreen, bool* isMedia,
                             uint64_t* audioFormat)
    {
        Self(cls)->OnAudioFormat(*ct, *spf, *usingScreen, *isMedia, *audioFormat);
    }
    void cb_video_report_size(void* cls, float* ws, float* hs, float* w, float* h)
    {
        Self(cls)->OnVideoSize(*ws, *hs, *w, *h);
    }
    void cb_audio_set_metadata(void* cls, const void* buf, int len) { Self(cls)->OnMetadata(static_cast<const unsigned char*>(buf), len); }
    void cb_audio_set_coverart(void* cls, const void* buf, int len) { Self(cls)->OnCoverArt(static_cast<const unsigned char*>(buf), len); }
    void cb_audio_stop_coverart_rendering(void* cls) { Self(cls)->OnStopCoverArt(); }
    void cb_audio_set_progress(void* cls, uint32_t* start, uint32_t* curr, uint32_t* end) { Self(cls)->OnProgress(*start, *curr, *end); }
    void cb_report_client_request(void* cls, char* deviceid, char* model, char* name, bool* admit)
    {
        Self(cls)->OnClientRequest(deviceid, model, name, admit);
    }
    void cb_display_pin(void*, char* pin) { LOGI("client asked for PIN %s (PIN pairing is not enabled)", pin); }
    void cb_register_client(void*, const char*, const char*, const char*) {}
    bool cb_check_register(void*, const char*) { return true; }
    const char* cb_passwd(void*, int* len)
    {
        *len = 0;  // no password
        return nullptr;
    }
    void cb_export_dacp(void*, const char*, const char*) {}
    void cb_video_reset(void* cls, reset_type_t type) { Self(cls)->OnVideoReset(static_cast<int>(type)); }
    int cb_video_set_codec(void* cls, video_codec_t codec) { return Self(cls)->OnVideoSetCodec(static_cast<int>(codec)); }
    void cb_mirror_video_running(void* cls, bool running) { Self(cls)->OnMirrorRunning(running); }
    // AirPlay video (URL / HLS casting)
    void cb_on_video_play(void* cls, const char* location, const float start) { Self(cls)->OnVideoPlay(location, start); }
    void cb_on_video_scrub(void* cls, const float position) { Self(cls)->OnVideoScrub(position); }
    void cb_on_video_rate(void* cls, const float rate) { Self(cls)->OnVideoRate(rate); }
    void cb_on_video_stop(void* cls) { Self(cls)->OnVideoStop(); }
    void cb_on_video_acquire_playback_info(void* cls, playback_info_t* info) { Self(cls)->OnPlaybackInfo(info); }
    float cb_on_video_playlist_remove(void* cls) { return Self(cls)->OnPlaylistRemove(); }

    void cb_log(void*, int level, const char* msg)
    {
        switch (level)
        {
        case LOGGER_EMERG:
        case LOGGER_ALERT:
        case LOGGER_CRIT:
        case LOGGER_ERR: Log::WriteRaw(Log::Level::Error, msg); break;
        case LOGGER_WARNING: Log::WriteRaw(Log::Level::Warn, msg); break;
        case LOGGER_NOTICE:
        case LOGGER_INFO: Log::WriteRaw(Log::Level::Info, msg); break;
        default: Log::WriteRaw(Log::Level::Debug, msg); break;
        }
    }

    std::string LocalIPv4()
    {
        // let the routing table choose the LAN interface (same trick the mDNS responder uses)
        std::string result;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == INVALID_SOCKET) return result;
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(5353);
        inet_pton(AF_INET, "224.0.0.251", &remote.sin_addr);
        sockaddr_in local{};
        int len = sizeof(local);
        if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0 &&
            getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
        {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
            result = buf;
        }
        closesocket(s);
        return result;
    }

    std::vector<char> ParseMac(const std::string& mac)
    {
        std::vector<char> bytes;
        for (size_t i = 0; i + 1 < mac.size(); i += 3)
            bytes.push_back(static_cast<char>(std::stoul(mac.substr(i, 2), nullptr, 16)));
        return bytes;
    }
}

AirPlayService::AirPlayService(VideoRenderer& video, AudioRenderer& audio, CastPlayer& cast)
    : m_video(video), m_audio(audio), m_cast(cast)
{
    // an item that played to its end: tell the client, as an Apple TV does, so that it moves on
    // (the YouTube app plays an advertisement and then the video as separate items)
    m_cast.Finished = [this] {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_raop && raop_send_video_event(m_raop, "stopped") != 0) LOGW("could not send the end-of-item event");
    };
}

AirPlayService::~AirPlayService() { Stop(); }

std::string AirPlayService::LoadOrCreateDeviceId(const std::wstring& folder)
{
    std::wstring path = folder + L"\\device_id.txt";
    char buf[32] = {};
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        size_t n = fread(buf, 1, 17, f);
        fclose(f);
        if (n == 17 && buf[2] == ':' && buf[14] == ':') return std::string(buf, 17);
    }
    std::random_device rd;
    unsigned char b[6];
    for (auto& x : b) x = static_cast<unsigned char>(rd() & 0xff);
    b[0] = static_cast<unsigned char>((b[0] & ~0x01) | 0x02);  // locally administered unicast
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    if (FILE* f = _wfopen(path.c_str(), L"wb"))
    {
        fwrite(buf, 1, 17, f);
        fclose(f);
    }
    return std::string(buf, 17);
}

bool AirPlayService::Start(const std::string& name, const std::wstring& stateFolder)
{
    Stop();

    std::string deviceId = LoadOrCreateDeviceId(stateFolder);
    std::vector<char> hwaddr = ParseMac(deviceId);
    LOGI("starting AirPlay receiver \"%s\", device id %s", name.c_str(), deviceId.c_str());

    ntp_global_init();

    raop_callbacks_t cbs{};
    cbs.cls = this;
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.video_report_size = cb_video_report_size;
    cbs.audio_set_metadata = cb_audio_set_metadata;
    cbs.audio_set_coverart = cb_audio_set_coverart;
    cbs.audio_stop_coverart_rendering = cb_audio_stop_coverart_rendering;
    cbs.audio_set_progress = cb_audio_set_progress;
    cbs.report_client_request = cb_report_client_request;
    cbs.display_pin = cb_display_pin;
    cbs.register_client = cb_register_client;
    cbs.check_register = cb_check_register;
    cbs.passwd = cb_passwd;
    cbs.export_dacp = cb_export_dacp;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec = cb_video_set_codec;
    cbs.mirror_video_running = cb_mirror_video_running;
    cbs.on_video_play = cb_on_video_play;
    cbs.on_video_scrub = cb_on_video_scrub;
    cbs.on_video_rate = cb_on_video_rate;
    cbs.on_video_stop = cb_on_video_stop;
    cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;
    cbs.on_video_playlist_remove = cb_on_video_playlist_remove;

    auto fail = [&](const char* what) {
        LOGE("%s", what);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.error = what;
        }
        Stop();
        NotifyStatus();
        return false;
    };

    m_raop = raop_init(&cbs);
    if (!m_raop) return fail("raop_init failed (could not initialize Winsock?)");
    raop_set_log_callback(m_raop, cb_log, nullptr);
    // protocol-level detail (every request, plist dumps) only when LocalState\debug.txt exists
    bool debugLog = GetFileAttributesW((stateFolder + L"\\debug.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    raop_set_log_level(m_raop, debugLog ? LOGGER_DEBUG : LOGGER_INFO);

    // nohold = 1: a new client takes over from the current one. Empty keyfile: the pairing key
    // is derived from the (persisted) device id, so it stays the same across launches.
    if (raop_init2(m_raop, 1, deviceId.c_str(), "") != 0) return fail("raop_init2 failed");

    raop_set_plist(m_raop, "width", 1920);
    raop_set_plist(m_raop, "height", 1080);
    raop_set_plist(m_raop, "refreshRate", 60);
    raop_set_plist(m_raop, "maxFPS", 60);

    // AirPlay video (URL / YouTube HLS casting), as UxPlay's -hls option. The language list picks
    // the audio track of multi-language (dubbed) YouTube videos: the user's Windows languages.
    raop_set_plist(m_raop, "hls", 1);
    m_langSystem.clear();
    try
    {
        for (auto const& l : winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages())
        {
            if (!m_langSystem.empty()) m_langSystem += ":";
            m_langSystem += winrt::to_string(l);
        }
    }
    catch (...)
    {
    }
    if (m_langSystem.empty()) m_langSystem = "en-US";
    raop_set_lang(m_raop, "", "", m_langSystem.c_str());

    unsigned short tcp[2] = {0, kPreferredRtspPort};  // mirror data port: dynamic; RTSP: 7000
    unsigned short udp[3] = {0, 0, 0};                 // timing, control, data: dynamic
    raop_set_tcp_ports(m_raop, tcp);
    raop_set_udp_ports(m_raop, udp);

    // the request handlers read the TXT records from dnssd, so attach it before listening
    int error = 0;
    m_dnssd = dnssd_init(name.c_str(), static_cast<int>(name.size()), hwaddr.data(), static_cast<int>(hwaddr.size()), 0, &error);
    if (!m_dnssd) return fail("dnssd_init failed");

    // feature bits as in UxPlay's start_dnssd() with -hls: mirroring, audio, AirPlay video + HLS
    // (bits 0 and 4), legacy pairing; no H.265 (the original Xbox One has no HEVC decoder)
    const int on[] = {0, 1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 27, 28, 30};
    const int off[] = {3, 8, 23, 24, 26, 29, 31, 42};
    for (int bit : on) dnssd_set_airplay_features(m_dnssd, bit, 1);
    for (int bit : off) dnssd_set_airplay_features(m_dnssd, bit, 0);
    LOGI("advertised features 0x%llx", (unsigned long long) dnssd_get_airplay_features(m_dnssd));
    raop_set_dnssd(m_raop, m_dnssd);

    unsigned short port = kPreferredRtspPort;
    if (raop_start_httpd(m_raop, &port) < 0)
    {
        LOGW("could not listen on TCP port %u, using a dynamic port", (unsigned) kPreferredRtspPort);
        port = 0;
        if (raop_start_httpd(m_raop, &port) < 0) return fail("could not start the AirPlay (RTSP) server socket");
    }
    raop_set_port(m_raop, port);
    LOGI("AirPlay server listening on TCP port %u", (unsigned) port);

    int ret = dnssd_register_raop(m_dnssd, port);
    if (ret != 0)
    {
        LOGE("mDNS: registering _raop._tcp failed (%d): could not open UDP port 5353 / join 224.0.0.251", ret);
        return fail("mDNS responder failed to start (UDP 5353)");
    }
    ret = dnssd_register_airplay(m_dnssd, port);
    if (ret != 0) return fail("mDNS: registering _airplay._tcp failed");
    LOGI("mDNS: advertising _airplay._tcp and _raop._tcp as \"%s\"", name.c_str());

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = Status{};
        m_status.running = true;
        m_status.name = name;
        m_status.address = LocalIPv4();
        m_status.port = port;
        m_stopControl = false;
        m_resetHttpd = false;
        m_missedFeedback = 0;
    }
    m_control = std::thread([this] { ControlLoop(); });
    NotifyStatus();
    return true;
}

void AirPlayService::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopControl = true;
    }
    m_controlCv.notify_all();
    if (m_control.joinable()) m_control.join();

    if (m_dnssd)
    {
        dnssd_unregister_raop(m_dnssd);
        dnssd_unregister_airplay(m_dnssd);
    }
    m_cast.Stop();
    if (m_raop)
    {
        std::lock_guard<std::mutex> lock(m_raopMutex);
        raop_destroy(m_raop);  // stops the server and all sessions
        m_raop = nullptr;
    }
    if (m_dnssd)
    {
        dnssd_destroy(m_dnssd);
        m_dnssd = nullptr;
    }
    m_video.Stop();
    m_audio.Stop();

    bool wasRunning;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        wasRunning = m_status.running;
        m_status.running = false;
        m_status.mirroring = false;
        m_status.casting = false;
        m_status.connections = 0;
        m_status.client.clear();
    }
    EndSpeakerMode();
    if (wasRunning) NotifyStatus();
}

AirPlayService::NowPlaying AirPlayService::GetNowPlaying()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nowPlaying;
}

void AirPlayService::NotifyNowPlaying()
{
    if (NowPlayingChanged) NowPlayingChanged();
}

void AirPlayService::EndSpeakerMode()
{
    bool was;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        was = m_status.speaker;
        m_status.speaker = false;
        m_nowPlaying = NowPlaying{};
    }
    if (was)
    {
        NotifyStatus();
        NotifyNowPlaying();
    }
}

std::string AirPlayService::HlsPlaylist(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_raopMutex);
    if (!m_raop) return {};
    int len = 0;
    char* data = raop_get_hls_playlist(m_raop, path.c_str(), &len);
    std::string text = data ? std::string(data, len) : std::string();
    free(data);
    return text;
}

AirPlayService::Status AirPlayService::GetStatus()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void AirPlayService::NotifyStatus()
{
    if (StatusChanged) StatusChanged();
}

void AirPlayService::ControlLoop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopControl)
    {
        m_controlCv.wait_for(lock, std::chrono::seconds(1));
        if (m_stopControl) break;

        if (++m_tick % 3 == 0)
        {
            lock.unlock();
            m_video.LogDiagnostics();
            lock.lock();
        }

        if (m_status.connections > 0)
        {
            // A cast video plays on its own, as on an Apple TV: the client may stop polling (a locked
            // phone) without leaving. The session ends with its /stop, or is dropped once the video is
            // over (or never loaded) and the client has stayed silent for a while.
            int limit = kMissedFeedbackLimit;
            if (m_status.casting)
            {
                lock.unlock();
                CastPlayer::Info ci = m_cast.GetInfo();
                lock.lock();
                limit = (ci.available && !ci.finished) ? INT_MAX : kIdleCastLimit;
            }
            if (++m_missedFeedback > limit)
            {
                LOGW("no heartbeat from the client for %d s: dropping the connection", m_missedFeedback);
                m_resetHttpd = true;
            }
        }
        else
        {
            m_missedFeedback = 0;
        }

        if (m_resetHttpd && m_raop)
        {
            m_resetHttpd = false;
            m_missedFeedback = 0;
            unsigned short port = m_status.port;
            lock.unlock();
            LOGI("resetting the AirPlay server");
            raop_stop_httpd(m_raop);  // closes every connection (conn_destroy callbacks run here)
            m_video.Stop();
            m_audio.Stop();
            if (raop_start_httpd(m_raop, &port) < 0) LOGE("could not restart the AirPlay server on port %u", (unsigned) port);
            raop_set_port(m_raop, port);
            lock.lock();
            m_status.mirroring = false;
            m_status.casting = false;
            m_status.client.clear();
            lock.unlock();
            m_cast.Stop();
            EndSpeakerMode();
            NotifyStatus();
            lock.lock();
        }
    }
}

// ---------------- library callbacks ----------------

void AirPlayService::OnConnInit()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.connections++;
        m_missedFeedback = 0;
    }
    NotifyStatus();
}

void AirPlayService::OnConnDestroy()
{
    bool last;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.connections > 0) m_status.connections--;
        last = m_status.connections == 0;
        if (last)
        {
            m_status.client.clear();
            m_status.mirroring = false;
            m_status.casting = false;
        }
    }
    if (last)
    {
        m_audio.Stop();
        m_video.Stop();
        m_cast.Stop();
        EndSpeakerMode();
    }
    NotifyStatus();
}

void AirPlayService::OnConnReset(int reason)
{
    LOGW("connection reset requested by the library (reason %d)", reason);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_resetHttpd = true;
    }
    m_controlCv.notify_all();
}

void AirPlayService::OnConnFeedback()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_missedFeedback = 0;
}

void AirPlayService::OnAudioProcess(const unsigned char* data, int len, unsigned char ct, uint64_t ntpLocal)
{
    if (data && len > 0) m_audio.Push(data, static_cast<size_t>(len), ct, ntpLocal);
}

void AirPlayService::OnVideoProcess(const unsigned char* data, int len, bool isH265, int nalCount)
{
    (void) nalCount;
    if (!data || len < 5 || isH265) return;
    if (data[0] != 0)  // the library marks packets that failed decryption/validation by setting byte 0
    {
        LOGW("dropping invalid video packet (%d bytes)", len);
        return;
    }
    m_video.PushFrame(data, static_cast<size_t>(len));
}

void AirPlayService::OnAudioFlush() { m_audio.Flush(); }
void AirPlayService::OnVideoFlush() { m_video.Flush(); }
void AirPlayService::OnVideoPause() { LOGI("client paused the video stream (screen locked?)"); }
void AirPlayService::OnVideoResume() { LOGI("client resumed the video stream"); }

double AirPlayService::OnGetClientVolume()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_volumeDb;
}

void AirPlayService::OnSetVolume(float volume)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_volumeDb = volume;
    }
    m_audio.SetVolumeDb(volume);
    m_cast.SetVolume(AudioRenderer::GainForAirPlayVolume(volume));
}

void AirPlayService::OnAudioFormat(unsigned char ct, unsigned short spf, bool usingScreen, bool isMedia, uint64_t format)
{
    LOGI("audio stream: ct=%u spf=%u usingScreen=%d isMedia=%d audioFormat=0x%llx", ct, spf, usingScreen, isMedia,
         (unsigned long long) format);
    m_audio.Start(ct, spf);
    bool speaker = !usingScreen;  // audio-only streaming (the receiver picked as a speaker)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.speaker == speaker) return;
        m_status.speaker = speaker;
        if (speaker) m_nowPlaying = NowPlaying{};
    }
    NotifyStatus();
    NotifyNowPlaying();
}

void AirPlayService::OnVideoSize(float widthSource, float heightSource, float width, float height)
{
    LOGI("video size: source %.0fx%.0f, stream %.0fx%.0f", widthSource, heightSource, width, height);
    m_video.SetSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

void AirPlayService::OnClientRequest(const char* deviceId, const char* model, const char* name, bool* admit)
{
    *admit = true;
    LOGI("connection request from \"%s\" (%s), device id %s", name ? name : "?", model ? model : "?",
         deviceId ? deviceId : "?");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.client = name ? name : (model ? model : "AirPlay device");
    }
    NotifyStatus();
}

void AirPlayService::OnVideoReset(int resetType)
{
    LOGI("video reset (type %d)", resetType);
    switch (static_cast<reset_type_t>(resetType))
    {
    case RESET_TYPE_NOHOLD:  // a new client took over: stop everything of the old one
        m_cast.Stop();
        EndSpeakerMode();
        [[fallthrough]];
    case RESET_TYPE_RTP_SHUTDOWN:
    case RESET_TYPE_RTP_TO_HLS_TEARDOWN:
        m_video.Stop();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.mirroring = false;
            if (resetType == RESET_TYPE_NOHOLD) m_status.casting = false;
        }
        NotifyStatus();
        break;
    case RESET_TYPE_HLS_SHUTDOWN:
    case RESET_TYPE_HLS_EOS:
        m_cast.Stop();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.casting = false;
        }
        NotifyStatus();
        break;
    default:
        break;
    }
}

int AirPlayService::OnVideoSetCodec(int codec)
{
    if (codec != VIDEO_CODEC_H264)
    {
        LOGE("client offered an unsupported video codec (%d)", codec);
        return -1;
    }
    LOGI("video codec: H.264");
    m_video.Start();
    return 0;
}

void AirPlayService::OnMirrorRunning(bool running)
{
    LOGI("mirroring %s", running ? "started" : "stopped");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.mirroring = running;
    }
    NotifyStatus();
}

// ---------------- audio-only "now playing" ----------------

namespace
{
    // DMAP ("application/x-dmap-tagged"): 4-byte tag, 4-byte big-endian length, payload;
    // "mlit"/"mlcl" are containers. Collects title (minm), artist (asar) and album (asal).
    void ParseDmap(const unsigned char* p, size_t len, AirPlayService::NowPlaying& np, int depth = 0)
    {
        size_t pos = 0;
        while (pos + 8 <= len && depth < 4)
        {
            std::string tag(reinterpret_cast<const char*>(p + pos), 4);
            size_t n = (size_t(p[pos + 4]) << 24) | (size_t(p[pos + 5]) << 16) | (size_t(p[pos + 6]) << 8) | p[pos + 7];
            pos += 8;
            if (n > len - pos) break;
            const char* v = reinterpret_cast<const char*>(p + pos);
            if (tag == "mlit" || tag == "mlcl") ParseDmap(p + pos, n, np, depth + 1);
            else if (tag == "minm") np.title.assign(v, n);
            else if (tag == "asar") np.artist.assign(v, n);
            else if (tag == "asal") np.album.assign(v, n);
            pos += n;
        }
    }
}

void AirPlayService::OnMetadata(const unsigned char* data, int len)
{
    if (!data || len <= 0) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        NowPlaying np = m_nowPlaying;
        np.title.clear();
        np.artist.clear();
        np.album.clear();
        ParseDmap(data, static_cast<size_t>(len), np);
        m_nowPlaying = std::move(np);
        LOGI("now playing: \"%s\" by \"%s\" (%s)", m_nowPlaying.title.c_str(), m_nowPlaying.artist.c_str(),
             m_nowPlaying.album.c_str());
    }
    NotifyNowPlaying();
}

void AirPlayService::OnCoverArt(const unsigned char* data, int len)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (data && len > 0) m_nowPlaying.coverArt.assign(data, data + len);
        else m_nowPlaying.coverArt.clear();
        m_nowPlaying.coverArtVersion++;
    }
    LOGI("cover art received (%d bytes)", len);
    NotifyNowPlaying();
}

void AirPlayService::OnStopCoverArt()
{
    // sent when the client tears down its audio stream
    EndSpeakerMode();
}

void AirPlayService::OnProgress(uint32_t start, uint32_t current, uint32_t end)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // RTP timestamps count samples at 44.1 kHz
        m_nowPlaying.duration = static_cast<uint32_t>(end - start) / 44100.0;
        m_nowPlaying.position = static_cast<uint32_t>(current - start) / 44100.0;
        m_nowPlaying.progressTick = GetTickCount64();
    }
    NotifyNowPlaying();
}

// ---------------- AirPlay video (URL / HLS casting) ----------------

void AirPlayService::OnVideoPlay(const char* location, float startPosition)
{
    if (!location) return;
    m_video.Stop();  // casting replaces any mirrored picture and its audio
    m_audio.Stop();
    EndSpeakerMode();
    m_cast.Play(location, startPosition, [this](const std::string& path) { return HlsPlaylist(path); });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.casting = true;
        m_status.mirroring = false;
    }
    NotifyStatus();
}

void AirPlayService::OnVideoScrub(float position) { m_cast.Seek(position); }

void AirPlayService::OnVideoRate(float rate) { m_cast.SetRate(rate); }

void AirPlayService::OnVideoStop()
{
    LOGI("client stopped the video");
    m_cast.Stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.casting = false;
    }
    NotifyStatus();
}

void AirPlayService::OnPlaybackInfo(void* playbackInfo)
{
    auto* info = static_cast<playback_info_t*>(playbackInfo);
    {
        // while casting, the client's once-a-second /playback-info poll is its heartbeat
        std::lock_guard<std::mutex> lock(m_mutex);
        m_missedFeedback = 0;
    }
    CastPlayer::Info ci = m_cast.GetInfo();
    info->stallcount = 0;
    info->seek_start = 0.0;
    if (ci.ended)
    {
        // playback failed: this tells the library the video is over, so it ends the session and
        // the client stops (a video that plays to its end is reported as paused at its end instead)
        info->duration = -1.0;
        info->position = -1.0;
        return;
    }
    if (!ci.available)
    {
        info->duration = 0.0;
        info->position = -1.0;  // "not available yet"
        return;
    }
    info->duration = ci.duration;
    info->position = ci.position;
    info->seek_duration = ci.duration;
    info->rate = ci.rate;
    info->ready_to_play = ci.ready;
    info->playback_buffer_empty = ci.bufferEmpty;
    info->playback_buffer_full = !ci.bufferEmpty;
    info->playback_likely_to_keep_up = ci.likelyToKeepUp;
}

float AirPlayService::OnPlaylistRemove() { return m_cast.Remove(); }
