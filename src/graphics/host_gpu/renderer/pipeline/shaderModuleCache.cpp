#include "graphics/host_gpu/renderer/pipeline/shaderModuleCache.h"

#include "common/file.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <xxhash.h>

namespace Libs::Graphics {
namespace {

constexpr std::array<char, 8> kMagic{'K', 'Y', 'T', 'Y', 'S', 'C', '2', '\0'};
constexpr uint32_t kMaxEntries       = 16384;
constexpr uint64_t kMaxFileBytes     = 128ull * 1024 * 1024;
constexpr uint32_t kMaxWordsPerEntry = 8 * 1024 * 1024;

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		bytes.push_back(static_cast<uint8_t>(value >> shift));
	}
}

void AppendU64(std::vector<uint8_t>& bytes, uint64_t value) {
	for (uint32_t shift = 0; shift < 64; shift += 8) {
		bytes.push_back(static_cast<uint8_t>(value >> shift));
	}
}

bool ReadU32(std::span<const uint8_t> bytes, size_t& offset, uint32_t& value) {
	if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t)) {
		return false;
	}
	value = 0;
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		value |= static_cast<uint32_t>(bytes[offset++]) << shift;
	}
	return true;
}

bool ReadU64(std::span<const uint8_t> bytes, size_t& offset, uint64_t& value) {
	if (offset > bytes.size() || bytes.size() - offset < sizeof(uint64_t)) {
		return false;
	}
	value = 0;
	for (uint32_t shift = 0; shift < 64; shift += 8) {
		value |= static_cast<uint64_t>(bytes[offset++]) << shift;
	}
	return true;
}

} // namespace

PersistentShaderCache::PersistentShaderCache(std::filesystem::path path, std::string signature)
    : m_path(std::move(path)), m_signature(std::move(signature)) {
	Load();
}

std::optional<std::vector<uint32_t>> PersistentShaderCache::Find(uint64_t key) const {
	const auto found = m_entries.find(key);
	if (found == m_entries.end()) {
		return std::nullopt;
	}
	return found->second;
}

void PersistentShaderCache::Insert(uint64_t key, std::span<const uint32_t> spirv) {
	if (spirv.empty() || spirv.size() > kMaxWordsPerEntry) {
		return;
	}
	std::vector<uint32_t> copy(spirv.begin(), spirv.end());
	const auto [entry, inserted] = m_entries.insert_or_assign(key, std::move(copy));
	(void)entry;
	(void)inserted;
	if (m_entries.size() > kMaxEntries) {
		auto oldest = m_entries.begin();
		m_entries.erase(oldest);
	}
	m_dirty = true;
}

void PersistentShaderCache::Erase(uint64_t key) {
	if (m_entries.erase(key) != 0) {
		m_dirty = true;
	}
}

void PersistentShaderCache::Load() {
	if (m_path.empty() || m_signature.empty() || !Common::File::IsFileExisting(m_path)) {
		return;
	}
	std::ifstream input(m_path, std::ios::binary | std::ios::ate);
	if (!input) {
		return;
	}
	const auto end = input.tellg();
	if (end < 0 || static_cast<uint64_t>(end) > kMaxFileBytes) {
		return;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(end));
	input.seekg(0);
	if (!bytes.empty()) {
		input.read(reinterpret_cast<char*>(bytes.data()),
		           static_cast<std::streamsize>(bytes.size()));
	}
	if (!input || bytes.size() < kMagic.size() + 4 + 4 + 8 + 8) {
		return;
	}

	size_t offset = 0;
	if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
		return;
	}
	offset += kMagic.size();
	uint32_t signature_size = 0;
	uint32_t entry_count    = 0;
	uint64_t payload_size   = 0;
	uint64_t payload_hash   = 0;
	if (!ReadU32(bytes, offset, signature_size) || !ReadU32(bytes, offset, entry_count) ||
	    !ReadU64(bytes, offset, payload_size) || !ReadU64(bytes, offset, payload_hash) ||
	    signature_size != m_signature.size() || entry_count > kMaxEntries ||
	    payload_size > kMaxFileBytes || signature_size > bytes.size() - offset ||
	    payload_size != bytes.size() - offset - signature_size) {
		return;
	}
	if (!std::equal(m_signature.begin(), m_signature.end(), bytes.begin() + offset)) {
		return;
	}
	offset += signature_size;
	const auto payload = std::span<const uint8_t>(bytes).subspan(offset);
	if (XXH3_64bits(payload.data(), payload.size()) != payload_hash) {
		return;
	}

	std::unordered_map<uint64_t, std::vector<uint32_t>> entries;
	entries.reserve(entry_count);
	size_t payload_offset = 0;
	for (uint32_t index = 0; index < entry_count; index++) {
		uint64_t key   = 0;
		uint32_t words = 0;
		if (!ReadU64(payload, payload_offset, key) || !ReadU32(payload, payload_offset, words) ||
		    words == 0 || words > kMaxWordsPerEntry || payload_offset > payload.size() ||
		    words > (payload.size() - payload_offset) / sizeof(uint32_t)) {
			return;
		}
		std::vector<uint32_t> spirv(words);
		for (uint32_t& word : spirv) {
			if (!ReadU32(payload, payload_offset, word)) {
				return;
			}
		}
		if (!entries.emplace(key, std::move(spirv)).second) {
			return;
		}
	}
	if (payload_offset != payload.size()) {
		return;
	}
	m_entries = std::move(entries);
}

bool PersistentShaderCache::Save() {
	if (!m_dirty) {
		return true;
	}
	if (m_path.empty() || m_signature.empty() || m_entries.empty()) {
		m_dirty = false;
		return true;
	}

	std::vector<uint8_t> payload;
	std::vector<uint64_t> keys;
	keys.reserve(m_entries.size());
	for (const auto& [key, spirv] : m_entries) {
		keys.push_back(key);
	}
	std::ranges::sort(keys);
	for (const auto key : keys) {
		const auto& spirv = m_entries.at(key);
		AppendU64(payload, key);
		AppendU32(payload, static_cast<uint32_t>(spirv.size()));
		for (const auto word : spirv) {
			AppendU32(payload, word);
		}
	}
	if (payload.size() > kMaxFileBytes ||
	    m_signature.size() > std::numeric_limits<uint32_t>::max()) {
		return false;
	}

	std::vector<uint8_t> bytes;
	bytes.reserve(kMagic.size() + 4 + 4 + 8 + 8 + m_signature.size() + payload.size());
	bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
	AppendU32(bytes, static_cast<uint32_t>(m_signature.size()));
	AppendU32(bytes, static_cast<uint32_t>(m_entries.size()));
	AppendU64(bytes, payload.size());
	AppendU64(bytes, XXH3_64bits(payload.data(), payload.size()));
	bytes.insert(bytes.end(), m_signature.begin(), m_signature.end());
	bytes.insert(bytes.end(), payload.begin(), payload.end());

	if (!Common::File::CreateDirectories(m_path.parent_path())) {
		return false;
	}
	auto temporary = m_path;
	temporary += ".tmp";
	std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
	if (!output) {
		return false;
	}
	output.write(reinterpret_cast<const char*>(bytes.data()),
	             static_cast<std::streamsize>(bytes.size()));
	output.flush();
	output.close();
	if (!output || !Common::File::RenameFile(temporary, m_path)) {
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		return false;
	}
	m_dirty = false;
	return true;
}

} // namespace Libs::Graphics
