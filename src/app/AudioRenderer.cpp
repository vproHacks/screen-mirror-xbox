#include "pch.h"
#include "AudioRenderer.h"
#include "Log.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include "AudioDecoder.h"

#pragma comment(lib, "xaudio2.lib")

namespace
{
    constexpr UINT32 kSampleRate = 44100;
    constexpr UINT32 kChannels = 2;
    // queue limits in milliseconds of audio: start playing after the pre-roll, drop new frames
    // instead of letting latency grow past the cap
    struct Buffering
    {
        double prerollMs;
        double maxMs;
    };
    // mirroring audio accompanies live video, so keep it tight; speaker audio is not lip-synced
    // to anything on the TV, so give Wi-Fi jitter more room
    constexpr Buffering kMirrorBuffering{30.0, 250.0};
    constexpr Buffering kSpeakerBuffering{150.0, 600.0};

}

AudioRenderer::~AudioRenderer()
{
    Stop();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_master) m_master->DestroyVoice();
    m_master = nullptr;
    if (m_xaudio) m_xaudio->Release();
    m_xaudio = nullptr;
}

bool AudioRenderer::Initialize()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_xaudio) return true;
    HRESULT hr = XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        LOGE("XAudio2Create failed: 0x%08x", static_cast<uint32_t>(hr));
        return false;
    }
    hr = m_xaudio->CreateMasteringVoice(&m_master);
    if (FAILED(hr))
    {
        LOGE("CreateMasteringVoice failed: 0x%08x", static_cast<uint32_t>(hr));
        m_xaudio->Release();
        m_xaudio = nullptr;
        return false;
    }
    return true;
}

bool AudioRenderer::CreateVoiceLocked()
{
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    HRESULT hr = m_xaudio->CreateSourceVoice(&m_voice, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
    if (FAILED(hr))
    {
        LOGE("CreateSourceVoice failed: 0x%08x", static_cast<uint32_t>(hr));
        m_voice = nullptr;
        return false;
    }
    m_voice->SetVolume(m_gain);
    m_playing = false;
    return true;
}

void AudioRenderer::Start(unsigned char ct, unsigned short spf)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    StopLocked();
    m_ct = ct;
    m_spf = spf;
    m_warnedFormat = false;
    m_frames = m_dropped = m_decodeErrors = 0;
    if (!m_xaudio)
    {
        LOGE("audio output is not available");
        return;
    }

    if (ct != 2 && ct != 8)
    {
        LOGW("audio format ct=%u (spf %u) is not supported; expected ALAC (2) or AAC-ELD (8)", ct, spf);
        return;
    }
    m_decoder = new AudioDecoder();
    if (!(ct == 8 ? m_decoder->InitAacEld() : m_decoder->InitAlac(spf ? spf : 352)))
    {
        LOGE("%s decoder init failed", ct == 8 ? "AAC-ELD" : "ALAC");
        delete m_decoder;
        m_decoder = nullptr;
        return;
    }
    m_peak = 0;

    if (!CreateVoiceLocked()) return;
    LOGI("audio started: %s 44.1 kHz stereo", ct == 2 ? "ALAC" : "AAC-ELD");
}

void AudioRenderer::SubmitLocked(const int16_t* pcm, size_t samples, uint32_t frameSize)
{
    const Buffering& b = (m_ct == 2) ? kSpeakerBuffering : kMirrorBuffering;
    double frameMs = 1000.0 * frameSize / kSampleRate;

    XAUDIO2_VOICE_STATE state;
    m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    double queuedMs = state.BuffersQueued * frameMs;
    if (queuedMs >= b.maxMs)
    {
        m_dropped++;  // falling behind: drop rather than build up latency
        return;
    }

    auto* buf = new std::vector<int16_t>(pcm, pcm + samples);
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = static_cast<UINT32>(buf->size() * sizeof(int16_t));
    xb.pAudioData = reinterpret_cast<const BYTE*>(buf->data());
    xb.pContext = buf;
    if (FAILED(m_voice->SubmitSourceBuffer(&xb)))
    {
        delete buf;
        return;
    }
    if (!m_playing && queuedMs + frameMs >= b.prerollMs)
    {
        m_voice->Start(0);
        m_playing = true;
    }
}

void AudioRenderer::Push(const uint8_t* data, size_t len, unsigned char ct, uint64_t ntpLocal)
{
    (void) ntpLocal;  // played as soon as it arrives (UxPlay's default, no audio clock sync)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ct != m_ct || !m_voice || !m_decoder)
    {
        if (!m_warnedFormat)
        {
            m_warnedFormat = true;
            LOGW("dropping audio: ct=%u has no active decoder", ct);
        }
        return;
    }

    const char* codec = ct == 8 ? "AAC-ELD" : "ALAC";
    m_pcm.clear();
    int samples = m_decoder->Decode(data, len, m_pcm);
    if (samples < 0)
    {
        if (m_decodeErrors++ < 10) LOGW("%s decode error %d (%zu bytes)", codec, samples, len);
        return;
    }
    if (samples == 0) return;
    if (m_frames++ == 0) LOGI("first %s frame decoded: %d samples/frame", codec, samples);
    if (m_frames <= 200)
    {
        // signal level over the first ~2 s, as a sanity check that the decoder output is audio
        for (int16_t s : m_pcm) m_peak = std::max(m_peak, std::abs(static_cast<int>(s)));
        if (m_frames == 200) LOGI("%s level check: peak %d over the first 200 frames", codec, m_peak);
    }
    SubmitLocked(m_pcm.data(), m_pcm.size(), static_cast<uint32_t>(samples));
}

void AudioRenderer::OnBufferEnd(void* context) noexcept
{
    delete static_cast<std::vector<int16_t>*>(context);
}

void AudioRenderer::OnVoiceError(void*, HRESULT error) noexcept
{
    LOGE("XAudio2 voice error 0x%08x", static_cast<uint32_t>(error));
}

void AudioRenderer::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_voice)
    {
        m_voice->Stop(0);
        m_voice->FlushSourceBuffers();
        m_playing = false;
    }
}

void AudioRenderer::StopLocked()
{
    if (m_voice)
    {
        m_voice->Stop(0);
        m_voice->FlushSourceBuffers();
        m_voice->DestroyVoice();  // blocks until callbacks finish, so pending buffers are freed
        m_voice = nullptr;
        LOGI("audio stopped: %llu frames decoded, %llu dropped, %llu decode errors", (unsigned long long) m_frames,
             (unsigned long long) m_dropped, (unsigned long long) m_decodeErrors);
    }
    delete m_decoder;
    m_decoder = nullptr;
    m_playing = false;
    m_ct = 0;
}

void AudioRenderer::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    StopLocked();
}

float AudioRenderer::GainForAirPlayVolume(float volume)
{
    // -144 = mute, otherwise -30 dB (slider at 0) .. 0 dB (slider at max). As in UxPlay:
    // slider fraction f, each halving of f is -10 dB (but never below a flat dB scale).
    if (volume <= -30.0f) return 0.0f;
    if (volume >= 0.0f) return 1.0f;
    double frac = (30.0 + volume) / 30.0;
    double db = 10.0 * (std::log10(frac) / std::log10(2.0));
    double dbFlat = -30.0 * (1.0 - frac);
    if (db < dbFlat) db = dbFlat;
    return static_cast<float>(std::pow(10.0, 0.05 * db));
}

void AudioRenderer::SetVolumeDb(float volume)
{
    float gain = GainForAirPlayVolume(volume);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gain = gain;
    if (m_voice) m_voice->SetVolume(gain);
    LOGI("volume %.1f dB -> gain %.3f", volume, gain);
}
