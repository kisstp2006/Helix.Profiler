# Helix.Profiler

CPU profiler of the Helix engine: static markers, scopes, counters and frame marks, recorded
into lock-free per-thread buffers and saved as a binary capture or as a Chrome trace.

## Usage

```cpp
#include <Helix/Profiler/Profiler.h>

helix::profiler::Counter drawCalls{"render.drawCalls"};

void Update()
{
    HX_PROFILE_SCOPE("Update");
    drawCalls.Set(120);
}

helix::profiler::BeginCapture();
// frames ...
helix::profiler::MarkFrame();
helix::profiler::EndCapture();
helix::profiler::SaveCapture("frame.hxprof");
helix::profiler::SaveChromeTrace("frame.json");
```

- A scope costs about 2 ns when no capture is running and about 27 ns while capturing (x64).
- Timestamps come from the CPU time stamp counter (x64) or the virtual counter (arm64), calibrated against the platform clock.
- Each thread appends to its own chunked buffer; saving only reads complete events.
- `LoadCapture` reads `.hxprof` files back into a `Capture` (markers, counters, threads, events, samples, frames).
- Chrome traces open in any viewer that reads the trace event JSON format.

## External tracks

`RecordTrackEvent(track, name, marker, ticks, kind)` adds events of a timeline that is not a CPU thread,
such as GPU timestamps converted with `CaptureTicks` and `CaptureTicksPerSecond`. Each track is saved
like a thread with the id `1 << 63 | track`.

## File format

`HXPROF01` magic, ticks per second, marker table (id, name, file, line), counter table,
threads (id, name, events, counter samples), frame timestamps. All values little endian.

## Build

```
git clone --recursive https://github.com/kisstp2006/Helix.Profiler
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## License

GPL-3.0, see `LICENSE`.
