#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANFAULT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANFAULT_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Libs::Graphics {

enum class VulkanOperation : uint8_t {
	Unknown,
	Create,
	Submit,
	SemaphoreQuery,
	Query,
	SemaphoreWait,
	Acquire,
	Present,
	QueueIdle,
	DeviceIdle,
};

struct VulkanFaultContext {
	VulkanOperation operation = VulkanOperation::Unknown;
	vk::Result       result    = vk::Result::eSuccess;
	uint8_t          domain    = 0;
	uint64_t         tick      = 0;
	uint32_t         debug_op  = 0;
	uint64_t         debug_submit = 0;
	uint32_t         debug_arg0 = 0;
	uint32_t         debug_arg1 = 0;
	uint32_t         debug_arg2 = 0;
	uint32_t         debug_arg3 = 0;
	uint64_t         debug_arg4 = 0;
	uint32_t         frame_index = 0;
	bool             has_frame_index = false;
};

class VulkanFaultState final {
public:
	[[nodiscard]] bool IsDeviceLost() const noexcept {
		return m_latched.load(std::memory_order_acquire);
	}

	// Returns true only for first observation. Later observations preserve first metadata.
	bool Latch(const VulkanFaultContext& context) noexcept {
		std::lock_guard lock(m_mutex);
		if (m_latched.load(std::memory_order_relaxed)) {
			m_repeated.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		m_first = context;
		m_latched.store(true, std::memory_order_release);
		return true;
	}

	[[nodiscard]] VulkanFaultContext First() const noexcept {
		std::lock_guard lock(m_mutex);
		return m_first;
	}

	[[nodiscard]] uint64_t RepeatedCount() const noexcept {
		return m_repeated.load(std::memory_order_relaxed);
	}

private:
	mutable std::mutex      m_mutex;
	VulkanFaultContext      m_first {};
	std::atomic_bool        m_latched {false};
	std::atomic_uint64_t    m_repeated {0};
};

struct GraphicContext;

// Records only VK_ERROR_DEVICE_LOST. Other results remain owned by their existing callers.
bool RecordVulkanDeviceLoss(GraphicContext& graphics, const VulkanFaultContext& context);

[[nodiscard]] const char* VulkanOperationName(VulkanOperation operation) noexcept;

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANFAULT_H_
