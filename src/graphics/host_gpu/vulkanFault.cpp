#include "graphics/host_gpu/vulkanFault.h"

#include "common/logging/log.h"
#include "common/performanceTelemetry.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdio>

namespace Libs::Graphics {

const char* VulkanOperationName(VulkanOperation operation) noexcept {
	switch (operation) {
		case VulkanOperation::Create: return "create";
		case VulkanOperation::Submit: return "submit";
		case VulkanOperation::SemaphoreQuery: return "semaphore-query";
		case VulkanOperation::Query: return "query";
		case VulkanOperation::SemaphoreWait: return "semaphore-wait";
		case VulkanOperation::Acquire: return "acquire";
		case VulkanOperation::Present: return "present";
		case VulkanOperation::QueueIdle: return "queue-idle";
		case VulkanOperation::DeviceIdle: return "device-idle";
		case VulkanOperation::Unknown: break;
	}
	return "unknown";
}

bool RecordVulkanDeviceLoss(GraphicContext& graphics, const VulkanFaultContext& context) {
	if (context.result != vk::Result::eErrorDeviceLost) {
		return false;
	}
	if (graphics.vulkan_fault.Latch(context)) {
		Performance::RecordDeviceLoss(VulkanOperationName(context.operation),
		                              static_cast<int>(context.result), context.tick, context.domain);
		const auto message = fmt::sprintf(
		    "Vulkan device lost: operation=%s result=%s (%d) domain=%u tick=%" PRIu64
		    " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64
		    "%s\n",
		    VulkanOperationName(context.operation), vk::to_string(context.result).c_str(),
		    static_cast<int>(context.result), context.domain, context.tick, context.debug_op,
		    context.debug_submit, context.debug_arg0, context.debug_arg1, context.debug_arg2,
		    context.debug_arg3, context.debug_arg4,
		    context.has_frame_index ? fmt::sprintf(" frame=%u", context.frame_index).c_str() : "");
		LOGF("%s", message.c_str());
		std::printf("%s", message.c_str());
		std::fflush(stdout);
	}
	return true;
}

} // namespace Libs::Graphics
