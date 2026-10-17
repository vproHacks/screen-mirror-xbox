#include "pch.h"
#include "AirPlayService.h"
#include "AudioRenderer.h"
#include "CastPlayer.h"
#include "Log.h"
#include "VideoRenderer.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

namespace
{
    constexpr wchar_t kDefaultName[] = L"Xbox One";

    std::string ToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring ToWide(std::string_view s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    // Receiver name: LocalState\name.txt if present (one line, UTF-8), else "Xbox One" on a console
    // and "<computer name> (PC)" elsewhere, so a PC test copy never shows up as a second "Xbox One".
    std::wstring LoadReceiverName(std::wstring const& folder)
    {
        std::wstring name = kDefaultName;
        try
        {
            using winrt::Windows::System::Profile::AnalyticsInfo;
            if (AnalyticsInfo::VersionInfo().DeviceFamily() != L"Windows.Xbox")
            {
                for (auto const& host : winrt::Windows::Networking::Connectivity::NetworkInformation::GetHostNames())
                {
                    if (host.Type() == winrt::Windows::Networking::HostNameType::DomainName)
                    {
                        std::wstring h(host.DisplayName());
                        name = h.substr(0, h.find(L'.')) + L" (PC)";
                        break;
                    }
                }
            }
        }
        catch (...)
        {
        }
        if (FILE* f = _wfopen((folder + L"\\name.txt").c_str(), L"rb"))
        {
            char buf[128] = {};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            std::string s(buf, n);
            if (s.size() >= 3 && (unsigned char) s[0] == 0xEF) s = s.substr(3);  // BOM
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) name = ToWide(s);
        }
        return name;
    }

    SolidColorBrush MakeBrush(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return SolidColorBrush(ColorHelper::FromArgb(a, r, g, b));
    }

    TextBlock MakeText(double size, SolidColorBrush const& brush)
    {
        TextBlock t;
        t.FontSize(size);
        t.Foreground(brush);
        t.TextTrimming(TextTrimming::CharacterEllipsis);
        return t;
    }

    MediaPlayerElement MakeVideoElement(winrt::Windows::Media::Playback::MediaPlayer const& player)
    {
        MediaPlayerElement e;
        e.SetMediaPlayer(player);
        e.Stretch(Stretch::Uniform);
        e.AreTransportControlsEnabled(false);
        e.HorizontalAlignment(HorizontalAlignment::Stretch);
        e.VerticalAlignment(VerticalAlignment::Stretch);
        e.Visibility(Visibility::Collapsed);
        return e;
    }

    std::wstring FormatTime(double seconds)
    {
        if (seconds < 0) seconds = 0;
        int s = static_cast<int>(seconds + 0.5);
        wchar_t buf[32];
        if (s >= 3600) swprintf(buf, 32, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
        else swprintf(buf, 32, L"%d:%02d", s / 60, s % 60);
        return buf;
    }
}

struct App : ApplicationT<App>
{
    App()
    {
        // no Xbox "mouse mode" cursor: the app has nothing to point at
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        Suspending({this, &App::OnSuspending});
        Resuming({this, &App::OnResuming});
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
            LOGE("unhandled exception: 0x%08x %ls", static_cast<uint32_t>(e.Exception().value), e.Message().c_str());
        });
    }

    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        Window window = Window::Current();
        if (window.Content())
        {
            window.Activate();
            return;
        }

        m_stateFolder = std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        Log::Init(m_stateFolder);
        Log::InstallCrashHandlers();
        LOGI("Screen Mirror Server starting (state folder %ls)", m_stateFolder.c_str());
        m_name = LoadReceiverName(m_stateFolder);
        // the recent-log lines on the idle screen are a diagnostic aid: shown only with LocalState\debug.txt
        m_showLog = GetFileAttributesW((m_stateFolder + L"\\debug.txt").c_str()) != INVALID_FILE_ATTRIBUTES;

        // use the whole TV picture (no Xbox overscan "safe area" margins)
        ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
        try
        {
            ApplicationView::GetForCurrentView().TryEnterFullScreenMode();
        }
        catch (...)
        {
        }

        BuildUi();
        window.Content(m_root);
        window.Activate();
        m_dispatcher = window.Dispatcher();

        if (!m_audio.Initialize()) LOGE("audio output unavailable; AirPlay audio will be silent");

        m_video.ActiveChanged = [this](bool active) {
            RunOnUi([this, active] {
                m_mirrorActive = active;
                UpdateView();
            });
        };
        m_cast.ActiveChanged = [this](bool active) {
            RunOnUi([this, active] {
                m_castActive = active;
                UpdateView();
            });
        };
        m_service.StatusChanged = [this] { RunOnUi([this] { UpdateView(); }); };
        m_service.NowPlayingChanged = [this] { RunOnUi([this] { RefreshNowPlaying(); }); };
        Log::SetListener([this] { RunOnUi([this] { RefreshLog(); }); });

        StartService();

        // refresh the status line and the now-playing progress
        m_timer.Interval(std::chrono::seconds(1));
        m_timer.Tick([this](IInspectable const&, IInspectable const&) {
            RefreshStatus();
            RefreshProgress();
        });
        m_timer.Start();
    }

    void StartService()
    {
        // network start-up (sockets, mDNS) happens off the UI thread
        std::thread([this] {
            if (!m_service.Start(ToUtf8(m_name), m_stateFolder))
                LOGE("the AirPlay service did not start");
        }).detach();
    }

    void OnSuspending(IInspectable const&, SuspendingEventArgs const& e)
    {
        auto deferral = e.SuspendingOperation().GetDeferral();
        LOGI("app suspending: stopping the AirPlay service");
        m_service.Stop();
        deferral.Complete();
    }

    void OnResuming(IInspectable const&, IInspectable const&)
    {
        LOGI("app resuming: restarting the AirPlay service");
        StartService();
    }

    template <typename F>
    void RunOnUi(F&& f)
    {
        if (!m_dispatcher) return;
        m_dispatcher.RunAsync(CoreDispatcherPriority::Normal, DispatchedHandler(std::forward<F>(f)));
    }

    void BuildUi()
    {
        m_root = Grid();
        m_root.Background(MakeBrush(0, 0, 0));

        m_videoElement = MakeVideoElement(m_video.Player());  // screen mirroring
        m_root.Children().Append(m_videoElement);
        m_castElement = MakeVideoElement(m_cast.Player());    // AirPlay video (URL/HLS)
        m_root.Children().Append(m_castElement);

        BuildNowPlaying();
        m_root.Children().Append(m_nowPlaying);

        // idle / status overlay
        m_overlay = Grid();
        m_overlay.Background(MakeBrush(16, 18, 24));
        m_root.Children().Append(m_overlay);

        StackPanel center;
        center.HorizontalAlignment(HorizontalAlignment::Center);
        center.VerticalAlignment(VerticalAlignment::Center);
        center.Spacing(12);
        m_overlay.Children().Append(center);

        TextBlock title = MakeText(28, MakeBrush(150, 160, 175));
        title.Text(L"Screen Mirror Server");
        title.HorizontalAlignment(HorizontalAlignment::Center);
        center.Children().Append(title);

        m_nameText = MakeText(64, MakeBrush(255, 255, 255));
        m_nameText.Text(m_name);
        m_nameText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        m_nameText.HorizontalAlignment(HorizontalAlignment::Center);
        center.Children().Append(m_nameText);

        m_hintText = MakeText(24, MakeBrush(200, 205, 215));
        m_hintText.Text(L"On your iPhone or iPad: open Control Center → Screen Mirroring → " + m_name +
                        L"\nor choose " + m_name + L" with the AirPlay button in a video or music app");
        m_hintText.HorizontalAlignment(HorizontalAlignment::Center);
        m_hintText.TextWrapping(TextWrapping::Wrap);
        m_hintText.TextAlignment(TextAlignment::Center);
        m_hintText.MaxWidth(1400);
        center.Children().Append(m_hintText);

        m_statusText = MakeText(24, MakeBrush(120, 200, 140));
        m_statusText.Text(L"Starting…");
        m_statusText.HorizontalAlignment(HorizontalAlignment::Center);
        m_statusText.TextAlignment(TextAlignment::Center);
        m_statusText.Margin(ThicknessHelper::FromLengths(0, 24, 0, 0));
        center.Children().Append(m_statusText);

        m_logText = MakeText(14, MakeBrush(110, 115, 125));
        m_logText.FontFamily(FontFamily(L"Consolas"));
        m_logText.TextWrapping(TextWrapping::NoWrap);
        m_logText.VerticalAlignment(VerticalAlignment::Bottom);
        m_logText.HorizontalAlignment(HorizontalAlignment::Left);
        m_logText.Margin(ThicknessHelper::FromLengths(48, 0, 48, 32));
        m_overlay.Children().Append(m_logText);
    }

    // speaker mode: cover art, title/artist/album and a progress bar, like a streaming box
    void BuildNowPlaying()
    {
        m_nowPlaying = Grid();
        m_nowPlaying.Background(MakeBrush(16, 18, 24));
        m_nowPlaying.Visibility(Visibility::Collapsed);

        // laid out on a 1600x900 canvas and scaled to the window (the Xbox renders XAML at 200%,
        // i.e. 960x540 effective pixels, a PC usually at 100-150%)
        Viewbox fit;
        fit.Stretch(Stretch::Uniform);
        Grid canvas;
        canvas.Width(1600);
        canvas.Height(900);
        fit.Child(canvas);
        m_nowPlaying.Children().Append(fit);

        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.HorizontalAlignment(HorizontalAlignment::Center);
        row.VerticalAlignment(VerticalAlignment::Center);
        row.Spacing(64);
        canvas.Children().Append(row);

        Border art;
        art.Width(480);
        art.Height(480);
        art.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
        art.Background(MakeBrush(40, 44, 54));
        m_coverArt = Image();
        m_coverArt.Stretch(Stretch::UniformToFill);
        art.Child(m_coverArt);
        row.Children().Append(art);

        StackPanel info;
        info.VerticalAlignment(VerticalAlignment::Center);
        info.Spacing(10);
        info.Width(820);
        row.Children().Append(info);

        m_npSource = MakeText(22, MakeBrush(150, 160, 175));
        info.Children().Append(m_npSource);
        m_npTitle = MakeText(54, MakeBrush(255, 255, 255));
        m_npTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        info.Children().Append(m_npTitle);
        m_npArtist = MakeText(34, MakeBrush(210, 215, 225));
        info.Children().Append(m_npArtist);
        m_npAlbum = MakeText(26, MakeBrush(150, 160, 175));
        info.Children().Append(m_npAlbum);

        m_npProgress = ProgressBar();
        m_npProgress.Minimum(0);
        m_npProgress.Maximum(1);
        m_npProgress.Height(6);
        m_npProgress.Margin(ThicknessHelper::FromLengths(0, 36, 0, 0));
        m_npProgress.Foreground(MakeBrush(235, 238, 245));
        m_npProgress.Background(MakeBrush(60, 64, 74));
        info.Children().Append(m_npProgress);
        m_npTime = MakeText(22, MakeBrush(150, 160, 175));
        info.Children().Append(m_npTime);
    }

    // Shows exactly one of: mirrored screen, cast video, now-playing panel, idle screen.
    void UpdateView()
    {
        auto s = m_service.GetStatus();
        bool showMirror = m_mirrorActive;
        bool showCast = !showMirror && m_castActive;
        bool showSpeaker = !showMirror && !showCast && s.speaker;
        m_videoElement.Visibility(showMirror ? Visibility::Visible : Visibility::Collapsed);
        m_castElement.Visibility(showCast ? Visibility::Visible : Visibility::Collapsed);
        m_nowPlaying.Visibility(showSpeaker ? Visibility::Visible : Visibility::Collapsed);
        m_overlay.Visibility(showMirror || showCast || showSpeaker ? Visibility::Collapsed : Visibility::Visible);

        // keep the TV from dimming while something is playing
        bool busy = showMirror || showCast || showSpeaker;
        try
        {
            if (busy && !m_displayRequested)
            {
                m_displayRequest.RequestActive();
                m_displayRequested = true;
            }
            else if (!busy && m_displayRequested)
            {
                m_displayRequest.RequestRelease();
                m_displayRequested = false;
            }
        }
        catch (...)
        {
        }
        RefreshStatus();
        if (showSpeaker) RefreshNowPlaying();
        else RefreshLog();
    }

    void RefreshStatus()
    {
        auto s = m_service.GetStatus();
        std::wstring client = s.client.empty() ? std::wstring(L"device") : ToWide(s.client);
        std::wstring text;
        if (!s.error.empty())
        {
            text = L"Error: " + ToWide(s.error);
            m_statusText.Foreground(MakeBrush(240, 110, 100));
        }
        else if (!s.running)
        {
            text = L"Starting…";
        }
        else
        {
            m_statusText.Foreground(MakeBrush(120, 200, 140));
            if (s.mirroring) text = L"Mirroring from " + client + L"…";
            else if (s.casting) text = L"Loading video from " + client + L"…";
            else if (s.connections > 0) text = L"Connected to " + client;
            else text = L"Ready — " + ToWide(s.address) + L":" + std::to_wstring(s.port);
        }
        m_statusText.Text(text);
    }

    void RefreshNowPlaying()
    {
        auto s = m_service.GetStatus();
        if (!s.speaker)
        {
            if (m_nowPlaying.Visibility() == Visibility::Visible) UpdateView();
            return;
        }
        if (m_nowPlaying.Visibility() != Visibility::Visible)
        {
            UpdateView();  // calls back here once the panel is showing
            return;
        }
        auto np = m_service.GetNowPlaying();
        m_npSource.Text(L"Playing from " + (s.client.empty() ? std::wstring(L"AirPlay") : ToWide(s.client)));
        m_npTitle.Text(np.title.empty() ? std::wstring(L"AirPlay audio") : ToWide(np.title));
        m_npArtist.Text(ToWide(np.artist));
        m_npAlbum.Text(ToWide(np.album));
        if (np.coverArtVersion != m_coverArtVersion)
        {
            m_coverArtVersion = np.coverArtVersion;
            LoadCoverArt(np.coverArt);
        }
        RefreshProgress();
    }

    void RefreshProgress()
    {
        if (m_nowPlaying.Visibility() != Visibility::Visible) return;
        auto np = m_service.GetNowPlaying();
        if (np.position < 0 || np.duration <= 0)
        {
            m_npProgress.Visibility(Visibility::Collapsed);
            m_npTime.Text(L"");
            return;
        }
        double pos = np.position + (GetTickCount64() - np.progressTick) / 1000.0;
        if (pos > np.duration) pos = np.duration;
        m_npProgress.Visibility(Visibility::Visible);
        m_npProgress.Value(pos / np.duration);
        m_npTime.Text(FormatTime(pos) + L" / " + FormatTime(np.duration));
    }

    winrt::fire_and_forget LoadCoverArt(std::vector<uint8_t> bytes)
    {
        if (bytes.empty())
        {
            m_coverArt.Source(nullptr);
            co_return;
        }
        try
        {
            winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            winrt::Windows::Storage::Streams::DataWriter writer(stream);
            writer.WriteBytes(bytes);
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
            winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            m_coverArt.Source(bitmap);
        }
        catch (winrt::hresult_error const& e)
        {
            LOGW("could not show the cover art: %ls", e.message().c_str());
            m_coverArt.Source(nullptr);
        }
    }

    void RefreshLog()
    {
        if (!m_showLog) return;
        if (m_overlay.Visibility() != Visibility::Visible) return;
        auto lines = Log::Recent(10);
        std::wstring text;
        for (auto const& l : lines)
        {
            if (!text.empty()) text += L"\n";
            text += ToWide(l.size() > 160 ? l.substr(0, 160) : l);
        }
        m_logText.Text(text);
    }

    std::wstring m_stateFolder;
    std::wstring m_name;
    CoreDispatcher m_dispatcher{nullptr};
    Grid m_root{nullptr};
    Grid m_overlay{nullptr};
    MediaPlayerElement m_videoElement{nullptr};
    MediaPlayerElement m_castElement{nullptr};
    TextBlock m_nameText{nullptr};
    TextBlock m_hintText{nullptr};
    TextBlock m_statusText{nullptr};
    TextBlock m_logText{nullptr};
    Grid m_nowPlaying{nullptr};
    Image m_coverArt{nullptr};
    TextBlock m_npSource{nullptr}, m_npTitle{nullptr}, m_npArtist{nullptr}, m_npAlbum{nullptr}, m_npTime{nullptr};
    ProgressBar m_npProgress{nullptr};
    uint64_t m_coverArtVersion = 0;
    DispatcherTimer m_timer;
    winrt::Windows::System::Display::DisplayRequest m_displayRequest;
    bool m_displayRequested = false;
    bool m_mirrorActive = false;
    bool m_castActive = false;
    bool m_showLog = false;

    VideoRenderer m_video;
    AudioRenderer m_audio;
    CastPlayer m_cast;
    AirPlayService m_service{m_video, m_audio, m_cast};
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Application::Start([](auto&&) { make<App>(); });
    return 0;
}
