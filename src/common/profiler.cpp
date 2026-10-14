#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <tracy/Tracy.hpp>

namespace Profiler {

void SetThreadName(const char* name) {
	if (tracy::ProfilerAvailable() && name != nullptr) {
		tracy::SetThreadName(name);
	}
}

void GameplayFrame(bool collected, uint64_t interval_nanoseconds, uint64_t captured_frames,
                   uint64_t native_draws, uint64_t native_dispatches, uint64_t queue_depth,
                   uint64_t gpu_timing_results) {
	if (!tracy::ProfilerAvailable()) {
		return;
	}
	FrameMarkNamed("GuestFrame");
	if (collected) {
		TracyPlot("Kyty/Guest frame interval ms",
		          static_cast<double>(interval_nanoseconds) / 1000000.0);
		TracyPlot("Kyty/Captured guest frames", static_cast<int64_t>(captured_frames));
		TracyPlot("Kyty/Native draws total", static_cast<int64_t>(native_draws));
		TracyPlot("Kyty/Native dispatches total", static_cast<int64_t>(native_dispatches));
		TracyPlot("Kyty/Guest GPU queue depth", static_cast<int64_t>(queue_depth));
		TracyPlot("Kyty/GPU timing results total", static_cast<int64_t>(gpu_timing_results));
	}
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
