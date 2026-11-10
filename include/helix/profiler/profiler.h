#pragma once

#include <helix/core/containers/array.h>
#include <helix/core/strings/string.h>

#include <atomic>

namespace helix::profiler
{
    class Marker
    {
    public:
        constexpr Marker(const char* name, const char* file = "", u32 line = 0) : name_(name), file_(file), line_(line) {}
        u32 Id();
        const char* Name() const { return name_; }
        const char* File() const { return file_; }
        u32 Line() const { return line_; }

    private:
        const char* name_;
        const char* file_;
        u32 line_;
        std::atomic<u32> id_{0};
    };

    class Counter
    {
    public:
        constexpr explicit Counter(const char* name) : name_(name) {}
        void Set(f64 value);
        u32 Id();
        const char* Name() const { return name_; }

    private:
        const char* name_;
        std::atomic<u32> id_{0};
    };

    namespace detail
    {
        extern std::atomic<bool> capturing;
        void Begin(Marker& marker);
        void End(Marker& marker);
    }

    inline bool IsCapturing() { return detail::capturing.load(std::memory_order_relaxed); }

    class Scope
    {
    public:
        explicit Scope(Marker& marker) : marker_(IsCapturing() ? &marker : nullptr)
        {
            if (marker_)
                detail::Begin(*marker_);
        }
        ~Scope()
        {
            if (marker_)
                detail::End(*marker_);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Marker* marker_;
    };

    void SetThreadName(const char* name);
    void BeginCapture();
    void EndCapture();
    void MarkFrame();
    bool SaveCapture(StringView path);
    bool SaveChromeTrace(StringView path);

    enum class EventKind : u8
    {
        Begin,
        End,
    };

    struct CaptureEvent
    {
        u64 ticks;
        u32 marker;
        EventKind kind;
    };

    struct CaptureSample
    {
        u64 ticks;
        u32 counter;
        f64 value;
    };

    struct CaptureThread
    {
        u64 id;
        String name;
        Array<CaptureEvent> events;
        Array<CaptureSample> samples;
    };

    struct CaptureMarker
    {
        u32 id;
        String name;
        String file;
        u32 line;
    };

    struct Capture
    {
        u64 ticksPerSecond = 0;
        Array<CaptureMarker> markers;
        Array<CaptureMarker> counters;
        Array<CaptureThread> threads;
        Array<u64> frames;

        const CaptureMarker* FindMarker(StringView name) const;
    };

    bool LoadCapture(StringView path, Capture& out);
}

#define HX_PROFILE_JOIN2(a, b) a##b
#define HX_PROFILE_JOIN(a, b) HX_PROFILE_JOIN2(a, b)
#define HX_PROFILE_SCOPE(name)                                                                                 \
    static ::helix::profiler::Marker HX_PROFILE_JOIN(hx_marker_, __LINE__){name, __FILE__, __LINE__};          \
    ::helix::profiler::Scope HX_PROFILE_JOIN(hx_scope_, __LINE__) { HX_PROFILE_JOIN(hx_marker_, __LINE__) }
