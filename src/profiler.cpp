#include <helix/profiler/profiler.h>
#include <helix/core/strings/format.h>
#include <helix/platform/file_system.h>
#include <helix/platform/thread.h>
#include <helix/platform/time.h>

#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#if HX_COMPILER_MSVC
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace helix::profiler
{
    namespace detail
    {
        std::atomic<bool> capturing{false};
    }

    namespace
    {
        constexpr char kMagic[8] = {'H', 'X', 'P', 'R', 'O', 'F', '0', '1'};

        constexpr u32 kChunkEvents = 8192;

        // Only the owner thread appends; readers see a consistent prefix through the release store of count.
        struct EventChunk
        {
            CaptureEvent events[kChunkEvents];
            std::atomic<u32> count{0};
            std::atomic<EventChunk*> next{nullptr};
        };

        std::atomic<u32> generation{0};

        HX_FORCE_INLINE u64 FastTicks()
        {
#if defined(_M_X64) || defined(__x86_64__)
            return __rdtsc();
#elif defined(__aarch64__)
            u64 v;
            asm volatile("mrs %0, cntvct_el0" : "=r"(v));
            return v;
#else
            return platform::Ticks();
#endif
        }

        u64 Calibrate()
        {
#if defined(_M_X64) || defined(__x86_64__)
            u64 t0 = platform::Ticks(), c0 = FastTicks();
            platform::SleepSeconds(0.02);
            u64 t1 = platform::Ticks(), c1 = FastTicks();
            f64 seconds = f64(t1 - t0) / f64(platform::TicksPerSecond());
            return u64(f64(c1 - c0) / seconds);
#elif defined(__aarch64__)
            u64 f;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
            return f;
#else
            return platform::TicksPerSecond();
#endif
        }

        u64 TicksPerSecond()
        {
            static u64 value = Calibrate();
            return value;
        }

        struct ThreadBuffer
        {
            u64 id = 0;
            String name;
            platform::SpinLock lock;
            EventChunk* head = nullptr;
            EventChunk* tail = nullptr;
            std::atomic<u32> generation{0};
            Array<CaptureSample> samples;
        };

        void ResetChunks(ThreadBuffer& b)
        {
            for (EventChunk* c = b.head; c; c = c->next.load(std::memory_order_relaxed))
                c->count.store(0, std::memory_order_relaxed);
            b.tail = b.head;
        }

        HX_FORCE_INLINE void Append(ThreadBuffer& b, CaptureEvent e)
        {
            u32 g = generation.load(std::memory_order_relaxed);
            if (HX_UNLIKELY(b.generation.load(std::memory_order_relaxed) != g))
            {
                ResetChunks(b);
                b.generation.store(g, std::memory_order_release);
            }
            EventChunk* c = b.tail;
            u32 n = c->count.load(std::memory_order_relaxed);
            if (HX_UNLIKELY(n == kChunkEvents))
            {
                EventChunk* next = c->next.load(std::memory_order_relaxed);
                if (!next)
                {
                    next = New<EventChunk>(HeapAllocator());
                    c->next.store(next, std::memory_order_release);
                }
                b.tail = c = next;
                n = 0;
            }
            c->events[n] = e;
            c->count.store(n + 1, std::memory_order_release);
        }

        template <typename F>
        void ForEachEvent(ThreadBuffer& b, F&& f)
        {
            if (b.generation.load(std::memory_order_acquire) != generation.load(std::memory_order_relaxed))
                return;
            for (EventChunk* c = b.head; c; c = c->next.load(std::memory_order_acquire))
            {
                u32 n = c->count.load(std::memory_order_acquire);
                for (u32 i = 0; i < n; i++)
                    f(c->events[i]);
                if (n < kChunkEvents)
                    break;
            }
        }

        u32 CountEvents(ThreadBuffer& b)
        {
            u32 n = 0;
            ForEachEvent(b, [&](const CaptureEvent&) { n++; });
            return n;
        }

        struct Registry
        {
            platform::Mutex mutex;
            Array<Marker*> markers;
            Array<Counter*> counters;
            Array<ThreadBuffer*> threads;
            Array<u64> frames;
        };

        Registry& R()
        {
            static Registry* registry = New<Registry>(HeapAllocator());
            return *registry;
        }

        thread_local ThreadBuffer* localBuffer = nullptr;

        ThreadBuffer& Local()
        {
            if (!localBuffer)
            {
                localBuffer = New<ThreadBuffer>(HeapAllocator());
                localBuffer->id = platform::CurrentThreadId();
                localBuffer->name = Format("thread {}", localBuffer->id);
                localBuffer->head = localBuffer->tail = New<EventChunk>(HeapAllocator());
                localBuffer->generation.store(generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
                platform::ScopedLock lock(R().mutex);
                R().threads.Push(localBuffer);
            }
            return *localBuffer;
        }

        template <typename T>
        u32 Register(Array<T*>& list, T* item, std::atomic<u32>& id)
        {
            u32 current = id.load(std::memory_order_acquire);
            if (current)
                return current;
            platform::ScopedLock lock(R().mutex);
            current = id.load(std::memory_order_relaxed);
            if (!current)
            {
                list.Push(item);
                current = u32(list.Size());
                id.store(current, std::memory_order_release);
            }
            return current;
        }

        struct Writer
        {
            Array<u8> bytes;
            void Raw(const void* p, usize n)
            {
                usize at = bytes.Size();
                bytes.Resize(at + n);
                std::memcpy(bytes.Data() + at, p, n);
            }
            template <typename T>
            void Put(T v) { Raw(&v, sizeof(T)); }
            void Text(StringView s)
            {
                Put(u16(s.Size()));
                Raw(s.Data(), s.Size());
            }
        };

        struct Reader
        {
            const u8* p;
            const u8* end;
            bool ok = true;
            void Raw(void* out, usize n)
            {
                if (usize(end - p) < n)
                {
                    ok = false;
                    std::memset(out, 0, n);
                    return;
                }
                std::memcpy(out, p, n);
                p += n;
            }
            template <typename T>
            T Get()
            {
                T v;
                Raw(&v, sizeof(T));
                return v;
            }
            String Text()
            {
                u16 n = Get<u16>();
                if (usize(end - p) < n)
                {
                    ok = false;
                    return String();
                }
                String s(StringView(reinterpret_cast<const char*>(p), n));
                p += n;
                return s;
            }
        };

        void AppendJsonString(String& out, StringView s)
        {
            out.Append('"');
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                    out.Append('\\');
                out.Append(u8(c) < 32 ? ' ' : c);
            }
            out.Append('"');
        }
    }

    u32 Marker::Id() { return Register(R().markers, this, id_); }
    u32 Counter::Id() { return Register(R().counters, this, id_); }

    void Counter::Set(f64 value)
    {
        if (!IsCapturing())
            return;
        u32 id = Id();
        ThreadBuffer& b = Local();
        b.lock.Lock();
        b.samples.Push({FastTicks(), id, value});
        b.lock.Unlock();
    }

    namespace detail
    {
        void Begin(Marker& marker) { Append(Local(), {FastTicks(), marker.Id(), EventKind::Begin}); }
        void End(Marker& marker) { Append(Local(), {FastTicks(), marker.Id(), EventKind::End}); }
    }

    void SetThreadName(const char* name)
    {
        ThreadBuffer& b = Local();
        b.lock.Lock();
        b.name = name;
        b.lock.Unlock();
    }

    void BeginCapture()
    {
        TicksPerSecond();
        platform::ScopedLock lock(R().mutex);
        for (ThreadBuffer* t : R().threads)
        {
            t->lock.Lock();
            t->samples.Clear();
            t->lock.Unlock();
        }
        generation.fetch_add(1, std::memory_order_acq_rel);
        R().frames.Clear();
        detail::capturing.store(true, std::memory_order_release);
    }

    void EndCapture() { detail::capturing.store(false, std::memory_order_release); }

    void MarkFrame()
    {
        if (!IsCapturing())
            return;
        platform::ScopedLock lock(R().mutex);
        R().frames.Push(FastTicks());
    }

    bool SaveCapture(StringView path)
    {
        Writer w;
        w.Raw(kMagic, sizeof(kMagic));
        w.Put(TicksPerSecond());
        platform::ScopedLock lock(R().mutex);
        w.Put(u32(R().markers.Size()));
        for (usize i = 0; i < R().markers.Size(); i++)
        {
            w.Put(u32(i + 1));
            w.Text(R().markers[i]->Name());
            w.Text(R().markers[i]->File());
            w.Put(R().markers[i]->Line());
        }
        w.Put(u32(R().counters.Size()));
        for (usize i = 0; i < R().counters.Size(); i++)
        {
            w.Put(u32(i + 1));
            w.Text(R().counters[i]->Name());
        }
        w.Put(u32(R().threads.Size()));
        for (ThreadBuffer* t : R().threads)
        {
            t->lock.Lock();
            w.Put(t->id);
            w.Text(t->name);
            w.Put(CountEvents(*t));
            ForEachEvent(*t, [&](const CaptureEvent& e) {
                w.Put(e.ticks);
                w.Put(e.marker);
                w.Put(u8(e.kind));
            });
            w.Put(u32(t->samples.Size()));
            for (const CaptureSample& s : t->samples)
            {
                w.Put(s.ticks);
                w.Put(s.counter);
                w.Put(s.value);
            }
            t->lock.Unlock();
        }
        w.Put(u32(R().frames.Size()));
        for (u64 f : R().frames)
            w.Put(f);
        return platform::WriteFile(path, w.bytes);
    }

    bool LoadCapture(StringView path, Capture& out)
    {
        Array<u8> data;
        if (!platform::ReadFile(path, data) || data.Size() < sizeof(kMagic) || std::memcmp(data.Data(), kMagic, sizeof(kMagic)) != 0)
            return false;
        Reader r{data.Data() + sizeof(kMagic), data.Data() + data.Size()};
        out = Capture();
        out.ticksPerSecond = r.Get<u64>();
        u32 markers = r.Get<u32>();
        for (u32 i = 0; i < markers && r.ok; i++)
        {
            CaptureMarker& m = out.markers.Emplace();
            m.id = r.Get<u32>();
            m.name = r.Text();
            m.file = r.Text();
            m.line = r.Get<u32>();
        }
        u32 counters = r.Get<u32>();
        for (u32 i = 0; i < counters && r.ok; i++)
        {
            CaptureMarker& c = out.counters.Emplace();
            c.id = r.Get<u32>();
            c.name = r.Text();
        }
        u32 threads = r.Get<u32>();
        for (u32 i = 0; i < threads && r.ok; i++)
        {
            CaptureThread& t = out.threads.Emplace();
            t.id = r.Get<u64>();
            t.name = r.Text();
            u32 events = r.Get<u32>();
            t.events.Reserve(events);
            for (u32 k = 0; k < events && r.ok; k++)
            {
                CaptureEvent e;
                e.ticks = r.Get<u64>();
                e.marker = r.Get<u32>();
                e.kind = EventKind(r.Get<u8>());
                t.events.Push(e);
            }
            u32 samples = r.Get<u32>();
            for (u32 k = 0; k < samples && r.ok; k++)
            {
                CaptureSample s;
                s.ticks = r.Get<u64>();
                s.counter = r.Get<u32>();
                s.value = r.Get<f64>();
                t.samples.Push(s);
            }
        }
        u32 frames = r.Get<u32>();
        for (u32 i = 0; i < frames && r.ok; i++)
            out.frames.Push(r.Get<u64>());
        return r.ok;
    }

    const CaptureMarker* Capture::FindMarker(StringView name) const
    {
        for (const CaptureMarker& m : markers)
            if (m.name == name)
                return &m;
        return nullptr;
    }

    bool SaveChromeTrace(StringView path)
    {
        String out("{\"traceEvents\":[");
        bool first = true;
        f64 toMicro = 1e6 / f64(TicksPerSecond());
        platform::ScopedLock lock(R().mutex);
        for (ThreadBuffer* t : R().threads)
        {
            t->lock.Lock();
            FormatAppend(out, "{}{{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":{},\"args\":{{\"name\":", first ? "" : ",", t->id);
            AppendJsonString(out, t->name);
            out.Append("}}");
            first = false;
            ForEachEvent(*t, [&](const CaptureEvent& e) {
                out.Append(",{\"name\":");
                AppendJsonString(out, R().markers[e.marker - 1]->Name());
                FormatAppend(out, ",\"ph\":\"{}\",\"ts\":{:.3f},\"pid\":1,\"tid\":{}}}", e.kind == EventKind::Begin ? "B" : "E", f64(e.ticks) * toMicro, t->id);
            });
            for (const CaptureSample& s : t->samples)
            {
                out.Append(",{\"name\":");
                AppendJsonString(out, R().counters[s.counter - 1]->Name());
                FormatAppend(out, ",\"ph\":\"C\",\"ts\":{:.3f},\"pid\":1,\"args\":{{\"value\":{}}}}}", f64(s.ticks) * toMicro, s.value);
            }
            t->lock.Unlock();
        }
        out.Append("]}");
        return platform::WriteFile(path, Span<const u8>(reinterpret_cast<const u8*>(out.Data()), out.Size()));
    }
}
