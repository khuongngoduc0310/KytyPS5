#include "common/performanceTelemetry.h"
#include "graphics/host_gpu/vulkanFault.h"

#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "VulkanFaultTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	using namespace Libs::Graphics;

	VulkanFaultState state;
	Check(!state.IsDeviceLost(), "fresh state is latched");

	VulkanFaultContext first {};
	first.operation = VulkanOperation::Submit;
	first.result    = vk::Result::eErrorDeviceLost;
	first.domain    = 1;
	first.tick      = 421168;
	Check(state.Latch(first), "first observation did not latch");
	Check(state.IsDeviceLost(), "latched state is not visible");

	VulkanFaultContext repeated {};
	repeated.operation = VulkanOperation::SemaphoreWait;
	repeated.result    = vk::Result::eErrorDeviceLost;
	repeated.tick      = 999;
	Check(!state.Latch(repeated), "repeated observation replaced first observation");
	Check(state.RepeatedCount() == 1, "repeated observation was not counted");

	const auto snapshot = state.First();
	Check(snapshot.operation == VulkanOperation::Submit && snapshot.tick == 421168,
	      "first observation metadata changed");
	Check(snapshot.domain == 1, "first observation domain changed");

	Performance::StartForTesting({}, {});
	Performance::RecordDeviceLoss("submit", -4, 421168, 1);
	const auto report = nlohmann::json::parse(
	    Performance::BuildJsonForTesting(Performance::CompletionReason::DeviceLost));
	Check(report.at("completion_reason") == "device_lost",
	      "device loss did not select terminal report reason");
	Check(report.at("device_loss").at("detected") == true,
	      "device loss missing from telemetry report");
	Check(report.at("device_loss").at("operation") == "submit",
	      "device loss operation missing from telemetry report");
	Check(report.at("device_loss").at("tick") == 421168,
	      "device loss tick missing from telemetry report");
	Performance::ResetForTesting();
	return 0;
}
