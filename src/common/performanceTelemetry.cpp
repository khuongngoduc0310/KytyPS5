#include "common/performanceTelemetry.h"

#include "common/emulatorConfig.h"
#include "common/profiler.h"
#include "common/systemInfo.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Performance {
namespace {

using Json  = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kCounterCount     = static_cast<size_t>(Counter::Count);
constexpr size_t kGaugeCount       = static_cast<size_t>(Gauge::Count);
constexpr size_t kDurationCount    = static_cast<size_t>(Duration::Count);
constexpr size_t kHistogramBuckets = 65;

struct MetricDescription {
	const char* name;
	const char* unit;
};

constexpr std::array<MetricDescription, kCounterCount> kCounterDescriptions{{
    {"frames.guest", "count"},
    {"presents.repeat", "count"},
    {"presents.blank", "count"},
    {"presents.paused", "count"},
    {"presents.overlay_only", "count"},
    {"presents.failed", "count"},
    {"vblank.late_opportunities", "count"},
    {"guest_gpu.submissions_queued", "count"},
    {"guest_gpu.submissions_completed", "count"},
    {"guest_gpu.slices_blocked", "count"},
    {"guest_gpu.slices_resumed", "count"},
    {"guest_gpu.synchronous_command_lane_calls", "count"},
    {"render.native_draws", "count"},
    {"render.skipped_draws", "count"},
    {"render.native_dispatches", "count"},
    {"render.skipped_dispatches", "count"},
    {"scheduler.guest_submissions", "count"},
    {"scheduler.presentation_submissions", "count"},
    {"render.dynamic_rendering_begins", "count"},
    {"render.dynamic_rendering_ends", "count"},
    {"barriers.shader", "count"},
    {"barriers.buffer", "count"},
    {"barriers.image", "count"},
    {"barriers.global", "count"},
    {"barriers.presentation", "count"},
    {"compile.shader_translations", "count"},
    {"compile.spirv_modules", "count"},
    {"program.permutation_hits", "count"},
    {"program.permutation_misses", "count"},
    {"program.permutation_creations", "count"},
    {"pipeline.graphics_hits", "count"},
    {"pipeline.graphics_misses", "count"},
    {"pipeline.graphics_creations", "count"},
    {"pipeline.compute_hits", "count"},
    {"pipeline.compute_misses", "count"},
    {"pipeline.compute_creations", "count"},
    {"descriptors.commits", "count"},
    {"descriptors.push_uses", "count"},
    {"descriptors.set_allocations", "count"},
    {"descriptors.batch_reuses", "count"},
    {"descriptors.pool_creations", "count"},
    {"descriptors.pool_resets", "count"},
    {"descriptors.pool_rollovers", "count"},
    {"transfer.upload_ranges", "count"},
    {"transfer.upload_bytes", "bytes"},
    {"transfer.download_ranges", "count"},
    {"transfer.download_bytes", "bytes"},
    {"transfer.temporary_upload_fallbacks", "count"},
    {"transfer.image_copies", "count"},
    {"transfer.image_copy_bytes", "bytes"},
    {"stream.reservations", "count"},
    {"stream.wraps", "count"},
    {"stream.rejected_reservations", "count"},
    {"stream.reuse_waits", "count"},
    {"gc.buffer_runs", "count"},
    {"gc.buffer_scanned", "count"},
    {"gc.buffer_retired", "count"},
    {"gc.buffer_dirty_bytes", "bytes"},
    {"gc.buffer_aggressive_runs", "count"},
    {"gc.texture_runs", "count"},
    {"gc.texture_scanned", "count"},
    {"gc.texture_retired", "count"},
    {"gc.texture_dirty_bytes", "bytes"},
    {"gc.texture_aggressive_runs", "count"},
    {"wait.readback_attempts", "count"},
    {"wait.readback_blocked", "count"},
    {"wait.garbage_collection_attempts", "count"},
    {"wait.garbage_collection_blocked", "count"},
    {"wait.frame_pool_reuse_attempts", "count"},
    {"wait.frame_pool_reuse_blocked", "count"},
    {"wait.finish_attempts", "count"},
    {"wait.finish_blocked", "count"},
    {"wait.shutdown_attempts", "count"},
    {"wait.shutdown_blocked", "count"},
    {"wait.priority_completion_attempts", "count"},
    {"wait.priority_completion_blocked", "count"},
    {"wait.stream_buffer_reuse_attempts", "count"},
    {"wait.stream_buffer_reuse_blocked", "count"},
    {"wait.generic_attempts", "count"},
    {"wait.generic_blocked", "count"},
    {"gpu_timing.unsupported", "count"},
    {"gpu_timing.query_exhaustion", "count"},
    {"gpu_timing.unavailable_results", "count"},
    {"gpu_timing.submissions", "count"},
    {"gpu_timing.results", "count"},
}};

constexpr std::array<MetricDescription, kGaugeCount> kGaugeDescriptions{{
    {"guest_gpu.queue_depth", "count"},
    {"guest_gpu.queue_depth_high_water", "count"},
    {"stream.occupancy", "bytes"},
    {"stream.occupancy_high_water", "bytes"},
    {"gpu_timing.pending_samples", "count"},
    {"gpu_timing.pending_samples_high_water", "count"},
}};

constexpr std::array<MetricDescription, kDurationCount> kDurationDescriptions{{
    {"frames.guest_interval", "nanoseconds"},
    {"presentation.cpu", "nanoseconds"},
    {"guest_gpu.pm4_processing", "nanoseconds"},
    {"wait.readback", "nanoseconds"},
    {"wait.garbage_collection", "nanoseconds"},
    {"wait.frame_pool_reuse", "nanoseconds"},
    {"wait.finish", "nanoseconds"},
    {"wait.shutdown", "nanoseconds"},
    {"wait.priority_completion", "nanoseconds"},
    {"wait.stream_buffer_reuse", "nanoseconds"},
    {"wait.generic", "nanoseconds"},
    {"compile.shader_translation", "nanoseconds"},
    {"compile.spirv", "nanoseconds"},
    {"pipeline.graphics_creation", "nanoseconds"},
    {"pipeline.compute_creation", "nanoseconds"},
    {"gc.buffer", "nanoseconds"},
    {"gc.texture", "nanoseconds"},
    {"gpu.guest_submission", "nanoseconds"},
    {"gpu.presentation_submission", "nanoseconds"},
}};

static_assert(kCounterDescriptions.size() == kCounterCount);
static_assert(kGaugeDescriptions.size() == kGaugeCount);
static_assert(kDurationDescriptions.size() == kDurationCount);

struct AtomicDuration {
	std::atomic<uint64_t> count{0};
	std::atomic<uint64_t> total{0};
	std::atomic<uint64_t> minimum{std::numeric_limits<uint64_t>::max()};
	std::atomic<uint64_t> maximum{0};
	std::array<std::atomic<uint64_t>, kHistogramBuckets> buckets{};
};

struct Collector {
	std::atomic<bool> enabled{false};
	std::atomic<CaptureState> state{CaptureState::Disabled};
	std::atomic<uint32_t> active_producers{0};
	std::atomic<uint64_t> warmup_frames{0};
	std::atomic<uint64_t> captured_guest_frames{0};
	std::atomic<int64_t> last_guest_frame_ns{0};
	std::array<std::atomic<uint64_t>, kCounterCount> counters{};
	std::array<std::atomic<uint64_t>, kGaugeCount> gauges{};
	std::array<AtomicDuration, kDurationCount> durations{};
	std::atomic<bool> publication_claimed{false};
	std::atomic<bool> warning_emitted{false};
};

Collector g_collector;
CaptureOptions g_options;
std::string g_host_cpu;
std::atomic<std::shared_ptr<const Metadata>> g_metadata;

constexpr size_t ToIndex(Counter metric) { return static_cast<size_t>(metric); }
constexpr size_t ToIndex(Gauge metric) { return static_cast<size_t>(metric); }
constexpr size_t ToIndex(Duration metric) { return static_cast<size_t>(metric); }

uint64_t NowNanoseconds() noexcept {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
	        .count());
}

size_t HistogramBucket(uint64_t value) noexcept {
	return value == 0 ? 0 : std::min<size_t>(std::bit_width(value), kHistogramBuckets - 1);
}

uint64_t BucketUpperBound(size_t bucket) noexcept {
	if (bucket == 0) {
		return 0;
	}
	if (bucket >= 64) {
		return std::numeric_limits<uint64_t>::max();
	}
	return (uint64_t{1} << bucket) - 1;
}

void AtomicMaximum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
	auto current = target.load(std::memory_order_relaxed);
	while (current < value &&
	       !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
	                                     std::memory_order_relaxed)) {
	}
}

void AtomicMinimum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
	auto current = target.load(std::memory_order_relaxed);
	while (current > value &&
	       !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
	                                     std::memory_order_relaxed)) {
	}
}

bool EnterProducer() noexcept {
	if (!g_collector.enabled.load(std::memory_order_relaxed) ||
	    g_collector.state.load(std::memory_order_relaxed) != CaptureState::Collecting) {
		return false;
	}
	g_collector.active_producers.fetch_add(1, std::memory_order_acquire);
	if (!g_collector.enabled.load(std::memory_order_relaxed) ||
	    g_collector.state.load(std::memory_order_acquire) != CaptureState::Collecting) {
		g_collector.active_producers.fetch_sub(1, std::memory_order_release);
		return false;
	}
	return true;
}

void LeaveProducer() noexcept {
	g_collector.active_producers.fetch_sub(1, std::memory_order_release);
}

void AddEntered(Counter metric, uint64_t value) noexcept {
	g_collector.counters[ToIndex(metric)].fetch_add(value, std::memory_order_relaxed);
}

void ObserveEntered(Duration metric, uint64_t nanoseconds) noexcept {
	auto& duration = g_collector.durations[ToIndex(metric)];
	duration.count.fetch_add(1, std::memory_order_relaxed);
	duration.total.fetch_add(nanoseconds, std::memory_order_relaxed);
	AtomicMinimum(duration.minimum, nanoseconds);
	AtomicMaximum(duration.maximum, nanoseconds);
	duration.buckets[HistogramBucket(nanoseconds)].fetch_add(1, std::memory_order_relaxed);
}

Aggregate SnapshotDuration(const AtomicDuration& duration) {
	Aggregate result;
	result.count = duration.count.load(std::memory_order_relaxed);
	result.total = duration.total.load(std::memory_order_relaxed);
	if (result.count == 0) {
		return result;
	}
	result.minimum        = duration.minimum.load(std::memory_order_relaxed);
	result.maximum        = duration.maximum.load(std::memory_order_relaxed);
	const auto percentile = [&](uint64_t numerator, uint64_t denominator) {
		const uint64_t target =
		    std::max<uint64_t>(1, (result.count * numerator + denominator - 1) / denominator);
		uint64_t accumulated = 0;
		for (size_t bucket = 0; bucket < duration.buckets.size(); bucket++) {
			accumulated += duration.buckets[bucket].load(std::memory_order_relaxed);
			if (accumulated >= target) {
				return BucketUpperBound(bucket);
			}
		}
		return result.maximum;
	};
	result.p50 = percentile(50, 100);
	result.p95 = percentile(95, 100);
	result.p99 = percentile(99, 100);
	return result;
}

const char* CompletionReasonName(CompletionReason reason) {
	switch (reason) {
	case CompletionReason::Complete:
		return "complete";
	case CompletionReason::ShutdownPartial:
		return "shutdown_partial";
	case CompletionReason::QuickExitPartial:
		return "quick_exit_partial";
	}
	return "unknown";
}

const char* StateName(CaptureState state) {
	switch (state) {
	case CaptureState::Disabled:
		return "disabled";
	case CaptureState::Warmup:
		return "warmup";
	case CaptureState::Collecting:
		return "collecting";
	case CaptureState::Complete:
		return "complete";
	case CaptureState::Flushed:
		return "flushed";
	}
	return "unknown";
}

std::string HostOsName() {
#if defined(__APPLE__)
	return "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return "Linux";
#else
	return "Unknown";
#endif
}

Json BuildJson(CompletionReason reason) {
	Json root;
	root["schema_version"]    = 1;
	root["completion_reason"] = CompletionReasonName(reason);
	root["capture_state"]     = StateName(g_collector.state.load(std::memory_order_relaxed));
	root["capture"]           = {
	    {"warmup_frames", g_options.warmup_frames},
	    {"capture_frames", g_options.capture_frames},
	    {"captured_guest_frames",
		 g_collector.captured_guest_frames.load(std::memory_order_relaxed)},
	    {"tracy_enabled", g_options.tracy_enabled},
	    {"bounded", g_options.capture_frames != 0},
	};

	const auto metadata = g_metadata.load(std::memory_order_acquire);
	root["metadata"]    = {
	    {"build_label", KYTY_BUILD_LABEL},
	    {"build_revision", KYTY_GIT_REVISION},
	    {"operating_system", HostOsName()},
	    {"cpu", g_host_cpu},
	    {"title_id", metadata ? metadata->title_id : "UNKNOWN"},
	    {"gpu_device", metadata ? metadata->gpu_device : "unknown"},
	    {"gpu_driver", metadata ? metadata->gpu_driver : "unknown"},
	};

	auto counter_value = [](Counter metric) {
		return g_collector.counters[ToIndex(metric)].load(std::memory_order_relaxed);
	};
	root["frame_classifications"] = {
	    {"guest", counter_value(Counter::GuestFrames)},
	    {"repeat", counter_value(Counter::RepeatPresents)},
	    {"blank", counter_value(Counter::BlankPresents)},
	    {"paused", counter_value(Counter::PausedPresents)},
	    {"overlay_only", counter_value(Counter::OverlayOnlyPresents)},
	    {"failed", counter_value(Counter::FailedPresents)},
	};

	Json metrics = Json::object();
	for (size_t index = 0; index < kCounterCount; index++) {
		const auto& description   = kCounterDescriptions[index];
		metrics[description.name] = {
		    {"kind", "counter"},
		    {"unit", description.unit},
		    {"value", g_collector.counters[index].load(std::memory_order_relaxed)},
		};
	}
	for (size_t index = 0; index < kGaugeCount; index++) {
		const auto& description   = kGaugeDescriptions[index];
		metrics[description.name] = {
		    {"kind", "gauge"},
		    {"unit", description.unit},
		    {"value", g_collector.gauges[index].load(std::memory_order_relaxed)},
		};
	}
	for (size_t index = 0; index < kDurationCount; index++) {
		const auto& description   = kDurationDescriptions[index];
		const auto aggregate      = SnapshotDuration(g_collector.durations[index]);
		metrics[description.name] = {
		    {"kind", "histogram"},
		    {"unit", description.unit},
		    {"count", aggregate.count},
		    {"total", aggregate.total},
		    {"minimum", aggregate.minimum},
		    {"maximum", aggregate.maximum},
		    {"mean", aggregate.count == 0 ? 0.0
			                              : static_cast<double>(aggregate.total) /
			                                    static_cast<double>(aggregate.count)},
		    {"p50", aggregate.p50},
		    {"p95", aggregate.p95},
		    {"p99", aggregate.p99},
		};
	}
	root["metrics"]     = std::move(metrics);
	root["sample_loss"] = {
	    {"gpu_timing_unsupported", counter_value(Counter::GpuTimingUnsupported)},
	    {"gpu_query_exhaustion", counter_value(Counter::GpuQueryExhaustion)},
	    {"gpu_unavailable_results", counter_value(Counter::GpuUnavailableResults)},
	    {"pending_gpu_samples",
		 g_collector.gauges[ToIndex(Gauge::PendingGpuSamples)].load(std::memory_order_relaxed)},
	};
	return root;
}

bool ReplaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return MoveFileExW(temporary.c_str(), destination.c_str(),
	                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	std::error_code error;
	std::filesystem::rename(temporary, destination, error);
	return !error;
#endif
}

void WarnOutputOnce(const std::filesystem::path& path) noexcept {
	if (!g_collector.warning_emitted.exchange(true, std::memory_order_relaxed)) {
		const auto text = path.u8string();
		std::fprintf(stderr, "Performance telemetry: failed to publish report: %s\n",
		             reinterpret_cast<const char*>(text.c_str()));
	}
}

void PublishReport(CompletionReason reason) noexcept {
	if (g_options.report_path.empty() ||
	    g_collector.publication_claimed.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	try {
		auto temporary = g_options.report_path;
		temporary += ".tmp";
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) {
			WarnOutputOnce(g_options.report_path);
			return;
		}
		output << BuildJson(reason).dump(2) << '\n';
		output.flush();
		output.close();
		if (!output || !ReplaceFile(temporary, g_options.report_path)) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			WarnOutputOnce(g_options.report_path);
		}
	} catch (...) {
		WarnOutputOnce(g_options.report_path);
	}
}

void ResetCollector() {
	g_collector.enabled.store(false, std::memory_order_relaxed);
	g_collector.state.store(CaptureState::Disabled, std::memory_order_relaxed);
	g_collector.active_producers.store(0, std::memory_order_relaxed);
	g_collector.warmup_frames.store(0, std::memory_order_relaxed);
	g_collector.captured_guest_frames.store(0, std::memory_order_relaxed);
	g_collector.last_guest_frame_ns.store(0, std::memory_order_relaxed);
	for (auto& counter : g_collector.counters) {
		counter.store(0, std::memory_order_relaxed);
	}
	for (auto& gauge : g_collector.gauges) {
		gauge.store(0, std::memory_order_relaxed);
	}
	for (auto& duration : g_collector.durations) {
		duration.count.store(0, std::memory_order_relaxed);
		duration.total.store(0, std::memory_order_relaxed);
		duration.minimum.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
		duration.maximum.store(0, std::memory_order_relaxed);
		for (auto& bucket : duration.buckets) {
			bucket.store(0, std::memory_order_relaxed);
		}
	}
	g_collector.publication_claimed.store(false, std::memory_order_relaxed);
	g_collector.warning_emitted.store(false, std::memory_order_relaxed);
	g_host_cpu.clear();
	g_metadata.store(nullptr, std::memory_order_relaxed);
}

void Start(const CaptureOptions& options, const Metadata& metadata) {
	ResetCollector();
	g_options = options;
	if (!options.tracy_enabled && options.report_path.empty()) {
		return;
	}
	g_host_cpu = Common::GetSystemInfo().ProcessorName;
	g_metadata.store(std::make_shared<const Metadata>(metadata), std::memory_order_release);
	g_collector.state.store(options.warmup_frames == 0 ? CaptureState::Collecting
	                                                   : CaptureState::Warmup,
	                        std::memory_order_release);
	g_collector.enabled.store(true, std::memory_order_release);
}

void WaitForProducers() noexcept {
	while (g_collector.active_producers.load(std::memory_order_acquire) != 0) {
		std::this_thread::yield();
	}
}

Counter PresentationCounter(PresentationClass classification) {
	switch (classification) {
	case PresentationClass::Guest:
		return Counter::GuestFrames;
	case PresentationClass::Repeat:
		return Counter::RepeatPresents;
	case PresentationClass::Blank:
		return Counter::BlankPresents;
	case PresentationClass::Paused:
		return Counter::PausedPresents;
	case PresentationClass::OverlayOnly:
		return Counter::OverlayOnlyPresents;
	case PresentationClass::Failed:
		return Counter::FailedPresents;
	}
	return Counter::FailedPresents;
}

std::pair<Counter, Counter> WaitCounters(WaitReason reason) {
	switch (reason) {
	case WaitReason::Readback:
		return {Counter::WaitReadbackAttempts, Counter::WaitReadbackBlocked};
	case WaitReason::GarbageCollection:
		return {Counter::WaitGarbageCollectionAttempts, Counter::WaitGarbageCollectionBlocked};
	case WaitReason::FramePoolReuse:
		return {Counter::WaitFramePoolReuseAttempts, Counter::WaitFramePoolReuseBlocked};
	case WaitReason::Finish:
		return {Counter::WaitFinishAttempts, Counter::WaitFinishBlocked};
	case WaitReason::Shutdown:
		return {Counter::WaitShutdownAttempts, Counter::WaitShutdownBlocked};
	case WaitReason::PriorityCompletion:
		return {Counter::WaitPriorityCompletionAttempts, Counter::WaitPriorityCompletionBlocked};
	case WaitReason::StreamBufferReuse:
		return {Counter::WaitStreamBufferReuseAttempts, Counter::WaitStreamBufferReuseBlocked};
	case WaitReason::Generic:
		return {Counter::WaitGenericAttempts, Counter::WaitGenericBlocked};
	}
	return {Counter::WaitGenericAttempts, Counter::WaitGenericBlocked};
}

Duration WaitDuration(WaitReason reason) {
	switch (reason) {
	case WaitReason::Readback:
		return Duration::WaitReadback;
	case WaitReason::GarbageCollection:
		return Duration::WaitGarbageCollection;
	case WaitReason::FramePoolReuse:
		return Duration::WaitFramePoolReuse;
	case WaitReason::Finish:
		return Duration::WaitFinish;
	case WaitReason::Shutdown:
		return Duration::WaitShutdown;
	case WaitReason::PriorityCompletion:
		return Duration::WaitPriorityCompletion;
	case WaitReason::StreamBufferReuse:
		return Duration::WaitStreamBufferReuse;
	case WaitReason::Generic:
		return Duration::WaitGeneric;
	}
	return Duration::WaitGeneric;
}

} // namespace

const char* CounterName(Counter metric) noexcept {
	const auto index = ToIndex(metric);
	return index < kCounterDescriptions.size() ? kCounterDescriptions[index].name : "invalid";
}

const char* GaugeName(Gauge metric) noexcept {
	const auto index = ToIndex(metric);
	return index < kGaugeDescriptions.size() ? kGaugeDescriptions[index].name : "invalid";
}

const char* DurationName(Duration metric) noexcept {
	const auto index = ToIndex(metric);
	return index < kDurationDescriptions.size() ? kDurationDescriptions[index].name : "invalid";
}

void Initialize() {
	CaptureOptions options;
	options.tracy_enabled  = Config::ProfilerEnabled();
	options.report_path    = Config::GetPerformanceReportPath();
	options.warmup_frames  = Config::GetPerformanceWarmupFrames();
	options.capture_frames = Config::GetPerformanceCaptureFrames();
	Start(options, {});
}

void Shutdown() {
	if (g_collector.state.load(std::memory_order_acquire) == CaptureState::Disabled ||
	    g_collector.state.load(std::memory_order_acquire) == CaptureState::Flushed) {
		return;
	}
	g_collector.enabled.store(false, std::memory_order_release);
	const auto previous =
	    g_collector.state.exchange(CaptureState::Flushed, std::memory_order_acq_rel);
	WaitForProducers();
	const bool complete = previous == CaptureState::Complete ||
	                      (previous == CaptureState::Collecting && g_options.capture_frames == 0);
	PublishReport(complete ? CompletionReason::Complete : CompletionReason::ShutdownPartial);
}

void EmergencyShutdown() {
	const auto state = g_collector.state.load(std::memory_order_acquire);
	if (state == CaptureState::Disabled || state == CaptureState::Flushed) {
		return;
	}
	g_collector.enabled.store(false, std::memory_order_release);
	g_collector.state.store(CaptureState::Flushed, std::memory_order_release);
	PublishReport(state == CaptureState::Complete ? CompletionReason::Complete
	                                              : CompletionReason::QuickExitPartial);
}

bool Enabled() noexcept { return g_collector.enabled.load(std::memory_order_relaxed); }

bool Collecting() noexcept {
	return Enabled() &&
	       g_collector.state.load(std::memory_order_relaxed) == CaptureState::Collecting;
}

CaptureState State() noexcept { return g_collector.state.load(std::memory_order_acquire); }

bool TracyEnabled() noexcept { return g_options.tracy_enabled; }

void Add(Counter metric, uint64_t value) noexcept {
	if (!Enabled() || !EnterProducer()) {
		return;
	}
	AddEntered(metric, value);
	LeaveProducer();
}

void SetGauge(Gauge metric, uint64_t value) noexcept {
	if (!Enabled() || !EnterProducer()) {
		return;
	}
	g_collector.gauges[ToIndex(metric)].store(value, std::memory_order_relaxed);
	LeaveProducer();
}

void MaxGauge(Gauge metric, uint64_t value) noexcept {
	if (!Enabled() || !EnterProducer()) {
		return;
	}
	AtomicMaximum(g_collector.gauges[ToIndex(metric)], value);
	LeaveProducer();
}

void Observe(Duration metric, uint64_t nanoseconds) noexcept {
	if (!Enabled() || !EnterProducer()) {
		return;
	}
	ObserveEntered(metric, nanoseconds);
	LeaveProducer();
}

void RecordWait(WaitReason reason, uint64_t blocked_nanoseconds) noexcept {
	if (!Enabled() || !EnterProducer()) {
		return;
	}
	const auto [attempts, blocked] = WaitCounters(reason);
	AddEntered(attempts, 1);
	if (blocked_nanoseconds != 0) {
		AddEntered(blocked, 1);
		if (reason == WaitReason::StreamBufferReuse) {
			AddEntered(Counter::StreamReuseWaits, 1);
		}
		ObserveEntered(WaitDuration(reason), blocked_nanoseconds);
	}
	LeaveProducer();
}

void ObserveWait(WaitReason reason, uint64_t nanoseconds) noexcept {
	if (nanoseconds != 0) {
		Observe(WaitDuration(reason), nanoseconds);
	}
}

void Presentation(PresentationClass classification, uint64_t cpu_nanoseconds) noexcept {
	if (!Enabled()) {
		return;
	}
	const auto now = NowNanoseconds();
	if (classification == PresentationClass::Guest) {
		if (g_collector.state.load(std::memory_order_acquire) == CaptureState::Warmup) {
			const auto warmed =
			    g_collector.warmup_frames.fetch_add(1, std::memory_order_relaxed) + 1;
			if (warmed >= g_options.warmup_frames) {
				CaptureState expected = CaptureState::Warmup;
				if (g_collector.state.compare_exchange_strong(expected, CaptureState::Collecting,
				                                              std::memory_order_acq_rel)) {
					g_collector.last_guest_frame_ns.store(static_cast<int64_t>(now),
					                                      std::memory_order_relaxed);
				}
			}
			Profiler::GameplayFrame(false, 0, 0, 0, 0, 0, 0);
			return;
		}
	}
	if (!EnterProducer()) {
		return;
	}
	AddEntered(PresentationCounter(classification), 1);
	ObserveEntered(Duration::PresentationCpu, cpu_nanoseconds);
	uint64_t frame_interval = 0;
	uint64_t captured       = g_collector.captured_guest_frames.load(std::memory_order_relaxed);
	if (classification == PresentationClass::Guest) {
		const auto previous = g_collector.last_guest_frame_ns.exchange(static_cast<int64_t>(now),
		                                                               std::memory_order_relaxed);
		if (previous != 0 && now >= static_cast<uint64_t>(previous)) {
			frame_interval = now - static_cast<uint64_t>(previous);
			ObserveEntered(Duration::GuestFrameInterval, frame_interval);
		}
		captured = g_collector.captured_guest_frames.fetch_add(1, std::memory_order_relaxed) + 1;
	}
	LeaveProducer();

	if (classification == PresentationClass::Guest) {
		Profiler::GameplayFrame(
		    true, frame_interval, captured,
		    g_collector.counters[ToIndex(Counter::NativeDraws)].load(std::memory_order_relaxed),
		    g_collector.counters[ToIndex(Counter::NativeDispatches)].load(
		        std::memory_order_relaxed),
		    g_collector.gauges[ToIndex(Gauge::GuestGpuQueueDepth)].load(std::memory_order_relaxed),
		    g_collector.counters[ToIndex(Counter::GpuTimestampResults)].load(
		        std::memory_order_relaxed));
		if (g_options.capture_frames != 0 && captured >= g_options.capture_frames) {
			CaptureState expected = CaptureState::Collecting;
			if (g_collector.state.compare_exchange_strong(expected, CaptureState::Complete,
			                                              std::memory_order_acq_rel)) {
				g_collector.enabled.store(false, std::memory_order_release);
				WaitForProducers();
				PublishReport(CompletionReason::Complete);
			}
		}
	}
}

void SetMetadata(const Metadata& metadata) {
	if (Enabled()) {
		g_metadata.store(std::make_shared<const Metadata>(metadata), std::memory_order_release);
	}
}

void SetGpuMetadata(std::string_view device, uint32_t driver_version) {
	if (!Enabled()) {
		return;
	}
	auto previous = g_metadata.load(std::memory_order_acquire);
	auto metadata = previous ? std::make_shared<Metadata>(*previous) : std::make_shared<Metadata>();
	metadata->gpu_device.assign(device);
	metadata->gpu_driver = std::to_string(driver_version);
	g_metadata.store(std::move(metadata), std::memory_order_release);
}

ScopedDuration::ScopedDuration(Duration metric) noexcept
    : m_metric(metric), m_enabled(Collecting()),
      m_start(m_enabled ? Clock::now() : Clock::time_point{}) {}

ScopedDuration::~ScopedDuration() {
	if (m_enabled) {
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start);
		Observe(m_metric, static_cast<uint64_t>(std::max<int64_t>(0, elapsed.count())));
	}
}

void StartForTesting(const CaptureOptions& options, const Metadata& metadata) {
	Start(options, metadata);
}

void ResetForTesting() {
	ResetCollector();
	g_options = {};
	g_host_cpu.clear();
	g_metadata.store(nullptr, std::memory_order_release);
}

TestSnapshot SnapshotForTesting() {
	TestSnapshot snapshot;
	snapshot.state = State();
	snapshot.captured_guest_frames =
	    g_collector.captured_guest_frames.load(std::memory_order_relaxed);
	for (size_t index = 0; index < kCounterCount; index++) {
		snapshot.counters[index] = g_collector.counters[index].load(std::memory_order_relaxed);
	}
	for (size_t index = 0; index < kGaugeCount; index++) {
		snapshot.gauges[index] = g_collector.gauges[index].load(std::memory_order_relaxed);
	}
	for (size_t index = 0; index < kDurationCount; index++) {
		snapshot.durations[index] = SnapshotDuration(g_collector.durations[index]);
	}
	return snapshot;
}

std::string BuildJsonForTesting(CompletionReason reason) { return BuildJson(reason).dump(); }

uint64_t TestSnapshot::CounterValue(Counter metric) const {
	return counters[static_cast<size_t>(metric)];
}

uint64_t TestSnapshot::GaugeValue(Gauge metric) const {
	return gauges[static_cast<size_t>(metric)];
}

Aggregate TestSnapshot::DurationValue(Duration metric) const {
	return durations[static_cast<size_t>(metric)];
}

} // namespace Performance
