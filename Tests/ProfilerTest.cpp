#include <Helix/Profiler/Profiler.h>
#include <Helix/Core/Types/Guid.h>
#include <Helix/Core/Strings/Format.h>
#include <Helix/Platform/IO/FileSystem.h>
#include <Helix/Platform/Threading/Thread.h>
#include <Helix/Testing/Test.h>

using namespace helix;

namespace
{
    profiler::Counter frameCounter{"frame.value"};

    void Work(int depth)
    {
        HX_PROFILE_SCOPE("Work");
        if (depth > 0)
            Work(depth - 1);
    }

    String TempFile(StringView extension)
    {
        return platform::JoinPath(platform::TempDirectory(), Format("helix_profiler_{}{}", Guid::Generate().ToString(), extension));
    }
}

HX_TEST(Profiler, NothingRecordedWhenIdle)
{
    HX_CHECK(!profiler::IsCapturing());
    Work(3);
    profiler::BeginCapture();
    profiler::EndCapture();
    String path = TempFile(".hxprof");
    HX_REQUIRE(profiler::SaveCapture(path));
    profiler::Capture capture;
    HX_REQUIRE(profiler::LoadCapture(path, capture));
    usize events = 0;
    for (const auto& t : capture.threads)
        events += t.events.Size();
    HX_CHECK_EQ(events, 0u);
    platform::RemoveFile(path);
}

HX_TEST(Profiler, CaptureRoundTripAcrossThreads)
{
    profiler::BeginCapture();
    profiler::SetThreadName("main");
    for (int frame = 0; frame < 3; frame++)
    {
        profiler::MarkFrame();
        Work(2);
        frameCounter.Set(frame * 10.0);
    }
    if (platform::ThreadsSupported())
    {
        platform::Thread t;
        HX_REQUIRE(t.Start({.function = [](void*) {
                                profiler::SetThreadName("worker");
                                Work(4);
                            }}));
        t.Join();
    }
    profiler::EndCapture();
    Work(1);

    String path = TempFile(".hxprof");
    HX_REQUIRE(profiler::SaveCapture(path));
    profiler::Capture capture;
    HX_REQUIRE(profiler::LoadCapture(path, capture));
    platform::RemoveFile(path);

    const profiler::CaptureMarker* work = capture.FindMarker("Work");
    HX_REQUIRE(work != nullptr);
    HX_CHECK_EQ(capture.frames.Size(), 3u);
    HX_CHECK(capture.ticksPerSecond > 0);

    const profiler::CaptureThread* mainThread = nullptr;
    const profiler::CaptureThread* worker = nullptr;
    for (const auto& t : capture.threads)
    {
        if (t.name == "main")
            mainThread = &t;
        if (t.name == "worker")
            worker = &t;
    }
    HX_REQUIRE(mainThread != nullptr);
    HX_CHECK_EQ(mainThread->events.Size(), 18u);
    HX_CHECK_EQ(mainThread->samples.Size(), 3u);
    HX_CHECK_EQ(mainThread->samples[2].value, 20.0);
    int depth = 0, maxDepth = 0;
    u64 last = 0;
    for (const auto& e : mainThread->events)
    {
        HX_CHECK_EQ(e.marker, work->id);
        HX_CHECK(e.ticks >= last);
        last = e.ticks;
        depth += e.kind == profiler::EventKind::Begin ? 1 : -1;
        maxDepth = depth > maxDepth ? depth : maxDepth;
        HX_CHECK(depth >= 0);
    }
    HX_CHECK_EQ(depth, 0);
    HX_CHECK_EQ(maxDepth, 3);
    if (platform::ThreadsSupported())
    {
        HX_REQUIRE(worker != nullptr);
        HX_CHECK_EQ(worker->events.Size(), 10u);
    }

    String trace = TempFile(".json");
    HX_REQUIRE(profiler::SaveChromeTrace(trace));
    Array<u8> json;
    HX_REQUIRE(platform::ReadFile(trace, json));
    StringView text(reinterpret_cast<const char*>(json.Data()), json.Size());
    HX_CHECK(text.StartsWith("{\"traceEvents\":["));
    HX_CHECK(text.Contains("\"name\":\"Work\""));
    HX_CHECK(text.Contains("\"name\":\"frame.value\""));
    HX_CHECK(text.EndsWith("]}"));
    platform::RemoveFile(trace);
}

HX_TEST(Profiler, RejectsBadFiles)
{
    String path = TempFile(".hxprof");
    u8 junk[16] = {1, 2, 3};
    HX_REQUIRE(platform::WriteFile(path, Span<const u8>(junk, 16)));
    profiler::Capture capture;
    HX_CHECK(!profiler::LoadCapture(path, capture));
    platform::RemoveFile(path);
    HX_CHECK(!profiler::LoadCapture("helix_missing_capture.hxprof", capture));
}
