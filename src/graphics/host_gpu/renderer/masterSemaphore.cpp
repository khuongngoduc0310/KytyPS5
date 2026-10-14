#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <chrono>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics, Performance::SchedulerDomain domain)
    : m_graphics(graphics), m_domain(domain) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

bool MasterSemaphore::Refresh() {
	if (m_graphics.vulkan_fault.IsDeviceLost()) {
		return false;
	}
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		RecordVulkanDeviceLoss(m_graphics, {.operation = VulkanOperation::SemaphoreQuery,
		                                    .result = result,
		                                    .domain = static_cast<uint8_t>(m_domain)});
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eErrorDeviceLost);
		return false;
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
	return true;
}

bool MasterSemaphore::Wait(uint64_t tick, Performance::WaitReason reason) {
	if (m_graphics.vulkan_fault.IsDeviceLost()) {
		return false;
	}
	if (IsFree(tick)) {
		Performance::RecordWait(reason, 0);
		return true;
	}
	if (!Refresh()) {
		return false;
	}
	if (IsFree(tick)) {
		Performance::RecordWait(reason, 0);
		return true;
	}

	const bool telemetry_enabled = Performance::Enabled();
	const auto begin = telemetry_enabled ? std::chrono::steady_clock::now()
	                                     : std::chrono::steady_clock::time_point {};
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		RecordVulkanDeviceLoss(m_graphics, {.operation = VulkanOperation::SemaphoreWait,
		                                    .result = result,
		                                    .domain = static_cast<uint8_t>(m_domain),
		                                    .tick = tick});
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eErrorDeviceLost);
		return false;
	}
	if (!Refresh()) {
		return false;
	}
	uint64_t elapsed_ns = 0;
	if (telemetry_enabled) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    std::chrono::steady_clock::now() - begin);
		elapsed_ns = static_cast<uint64_t>(std::max<int64_t>(0, elapsed.count()));
	}
	Performance::RecordWait(reason, elapsed_ns);
	return true;
}

} // namespace Libs::Graphics
