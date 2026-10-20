#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	if (Config::OcclusionQueriesEnabled()) {
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eOcclusion;
		info.queryCount = OcclusionSlots;
		if (m_graphics.device.createQueryPool(&info, nullptr, &m_occlusion_pool) !=
		    vk::Result::eSuccess) {
			m_occlusion_pool = nullptr;
		} else {
			// Host-side reset is legal before any query is recorded (Vulkan 1.2).
			m_graphics.device.resetQueryPool(m_occlusion_pool, 0, OcclusionSlots);
		}
	}
}

int32_t RenderContext::OcclusionAcquire(uint64_t base_addr) noexcept {
	if (m_occlusion_pool == nullptr) {
		return -1;
	}
	std::lock_guard lock(m_occlusion_mutex);
	if (const auto it = m_occlusion_slots.find(base_addr); it != m_occlusion_slots.end()) {
		return static_cast<int32_t>(it->second);
	}
	if (m_occlusion_slots.size() >= OcclusionSlots) {
		return -1;
	}
	const auto slot = static_cast<uint32_t>(m_occlusion_slots.size());
	m_occlusion_slots.emplace(base_addr, slot);
	return static_cast<int32_t>(slot);
}

void RenderContext::OcclusionBegin(uint32_t slot, CommandBuffer& buffer) {
	if (m_occlusion_pool == nullptr || slot >= OcclusionSlots) {
		return;
	}
	buffer.Handle().beginQuery(m_occlusion_pool, slot, vk::QueryControlFlagBits::ePrecise);
}

void RenderContext::OcclusionEnd(uint32_t slot, CommandBuffer& buffer) {
	if (m_occlusion_pool == nullptr || slot >= OcclusionSlots) {
		return;
	}
	buffer.Handle().endQuery(m_occlusion_pool, slot);
}

bool RenderContext::OcclusionResolve(uint32_t slot, uint64_t& count) noexcept {
	if (m_occlusion_pool == nullptr || slot >= OcclusionSlots) {
		return false;
	}
	std::lock_guard lock(m_occlusion_mutex);
	uint64_t packed = 0;
	const auto result = m_graphics.device.getQueryPoolResults(
	    m_occlusion_pool, slot, 1, sizeof(packed), &packed, sizeof(packed),
	    vk::QueryResultFlagBits::e64);
	if (result != vk::Result::eSuccess) {
		return false;
	}
	m_graphics.device.resetQueryPool(m_occlusion_pool, slot, 1);
	count = packed;
	return true;
}

void RenderContext::OcclusionReset(uint32_t slot) noexcept {
	if (m_occlusion_pool == nullptr || slot >= OcclusionSlots) {
		return;
	}
	std::lock_guard lock(m_occlusion_mutex);
	m_graphics.device.resetQueryPool(m_occlusion_pool, slot, 1);
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
	if (m_occlusion_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_occlusion_pool, nullptr);
		m_occlusion_pool = nullptr;
	}
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}

	// Resolve against the whole cached buffer that contains the fault. Taking one fault per buffer
	// instead of one per 4 KiB page matters because BufferCache::ReadMemory does a synchronous
	// GPU-thread round-trip, and InvalidateRegion unprotects every page it covers, so a wider
	// resolution also prevents the follow-on faults.
	//
	// Textures keep the byte-scoped behaviour: their aliases are tracked separately and a wide
	// texture invalidation has no measured benefit here.
	constexpr uint64_t max_fault_range = 256u * 1024u;
	auto [range_addr, range_size] = m_buffer_cache.FindRegisteredRange(fault_vaddr);
	const bool range_ok = range_size != 0 && range_size <= max_fault_range;

	Common::DebugCounters::Add(range_ok ? Common::DebugCounters::Counter::FaultRangeResolved
	                                    : Common::DebugCounters::Counter::FaultByteFallback);

	if (access == PageFaultAccess::Write) {
		if (range_ok) {
			m_buffer_cache.InvalidateMemory(range_addr, range_size);
		} else {
			m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		}
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		if (range_ok) {
			m_buffer_cache.ReadMemory(range_addr, range_size);
		} else {
			m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
		}
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_buffer_cache.QueueBdaSync(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_buffer_cache.SynchronizeBda(m_mapped_ranges);
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
