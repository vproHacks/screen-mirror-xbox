#include "pch.h"
#include "Log.h"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    FILE* g_file = nullptr;
    size_t g_fileBytes = 0;
    std::wstring g_path;
    std::deque<std::string> g_recent;
    std::function<void()> g_listener;
    constexpr size_t kMaxRecent = 200;
    constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;

    const char* LevelName(Log::Level level)
    {
        switch (level)
        {
        case Log::Level::Debug: return "D";
        case Log::Level::Info: return "I";
        case Log::Level::Warn: return "W";
        default: return "E";
        }
    }
}

void Log::Init(const std::wstring& folder)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    g_path = folder + L"\\airplay.log";
    std::wstring prev = folder + L"\\airplay.prev.log";
    _wremove(prev.c_str());
    _wrename(g_path.c_str(), prev.c_str());
    g_file = _wfopen(g_path.c_str(), L"wb");
}

void Log::SetListener(std::function<void()> listener)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_listener = std::move(listener);
}

void Log::WriteRaw(Level level, const std::string& text)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d %s ", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, LevelName(level));

    std::string line = stamp + text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    std::function<void()> listener;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file)
        {
            if (g_fileBytes > kMaxFileBytes)
            {
                // keep the log bounded: start over (the previous run is still in airplay.prev.log)
                fclose(g_file);
                g_file = _wfopen(g_path.c_str(), L"wb");
                g_fileBytes = 0;
            }
            if (g_file)
            {
                g_fileBytes += fwrite(line.data(), 1, line.size(), g_file);
                g_fileBytes += fwrite("\r\n", 1, 2, g_file);
                fflush(g_file);
            }
        }
        if (level != Level::Debug)
        {
            g_recent.push_back(line);
            while (g_recent.size() > kMaxRecent) g_recent.pop_front();
            listener = g_listener;
        }
    }
    OutputDebugStringA((line + "\n").c_str());
    if (listener) listener();
}

void Log::Write(Level level, const char* fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    WriteRaw(level, buf);
}

std::vector<std::string> Log::Recent(size_t maxLines)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    size_t n = g_recent.size() < maxLines ? g_recent.size() : maxLines;
    return std::vector<std::string>(g_recent.end() - n, g_recent.end());
}

// ---------------------------------------------------------------- crash reports

namespace
{
    uintptr_t g_imageBase = 0;
    uintptr_t g_imageEnd = 0;
    std::atomic<bool> g_crashing{false};

    std::string Where(uintptr_t pc)
    {
        char buf[48];
        if (pc >= g_imageBase && pc < g_imageEnd)
            snprintf(buf, sizeof(buf), "exe+0x%llx", static_cast<unsigned long long>(pc - g_imageBase));
        else
            snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(pc));
        return buf;
    }

    // Writes straight to the file: no UI listener, and no waiting on the log lock, which the
    // crashing thread may hold
    void CrashLine(const std::string& text)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d E ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::string line = stamp + text + "\r\n";
        bool locked = g_mutex.try_lock();
        if (g_file)
        {
            fwrite(line.data(), 1, line.size(), g_file);
            fflush(g_file);
        }
        if (locked) g_mutex.unlock();
        OutputDebugStringA(line.c_str());
    }

    // x64 unwind from a thread context, using the functions' unwind data
    std::string Stack(CONTEXT ctx)
    {
        std::string s;
        for (int i = 0; i < 24 && ctx.Rip; i++)
        {
            s += (i ? " < " : "") + Where(static_cast<uintptr_t>(ctx.Rip));
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (fn)
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
            else
            {
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);  // leaf function: return address on top
                ctx.Rsp += 8;
            }
        }
        return s;
    }

    std::string CurrentStack()
    {
        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        return Stack(ctx);
    }

    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep)
    {
        if (g_crashing.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "CRASH: exception 0x%08lx at %s on thread %lu", er->ExceptionCode,
                         Where(reinterpret_cast<uintptr_t>(er->ExceptionAddress)).c_str(), GetCurrentThreadId());
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            snprintf(buf + n, sizeof(buf) - n, " (%s 0x%llx)",
                     er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                     static_cast<unsigned long long>(er->ExceptionInformation[1]));
        CrashLine(buf);  // before walking a possibly broken stack
        CrashLine("CRASH: stack " + Stack(*ep->ContextRecord));
        return EXCEPTION_CONTINUE_SEARCH;  // then the system's crash handling (and dump, if enabled)
    }

    void OnTerminate()
    {
        if (!g_crashing.exchange(true))
        {
            std::string what = "no active exception";
            if (std::exception_ptr e = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(e);
                }
                catch (winrt::hresult_error const& x)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "hresult 0x%08x ", static_cast<uint32_t>(x.code()));
                    what = buf + winrt::to_string(x.message());
                }
                catch (std::exception const& x)
                {
                    what = x.what();
                }
                catch (...)
                {
                    what = "unknown exception type";
                }
            }
            CrashLine("CRASH: std::terminate (" + what + ") on thread " + std::to_string(GetCurrentThreadId()));
            CrashLine("CRASH: stack " + CurrentStack());
        }
        abort();
    }

    void OnAbort(int)
    {
        if (g_crashing.exchange(true)) return;
        CrashLine("CRASH: abort() on thread " + std::to_string(GetCurrentThreadId()));
        CrashLine("CRASH: stack " + CurrentStack());
    }

    void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
    {
        if (!g_crashing.exchange(true))
        {
            CrashLine("CRASH: invalid parameter passed to a C runtime function, on thread " +
                      std::to_string(GetCurrentThreadId()));
            CrashLine("CRASH: stack " + CurrentStack());
        }
        abort();
    }
}

void Log::InstallCrashHandlers()
{
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_imageBase = reinterpret_cast<uintptr_t>(base);
    g_imageEnd = g_imageBase + nt->OptionalHeader.SizeOfImage;
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    signal(SIGABRT, OnAbort);
    _set_invalid_parameter_handler(OnInvalidParameter);
}
