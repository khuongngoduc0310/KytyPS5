#ifndef KYTY_COMMON_PERFORMANCE_TELEMETRY_H_
#define KYTY_COMMON_PERFORMANCE_TELEMETRY_H_

#include "common/common.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Performance {

enum class CaptureState : uint8_t { Disabled, Warmup, Collecting, Complete, Flushed };
enum class CompletionReason : uint8_t { Complete, ShutdownPartial, QuickExitPartial };
enum class PresentationClass : uint8_t { Guest, Repeat, Blank, Paused, OverlayOnly, Failed };
enum class SchedulerDomain : uint8_t { Guest, Presentation };
enum class WaitReason : uint8_t {
	Readback,
	GarbageCollection,
	FramePoolReuse,
	Finish,
	Shutdown,
	PriorityCompletion,
	StreamBufferReuse,
	Generic,
};

enum class Counter : uint16_t {
	GuestFrames,
	RepeatPresents,
	BlankPresents,
	PausedPresents,
	OverlayOnlyPresents,
	FailedPresents,
	LateVblankOpportunities,
	GuestGpuQueuedSubmissions,
	GuestGpuCompletedSubmissions,
	GuestGpuBlockedSlices,
	GuestGpuResumedSlices,
	SynchronousCommandLaneCalls,
	NativeDraws,
	SkippedDraws,
	NativeDispatches,
	SkippedDispatches,
	GuestQueueSubmissions,
	PresentationQueueSubmissions,
	DynamicRenderingBegins,
	DynamicRenderingEnds,
	ShaderBarriers,
	BufferBarriers,
	ImageBarriers,
	GlobalBarriers,
	PresentationBarriers,
	ShaderTranslations,
	SpirvCompilations,
	ProgramPermutationHits,
	ProgramPermutationMisses,
	ProgramPermutationCreations,
	GraphicsPipelineHits,
	GraphicsPipelineMisses,
	GraphicsPipelineCreations,
	ComputePipelineHits,
	ComputePipelineMisses,
	ComputePipelineCreations,
	DescriptorCommits,
	PushDescriptorUses,
	DescriptorSetAllocations,
	DescriptorBatchReuses,
	DescriptorPoolCreations,
	DescriptorPoolResets,
	DescriptorPoolRollovers,
	UploadRanges,
	UploadBytes,
	DownloadRanges,
	DownloadBytes,
	TemporaryUploadFallbacks,
	ImageCopies,
	ImageCopyBytes,
	StreamReservations,
	StreamWraps,
	StreamRejectedReservations,
	StreamReuseWaits,
	BufferGcRuns,
	BufferGcScanned,
	BufferGcRetired,
	BufferGcDirtyBytes,
	BufferGcAggressiveRuns,
	TextureGcRuns,
	TextureGcScanned,
	TextureGcRetired,
	TextureGcDirtyBytes,
	TextureGcAggressiveRuns,
	WaitReadbackAttempts,
	WaitReadbackBlocked,
	WaitGarbageCollectionAttempts,
	WaitGarbageCollectionBlocked,
	WaitFramePoolReuseAttempts,
	WaitFramePoolReuseBlocked,
	WaitFinishAttempts,
	WaitFinishBlocked,
	WaitShutdownAttempts,
	WaitShutdownBlocked,
	WaitPriorityCompletionAttempts,
	WaitPriorityCompletionBlocked,
	WaitStreamBufferReuseAttempts,
	WaitStreamBufferReuseBlocked,
	WaitGenericAttempts,
	WaitGenericBlocked,
	GpuTimingUnsupported,
	GpuQueryExhaustion,
	GpuUnavailableResults,
	GpuTimestampSubmissions,
	GpuTimestampResults,
	Count,
};

enum class Gauge : uint8_t {
	GuestGpuQueueDepth,
	GuestGpuQueueDepthHighWater,
	StreamOccupancyBytes,
	StreamOccupancyHighWaterBytes,
	PendingGpuSamples,
	PendingGpuSamplesHighWater,
	Count,
};

enum class Duration : uint8_t {
	GuestFrameInterval,
	PresentationCpu,
	GuestGpuPm4Processing,
	WaitReadback,
	WaitGarbageCollection,
	WaitFramePoolReuse,
	WaitFinish,
	WaitShutdown,
	WaitPriorityCompletion,
	WaitStreamBufferReuse,
	WaitGeneric,
	ShaderTranslation,
	SpirvCompilation,
	GraphicsPipelineCreation,
	ComputePipelineCreation,
	BufferGarbageCollection,
	TextureGarbageCollection,
	GuestGpuSubmission,
	PresentationGpuSubmission,
	Count,
};

struct CaptureOptions {
	bool tracy_enabled = false;
	std::filesystem::path report_path;
	uint64_t warmup_frames  = 0;
	uint64_t capture_frames = 0;
};

struct Metadata {
	std::string title_id   = "UNKNOWN";
	std::string gpu_device = "unknown";
	std::string gpu_driver = "unknown";
};

struct Aggregate {
	uint64_t count   = 0;
	uint64_t total   = 0;
	uint64_t minimum = 0;
	uint64_t maximum = 0;
	uint64_t p50     = 0;
	uint64_t p95     = 0;
	uint64_t p99     = 0;
};

struct TestSnapshot {
	CaptureState state             = CaptureState::Disabled;
	uint64_t captured_guest_frames = 0;
	uint64_t CounterValue(Counter metric) const;
	uint64_t GaugeValue(Gauge metric) const;
	Aggregate DurationValue(Duration metric) const;

  private:
	friend TestSnapshot SnapshotForTesting();
	static constexpr size_t CounterCount  = static_cast<size_t>(Counter::Count);
	static constexpr size_t GaugeCount    = static_cast<size_t>(Gauge::Count);
	static constexpr size_t DurationCount = static_cast<size_t>(Duration::Count);
	uint64_t counters[CounterCount]{};
	uint64_t gauges[GaugeCount]{};
	Aggregate durations[DurationCount]{};
};

void Initialize();
void Shutdown();
void EmergencyShutdown();

struct Lifecycle {
	static constexpr const char* name        = "PerformanceTelemetry";
	static constexpr auto initialize         = Performance::Initialize;
	static constexpr auto shutdown           = Performance::Shutdown;
	static constexpr auto emergency_shutdown = Performance::EmergencyShutdown;
};

[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] bool Collecting() noexcept;
[[nodiscard]] CaptureState State() noexcept;
[[nodiscard]] bool TracyEnabled() noexcept;

void Add(Counter metric, uint64_t value = 1) noexcept;
void SetGauge(Gauge metric, uint64_t value) noexcept;
void MaxGauge(Gauge metric, uint64_t value) noexcept;
void Observe(Duration metric, uint64_t nanoseconds) noexcept;
void RecordWait(WaitReason reason, uint64_t blocked_nanoseconds) noexcept;
void ObserveWait(WaitReason reason, uint64_t nanoseconds) noexcept;
void Presentation(PresentationClass classification, uint64_t cpu_nanoseconds) noexcept;
void SetMetadata(const Metadata& metadata);
void SetGpuMetadata(std::string_view device, uint32_t driver_version);

class ScopedDuration final {
  public:
	explicit ScopedDuration(Duration metric) noexcept;
	~ScopedDuration();
	ScopedDuration(const ScopedDuration&)            = delete;
	ScopedDuration& operator=(const ScopedDuration&) = delete;
	ScopedDuration(ScopedDuration&&)                 = delete;
	ScopedDuration& operator=(ScopedDuration&&)      = delete;

  private:
	Duration m_metric;
	bool m_enabled;
	std::chrono::steady_clock::time_point m_start;
};

void StartForTesting(const CaptureOptions& options, const Metadata& metadata = {});
void ResetForTesting();
[[nodiscard]] TestSnapshot SnapshotForTesting();
[[nodiscard]] std::string BuildJsonForTesting(CompletionReason reason);

[[nodiscard]] const char* CounterName(Counter metric) noexcept;
[[nodiscard]] const char* GaugeName(Gauge metric) noexcept;
[[nodiscard]] const char* DurationName(Duration metric) noexcept;

} // namespace Performance

#endif /* KYTY_COMMON_PERFORMANCE_TELEMETRY_H_ */
