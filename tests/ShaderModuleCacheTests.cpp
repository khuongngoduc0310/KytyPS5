#include "graphics/host_gpu/renderer/pipeline/shaderModuleCache.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ShaderModuleCacheTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	const auto directory = std::filesystem::temp_directory_path() / "kyty-shader-cache-tests";
	const auto path      = directory / "cache.bin";
	std::error_code ignored;
	std::filesystem::remove_all(directory, ignored);
	std::filesystem::create_directories(directory);

	const std::vector<uint32_t> spirv{0x07230203u, 0x00010600u, 1u, 2u, 3u};
	{
		Libs::Graphics::PersistentShaderCache cache(path, "test-signature");
		cache.Insert(0x1234, spirv);
		Check(cache.Dirty(), "insert did not mark cache dirty");
		Check(cache.Save(), "cache save failed");
	}
	{
		Libs::Graphics::PersistentShaderCache cache(path, "test-signature");
		const auto value = cache.Find(0x1234);
		Check(value.has_value() && *value == spirv, "saved shader entry did not reload");
		Check(!cache.Find(0x5678).has_value(), "missing shader entry reported present");
	}
	{
		std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
		corrupt << "corrupt";
		corrupt.close();
		Libs::Graphics::PersistentShaderCache cache(path, "test-signature");
		Check(cache.Empty(), "corrupt cache was accepted");
	}

	std::filesystem::remove_all(directory, ignored);
	return 0;
}
