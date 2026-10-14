#include "graphics/host_gpu/renderer/gpuTimestampCollector.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>

namespace Libs::Graphics {

GpuTimestampCollector::GpuTimestampCollector(GraphicContext& graphics, MasterSemaphore& master,
                                             Performance::SchedulerDomain domain)
    : m_graphics(graphics), m_master(master), m_domain(domain) {
	if (!Performance::Enabled() ||
	    !SupportsTimestampDevice(graphics.queue_timestamp_valid_bits,
	                             graphics.timestamp_period_nanoseconds)) {
		return;
	}
	vk::QueryPoolCreateInfo create{};
	create.queryType  = vk::QueryType::eTimestamp;
	create.queryCount = QueryPairCount * 2;
	const auto result = graphics.device.createQueryPool(&create, nullptr, &m_pool);
	if (result != vk::Result::eSuccess) {
		RecordVulkanDeviceLoss(graphics,
		                       {.operation = VulkanOperation::Create, .result = result,
		                        .domain = static_cast<uint8_t>(domain)});
		m_pool = nullptr;
	}
}

GpuTimestampCollector::~GpuTimestampCollector() {
	if (m_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_pool, nullptr);
	}
}

void GpuTimestampCollector::ReportUnsupported() {
	if (Performance::Collecting() && !m_unsupported_reported) {
		m_unsupported_reported = true;
		Performance::Add(Performance::Counter::GpuTimingUnsupported);
	}
}

std::optional<GpuTimestampCollector::Token>
GpuTimestampCollector::Begin(vk::CommandBuffer command) {
	if (!Performance::Collecting()) {
		return std::nullopt;
	}
	if (m_pool == nullptr) {
		ReportUnsupported();
		return std::nullopt;
	}
	Collect();
	for (uint32_t offset = 0; offset < QueryPairCount; offset++) {
		const auto token = (m_hint + offset) % QueryPairCount;
		if (m_slots[token].state != SlotState::Free) {
			continue;
		}
		m_slots[token].state   = SlotState::Recording;
		m_slots[token].tick    = 0;
		m_hint                 = (token + 1) % QueryPairCount;
		const auto first_query = token * 2;
		command.resetQueryPool(m_pool, first_query, 2);
		command.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, m_pool, first_query);
		return token;
	}
	Performance::Add(Performance::Counter::GpuQueryExhaustion);
	return std::nullopt;
}

void GpuTimestampCollector::End(vk::CommandBuffer command, std::optional<Token> token) {
	if (!token.has_value()) {
		return;
	}
	if (*token >= QueryPairCount || m_slots[*token].state != SlotState::Recording) {
		return;
	}
	command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, m_pool, *token * 2 + 1);
}

void GpuTimestampCollector::Submitted(std::optional<Token> token, uint64_t tick) {
	if (!token.has_value()) {
		return;
	}
	if (*token >= QueryPairCount || m_slots[*token].state != SlotState::Recording) {
		return;
	}
	auto& slot = m_slots[*token];
	slot.state = SlotState::Pending;
	slot.tick  = tick;
	m_pending++;
	Performance::Add(Performance::Counter::GpuTimestampSubmissions);
	Performance::SetGauge(Performance::Gauge::PendingGpuSamples, m_pending);
	Performance::MaxGauge(Performance::Gauge::PendingGpuSamplesHighWater, m_pending);
}

void GpuTimestampCollector::Release(Token token) {
	auto& slot = m_slots[token];
	slot       = {};
	if (m_pending != 0) {
		m_pending--;
	}
	Performance::SetGauge(Performance::Gauge::PendingGpuSamples, m_pending);
}

void GpuTimestampCollector::Collect() {
	if (m_pool == nullptr || m_pending == 0) {
		return;
	}
	const auto completed_tick = m_master.KnownGpuTick();
	struct QueryResult {
		uint64_t value;
		uint64_t available;
	};
	for (Token token = 0; token < QueryPairCount; token++) {
		auto& slot = m_slots[token];
		if (slot.state != SlotState::Pending || slot.tick > completed_tick) {
			continue;
		}
		std::array<QueryResult, 2> results{};
		const auto result = m_graphics.device.getQueryPoolResults(
		    m_pool, token * 2, 2, sizeof(results), results.data(), sizeof(QueryResult),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		if (result == vk::Result::eErrorDeviceLost) {
			RecordVulkanDeviceLoss(m_graphics,
			                       {.operation = VulkanOperation::Query,
			                        .result = result,
			                        .domain = static_cast<uint8_t>(m_domain),
			                        .tick = slot.tick});
			Release(token);
			return;
		}
		if (result != vk::Result::eSuccess || results[0].available == 0 ||
		    results[1].available == 0) {
			Performance::Add(Performance::Counter::GpuUnavailableResults);
			Release(token);
			continue;
		}
		uint64_t elapsed = 0;
		if (!CalculateElapsedNanoseconds(results[0].value, results[1].value,
		                                 m_graphics.queue_timestamp_valid_bits,
		                                 m_graphics.timestamp_period_nanoseconds, elapsed)) {
			Performance::Add(Performance::Counter::GpuUnavailableResults);
			Release(token);
			continue;
		}
		Performance::Observe(m_domain == Performance::SchedulerDomain::Guest
		                         ? Performance::Duration::GuestGpuSubmission
								 : Performance::Duration::PresentationGpuSubmission,
		                     elapsed);
		Performance::Add(Performance::Counter::GpuTimestampResults);
		Release(token);
	}
}

void GpuTimestampCollector::Drain() {
	if (m_pool == nullptr) {
		return;
	}
	if (!m_master.Refresh()) {
		for (Token token = 0; token < QueryPairCount; token++) {
			if (m_slots[token].state != SlotState::Free) {
				Performance::Add(Performance::Counter::GpuUnavailableResults);
				Release(token);
			}
		}
		return;
	}
	Collect();
	for (Token token = 0; token < QueryPairCount; token++) {
		if (m_slots[token].state != SlotState::Free) {
			Performance::Add(Performance::Counter::GpuUnavailableResults);
			Release(token);
		}
	}
}

} // namespace Libs::Graphics
