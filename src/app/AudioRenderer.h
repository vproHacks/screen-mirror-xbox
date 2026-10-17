#pragma once

#include <cstdint>
#include <mutex>
#include <vector>
#include <xaudio2.h>

class AudioDecoder;

// Decodes AirPlay audio (with FFmpeg) and plays the PCM through XAudio2:
//   ct=8  AAC-ELD, 480 samples per frame (screen mirroring audio)
//   ct=2  Apple Lossless (ALAC), 352 samples per frame (AirPlay speaker / audio-only streaming)
// Both are 44.1 kHz stereo.
class AudioRenderer : public IXAudio2VoiceCallback
{
public:
    AudioRenderer() = default;
    ~AudioRenderer();

    bool Initialize();
    void Start(unsigned char ct, unsigned short spf);
    void Push(const uint8_t* data, size_t len, unsigned char ct, uint64_t ntpLocal);
    void Flush();
    void Stop();
    void SetVolumeDb(float airplayVolumeDb);

    // AirPlay volume (-144 = mute, -30 dB .. 0 dB slider) to a linear gain, as UxPlay maps it
    static float GainForAirPlayVolume(float airplayVolumeDb);

    // IXAudio2VoiceCallback
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override;

private:
    void StopLocked();
    bool CreateVoiceLocked();
    void SubmitLocked(const int16_t* pcm, size_t samples, uint32_t frameSize);

    std::mutex m_mutex;
    IXAudio2* m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SourceVoice* m_voice = nullptr;
    AudioDecoder* m_decoder = nullptr;
    unsigned short m_spf = 0;
    unsigned char m_ct = 0;
    bool m_warnedFormat = false;
    bool m_playing = false;
    float m_gain = 1.0f;
    uint64_t m_frames = 0;
    uint64_t m_dropped = 0;
    uint64_t m_decodeErrors = 0;
    std::vector<int16_t> m_pcm;
    int m_peak = 0;
};
