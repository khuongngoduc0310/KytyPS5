#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPCOLLECTOR_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPCOLLECTOR_H_

#include "common/common.h"
#include "common/performanceTelemetry.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

class GpuTimestampCollector final {
  public:
	using Token                              = uint32_t;
	static constexpr uint32_t QueryPairCount = 256;

	GpuTimestampCollector(GraphicContext& graphics, MasterSemaphore& master,
	                      Performance::SchedulerDomain domain);
	~GpuTimestampCollector();
	KYTY_CLASS_NO_COPY(GpuTimestampCollector);

	[[nodiscard]] std::optional<Token> Begin(vk::CommandBuffer command);
	void End(vk::CommandBuffer command, std::optional<Token> token);
	void Submitted(std::optional<Token> token, uint64_t tick);
	void Collect();
	void Drain();

	[[nodiscard]] bool Supported() const noexcept { return m_pool != nullptr; }
	[[nodiscard]] static bool SupportsTimestampDevice(uint32_t valid_bits,
	                                                  float timestamp_period) noexcept {
		return valid_bits != 0 && valid_bits <= 64 && std::isfinite(timestamp_period) &&
		       timestamp_period > 0.0f;
	}
	[[nodiscard]] static bool CalculateElapsedNanoseconds(uint64_t begin, uint64_t end,
	                                                      uint32_t valid_bits,
	                                                      float timestamp_period,
	                                                      uint64_t& elapsed_nanoseconds) noexcept {
		if (!SupportsTimestampDevice(valid_bits, timestamp_period)) {
			return false;
		}
		const uint64_t mask           = valid_bits == 64 ? std::numeric_limits<uint64_t>::max()
		                                                 : (uint64_t{1} << valid_bits) - 1;
		const uint64_t ticks          = (end - begin) & mask;
		const long double nanoseconds = static_cast<long double>(ticks) * timestamp_period;
		if (nanoseconds < 0.0L ||
		    nanoseconds > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
			return false;
		}
		elapsed_nanoseconds = static_cast<uint64_t>(nanoseconds);
		return true;
	}

  private:
	enum class SlotState : uint8_t { Free, Recording, Pending };
	struct Slot {
		SlotState state = SlotState::Free;
		uint64_t tick   = 0;
	};

	void ReportUnsupported();
	void Release(Token token);

	GraphicContext& m_graphics;
	MasterSemaphore& m_master;
	Performance::SchedulerDomain m_domain;
	vk::QueryPool m_pool = nullptr;
	std::array<Slot, QueryPairCount> m_slots{};
	uint32_t m_hint             = 0;
	uint32_t m_pending          = 0;
	bool m_unsupported_reported = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPCOLLECTOR_H_
