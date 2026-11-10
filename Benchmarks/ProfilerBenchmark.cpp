#include <Helix/Profiler/Profiler.h>
#include <Helix/Testing/Bench.h>

using namespace helix;

HX_BENCH(ScopeIdle)
{
    while (state.KeepRunning())
    {
        HX_PROFILE_SCOPE("Idle");
        helix::testing::ClobberMemory();
    }
}

HX_BENCH(ScopeCapturing)
{
    profiler::BeginCapture();
    while (state.KeepRunning())
    {
        HX_PROFILE_SCOPE("Capturing");
        helix::testing::ClobberMemory();
    }
    profiler::EndCapture();
    profiler::BeginCapture();
    profiler::EndCapture();
}
