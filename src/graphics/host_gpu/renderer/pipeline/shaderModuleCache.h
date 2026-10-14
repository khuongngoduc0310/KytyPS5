#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERMODULECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERMODULECACHE_H_

#include "common/common.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class PersistentShaderCache final {
  public:
	PersistentShaderCache() = default;
	PersistentShaderCache(std::filesystem::path path, std::string signature);
	~PersistentShaderCache() = default;
	KYTY_CLASS_NO_COPY(PersistentShaderCache);

	[[nodiscard]] std::optional<std::vector<uint32_t>> Find(uint64_t key) const;
	void Insert(uint64_t key, std::span<const uint32_t> spirv);
	void Erase(uint64_t key);
	[[nodiscard]] bool Empty() const noexcept { return m_entries.empty(); }
	[[nodiscard]] bool Dirty() const noexcept { return m_dirty; }
	[[nodiscard]] bool Save();

  private:
	void Load();

	std::filesystem::path m_path;
	std::string m_signature;
	std::unordered_map<uint64_t, std::vector<uint32_t>> m_entries;
	bool m_dirty = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERMODULECACHE_H_
