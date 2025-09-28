#include "gdx/gdx_metadata_cache.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace gdx {

GDXMetadataCache &GDXMetadataCache::Get() {
	static GDXMetadataCache instance;
	return instance;
}

std::shared_ptr<const GDXMetadataEntry> GDXMetadataCache::Lookup(const std::string &resolved_path) const {
	auto key = resolved_path;
	std::lock_guard<std::mutex> lock(mutex);
	auto it = entries.find(key);
	if (it == entries.end()) {
		return nullptr;
	}
	return it->second;
}

std::shared_ptr<GDXMetadataEntry> GDXMetadataCache::GetOrLoad(ClientContext &context, const std::string &file_or_url,
                                                             bool force_reload) {
	GDXFileRandomAccessProvider provider;
	provider.Initialize(context, file_or_url);
	return GetOrLoad(context, provider, force_reload);
}

std::shared_ptr<GDXMetadataEntry> GDXMetadataCache::GetOrLoad(ClientContext &context,
                                                             GDXFileRandomAccessProvider &provider,
                                                             bool force_reload) {
	auto resolved = provider.ResolvedPath().empty() ? provider.Location() : provider.ResolvedPath();
	if (!force_reload) {
		std::lock_guard<std::mutex> lock(mutex);
		auto it = entries.find(resolved);
		if (it != entries.end()) {
			return it->second;
		}
	} else {
		std::lock_guard<std::mutex> lock(mutex);
		entries.erase(resolved);
	}

	auto entry = LoadGDXMetadata(context, provider);
	{
		std::lock_guard<std::mutex> lock(mutex);
		entries[entry->resolved_path] = entry;
	}
	return entry;
}

void GDXMetadataCache::Invalidate(const std::string &resolved_path) {
	std::lock_guard<std::mutex> lock(mutex);
	entries.erase(resolved_path);
}

} // namespace gdx
} // namespace duckdb
