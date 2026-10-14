#include "common/performanceTelemetry.h"
#include "graphics/host_gpu/renderer/gpuTimestampCollector.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PerformanceTelemetryTests: failed: %s\n", message);
		std::abort();
	}
}

Performance::CaptureOptions EnabledOptions() {
	Performance::CaptureOptions options;
	options.tracy_enabled = true;
	return options;
}

void TestDisabledPath() {
	Performance::ResetForTesting();
	Performance::Add(Performance::Counter::NativeDraws, 7);
	Performance::SetGauge(Performance::Gauge::GuestGpuQueueDepth, 3);
	Performance::Observe(Performance::Duration::ShaderTranslation, 100);
	Performance::Presentation(Performance::PresentationClass::Guest, 200);
	const auto snapshot = Performance::SnapshotForTesting();
	Check(snapshot.state == Performance::CaptureState::Disabled, "disabled state changed");
	Check(snapshot.CounterValue(Performance::Counter::NativeDraws) == 0,
	      "disabled counter changed");
	Check(snapshot.DurationValue(Performance::Duration::ShaderTranslation).count == 0,
	      "disabled duration changed");
}

void TestCaptureWindow() {
	auto options           = EnabledOptions();
	options.warmup_frames  = 2;
	options.capture_frames = 3;
	Performance::StartForTesting(options);
	for (uint32_t frame = 0; frame < 6; frame++) {
		Performance::Add(Performance::Counter::NativeDraws, 10);
		Performance::Presentation(Performance::PresentationClass::Guest, 100 + frame);
	}
	const auto snapshot = Performance::SnapshotForTesting();
	Check(snapshot.state == Performance::CaptureState::Complete,
	      "bounded capture did not complete");
	Check(snapshot.captured_guest_frames == 3, "wrong bounded guest-frame count");
	Check(snapshot.CounterValue(Performance::Counter::GuestFrames) == 3,
	      "guest classification included warmup or post-capture frame");
	Check(snapshot.CounterValue(Performance::Counter::NativeDraws) == 30,
	      "counter capture window is wrong");
	Check(snapshot.DurationValue(Performance::Duration::PresentationCpu).count == 3,
	      "presentation durations included wrong frames");
}

void TestClassifications() {
	Performance::StartForTesting(EnabledOptions());
	Performance::Presentation(Performance::PresentationClass::Repeat, 1);
	Performance::Presentation(Performance::PresentationClass::Blank, 1);
	Performance::Presentation(Performance::PresentationClass::Paused, 1);
	Performance::Presentation(Performance::PresentationClass::OverlayOnly, 1);
	Performance::Presentation(Performance::PresentationClass::Failed, 1);
	Performance::Presentation(Performance::PresentationClass::Guest, 1);
	const auto snapshot = Performance::SnapshotForTesting();
	Check(snapshot.captured_guest_frames == 1, "non-guest presentation advanced capture");
	Check(snapshot.CounterValue(Performance::Counter::RepeatPresents) == 1,
	      "repeat classification missing");
	Check(snapshot.CounterValue(Performance::Counter::BlankPresents) == 1,
	      "blank classification missing");
	Check(snapshot.CounterValue(Performance::Counter::PausedPresents) == 1,
	      "paused classification missing");
	Check(snapshot.CounterValue(Performance::Counter::OverlayOnlyPresents) == 1,
	      "overlay classification missing");
	Check(snapshot.CounterValue(Performance::Counter::FailedPresents) == 1,
	      "failed classification missing");
}

void TestAggregates() {
	Performance::StartForTesting(EnabledOptions());
	for (const uint64_t value : {1, 2, 3, 8, 16}) {
		Performance::Observe(Performance::Duration::ShaderTranslation, value);
	}
	const auto value =
	    Performance::SnapshotForTesting().DurationValue(Performance::Duration::ShaderTranslation);
	Check(value.count == 5 && value.total == 30, "duration count or total wrong");
	Check(value.minimum == 1 && value.maximum == 16, "duration bounds wrong");
	Check(value.p50 >= 3 && value.p50 <= 7, "p50 bucket wrong");
	Check(value.p95 >= 16 && value.p99 >= 16, "tail percentile bucket wrong");
}

void TestConcurrentUpdates() {
	Performance::StartForTesting(EnabledOptions());
	constexpr uint32_t kThreads = 8;
	constexpr uint32_t kUpdates = 20000;
	std::vector<std::thread> workers;
	for (uint32_t thread = 0; thread < kThreads; thread++) {
		workers.emplace_back([] {
			for (uint32_t update = 0; update < kUpdates; update++) {
				Performance::Add(Performance::Counter::NativeDispatches);
				Performance::MaxGauge(Performance::Gauge::GuestGpuQueueDepthHighWater, update);
			}
		});
	}
	for (auto& worker : workers) {
		worker.join();
	}
	const auto snapshot = Performance::SnapshotForTesting();
	Check(snapshot.CounterValue(Performance::Counter::NativeDispatches) ==
	          static_cast<uint64_t>(kThreads) * kUpdates,
	      "concurrent counter updates were lost");
	Check(snapshot.GaugeValue(Performance::Gauge::GuestGpuQueueDepthHighWater) == kUpdates - 1,
	      "concurrent high-water update was lost");
}

void TestJsonAndPublication() {
	const auto directory = std::filesystem::temp_directory_path() / "kyty-performance-tests";
	std::filesystem::create_directories(directory);
	const auto report = directory / "capture.json";
	std::error_code ignored;
	std::filesystem::remove(report, ignored);

	auto options           = EnabledOptions();
	options.report_path    = report;
	options.capture_frames = 1;
	Performance::Metadata metadata;
	metadata.title_id   = "TEST00001";
	metadata.gpu_device = "Test GPU";
	metadata.gpu_driver = "123";
	Performance::StartForTesting(options, metadata);
	Performance::Add(Performance::Counter::NativeDraws, 4);
	Performance::Presentation(Performance::PresentationClass::Guest, 500);
	Check(std::filesystem::is_regular_file(report), "bounded report was not published");

	std::ifstream input(report);
	Json json;
	input >> json;
	Check(json.at("schema_version") == 1, "schema version missing");
	Check(json.at("completion_reason") == "complete", "completion reason wrong");
	Check(json.at("metadata").at("title_id") == "TEST00001", "title metadata wrong");
	Check(json.at("capture").at("captured_guest_frames") == 1, "capture metadata wrong");
	Check(json.at("metrics").at("render.native_draws").at("value") == 4, "metric JSON wrong");
	Check(json.at("metrics").at("presentation.cpu").at("unit") == "nanoseconds",
	      "metric unit missing");

	std::filesystem::remove(report, ignored);
	std::filesystem::remove(directory, ignored);
}

void TestIdempotentFinalization() {
	const auto directory = std::filesystem::temp_directory_path() / "kyty-performance-finalize";
	std::filesystem::create_directories(directory);
	const auto report = directory / "capture.json";
	std::error_code ignored;
	std::filesystem::remove(report, ignored);

	auto options        = EnabledOptions();
	options.report_path = report;
	Performance::StartForTesting(options);
	Performance::Add(Performance::Counter::NativeDraws);
	std::thread normal([] { Performance::Shutdown(); });
	std::thread emergency([] { Performance::EmergencyShutdown(); });
	normal.join();
	emergency.join();
	Check(std::filesystem::is_regular_file(report), "concurrent finalization wrote no report");
	std::ifstream input(report);
	Json json;
	input >> json;
	const auto reason = json.at("completion_reason").get<std::string>();
	Check(reason == "complete" || reason == "shutdown_partial" || reason == "quick_exit_partial",
	      "concurrent finalization used invalid reason");
	Check(json.at("schema_version") == 1, "concurrent report is malformed");

	std::filesystem::remove(report, ignored);
	std::filesystem::remove(directory, ignored);
}

void TestEmergencyPendingGpuReport() {
	const auto directory = std::filesystem::temp_directory_path() / "kyty-performance-emergency";
	std::filesystem::create_directories(directory);
	const auto report = directory / "capture.json";
	std::error_code ignored;
	std::filesystem::remove(report, ignored);

	auto options        = EnabledOptions();
	options.report_path = report;
	Performance::StartForTesting(options);
	Performance::SetGauge(Performance::Gauge::PendingGpuSamples, 3);
	Performance::EmergencyShutdown();
	std::ifstream input(report);
	Json json;
	input >> json;
	Check(json.at("completion_reason") == "quick_exit_partial",
	      "emergency completion reason wrong");
	Check(json.at("sample_loss").at("pending_gpu_samples") == 3,
	      "emergency report lost pending GPU sample count");

	std::filesystem::remove(report, ignored);
	std::filesystem::remove(directory, ignored);
}

void TestUnlimitedShutdownCompletes() {
	const auto directory = std::filesystem::temp_directory_path() / "kyty-performance-unlimited";
	std::filesystem::create_directories(directory);
	const auto report = directory / "capture.json";
	std::error_code ignored;
	std::filesystem::remove(report, ignored);

	auto options        = EnabledOptions();
	options.report_path = report;
	Performance::StartForTesting(options);
	Performance::Presentation(Performance::PresentationClass::Guest, 1);
	Performance::Shutdown();
	std::ifstream input(report);
	Json json;
	input >> json;
	Check(json.at("completion_reason") == "complete", "unlimited normal shutdown was partial");

	std::filesystem::remove(report, ignored);
	std::filesystem::remove(directory, ignored);
}

void TestPartialShutdownReason() {
	const auto directory = std::filesystem::temp_directory_path() / "kyty-performance-partial";
	std::filesystem::create_directories(directory);
	const auto report = directory / "capture.json";
	std::error_code ignored;
	std::filesystem::remove(report, ignored);

	auto options           = EnabledOptions();
	options.report_path    = report;
	options.capture_frames = 10;
	Performance::StartForTesting(options);
	Performance::Presentation(Performance::PresentationClass::Guest, 1);
	Performance::Shutdown();
	std::ifstream input(report);
	Json json;
	input >> json;
	Check(json.at("completion_reason") == "shutdown_partial",
	      "normal partial shutdown reason wrong");

	std::filesystem::remove(report, ignored);
	std::filesystem::remove(directory, ignored);
}

void TestGpuTimestampMath() {
	using Libs::Graphics::GpuTimestampCollector;
	uint64_t elapsed = 0;
	Check(GpuTimestampCollector::CalculateElapsedNanoseconds(100, 140, 64, 2.5f, elapsed) &&
	          elapsed == 100,
	      "GPU timestamp conversion failed");
	Check(GpuTimestampCollector::CalculateElapsedNanoseconds(250, 5, 8, 2.0f, elapsed) &&
	          elapsed == 22,
	      "GPU timestamp wrap conversion failed");
	Check(!GpuTimestampCollector::SupportsTimestampDevice(0, 1.0f),
	      "zero valid timestamp bits reported supported");
	Check(!GpuTimestampCollector::SupportsTimestampDevice(65, 1.0f),
	      "excess valid timestamp bits reported supported");
	Check(!GpuTimestampCollector::SupportsTimestampDevice(64,
	                                                      std::numeric_limits<float>::quiet_NaN()),
	      "NaN timestamp period reported supported");
	Check(!GpuTimestampCollector::CalculateElapsedNanoseconds(0, 1, 64, -1.0f, elapsed),
	      "invalid timestamp period converted");
}

void TestOutputFailureIsNonFatal() {
	auto options        = EnabledOptions();
	options.report_path = std::filesystem::temp_directory_path() /
	                      "kyty-directory-that-does-not-exist" / "capture.json";
	Performance::StartForTesting(options);
	Performance::EmergencyShutdown();
	Check(Performance::State() == Performance::CaptureState::Flushed,
	      "output failure prevented finalization");
}

} // namespace

int main() {
	TestDisabledPath();
	TestCaptureWindow();
	TestClassifications();
	TestAggregates();
	TestConcurrentUpdates();
	TestJsonAndPublication();
	TestIdempotentFinalization();
	TestEmergencyPendingGpuReport();
	TestUnlimitedShutdownCompletes();
	TestPartialShutdownReason();
	TestGpuTimestampMath();
	TestOutputFailureIsNonFatal();
	Performance::ResetForTesting();
	return 0;
}
