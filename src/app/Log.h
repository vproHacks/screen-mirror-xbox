#pragma once

#include <string>
#include <vector>
#include <functional>

// Thread-safe log: appends to LocalState\airplay.log and keeps recent lines for the on-screen view.
namespace Log
{
    enum class Level { Debug, Info, Warn, Error };

    void Init(const std::wstring& folder);
    void Write(Level level, const char* fmt, ...);
    void WriteRaw(Level level, const std::string& line);
    std::vector<std::string> Recent(size_t maxLines);
    // Called (on the logging thread) whenever a line at Info or above is added.
    void SetListener(std::function<void()> listener);
    // Writes crashes to the log: a native exception nobody handles, std::terminate (an exception
    // escaping a thread or a fire_and_forget coroutine), abort() and CRT parameter errors. Code
    // addresses are logged as "exe+0x<RVA>", to look up in the linker map
    // (build\x64\<config>\AirServerXbox.map). Fail-fast exits (heap corruption, /GS) bypass this.
    void InstallCrashHandlers();
}

#define LOGD(...) ::Log::Write(::Log::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::Log::Write(::Log::Level::Info, __VA_ARGS__)
#define LOGW(...) ::Log::Write(::Log::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::Log::Write(::Log::Level::Error, __VA_ARGS__)
