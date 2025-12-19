#include "gdx/gdx_metadata_cache.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"

// Sidecar feature requires nlohmann JSON which is not available in WASM builds
#ifndef __EMSCRIPTEN__
#include "gdx/gdx_sidecar.hpp"
#define GDX_SIDECAR_ENABLED 0
#else
#define GDX_SIDECAR_ENABLED 0
#endif

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

std::shared_ptr<GDXMetadataEntry>
GDXMetadataCache::GetOrLoad(ClientContext &context, GDXFileRandomAccessProvider &provider, bool force_reload) {
	auto resolved = provider.ResolvedPath().empty() ? provider.Location() : provider.ResolvedPath();

#if GDX_SIDECAR_ENABLED
	// Sidecar attempt
	if (!force_reload) {
		auto &fs = FileSystem::GetFileSystem(context);
		std::string sidecar_path = resolved + ".gdxi";
		if (fs.FileExists(sidecar_path)) {
			try {
				auto sidecar_handle = fs.OpenFile(sidecar_path, FileFlags::FILE_FLAGS_READ);
				if (sidecar_handle) {
					const auto sidecar_size = fs.GetFileSize(*sidecar_handle);
					std::string sc_content;
					sc_content.resize(static_cast<size_t>(sidecar_size));
					if (sidecar_size > 0) {
						fs.Read(*sidecar_handle, sc_content.data(), sidecar_size, 0);
					}
					auto sc = LoadSidecar(sc_content);
					auto data_handle = fs.OpenFile(resolved, FileFlags::FILE_FLAGS_READ);
					if (data_handle) {
						uint64_t fsize = fs.GetFileSize(*data_handle);
						int64_t fmtime = fs.GetLastModifiedTime(*data_handle);
						if (IsSidecarFresh(sc, resolved, fsize, fmtime)) {
							auto entry = std::make_shared<GDXMetadataEntry>();
							entry->resolved_path = sc.source_path;
							entry->is_remote = provider.IsRemote();
							entry->symbols.reserve(sc.symbols.size());
							for (auto &s : sc.symbols) {
								GDXSymbolMetadata md;
								md.name = s.name;
								md.type_code = s.type_code;
								md.dimension_count = s.dimension_count;
								md.record_count = s.record_count;
								md.data_position = s.data_position;
								md.domain_labels = s.domain_labels;
								md.cached_domain_values = s.cached_domain_values;
								entry->symbols.emplace_back(std::move(md));
							}
							{
								std::lock_guard<std::mutex> lock(mutex);
								entries[entry->resolved_path] = entry;
							}
							return entry;
						}
					}
				}
			} catch (...) {
				// Ignore sidecar errors; fall back to load
			}
		}
	}
#endif // GDX_SIDECAR_ENABLED
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

#if GDX_SIDECAR_ENABLED
	// Write sidecar
	try {
		auto &fs = FileSystem::GetFileSystem(context);
		GDXSidecarFile sc;
		sc.source_path = entry->resolved_path;
		{
			auto data_handle = fs.OpenFile(entry->resolved_path, FileFlags::FILE_FLAGS_READ);
			if (data_handle) {
				sc.file_size = fs.GetFileSize(*data_handle);
				sc.modified_time = fs.GetLastModifiedTime(*data_handle);
			}
		}
		sc.hash_hint = ""; // optional future use
		sc.symbols.reserve(entry->symbols.size());
		for (auto &md : entry->symbols) {
			GDXSidecarSymbol s;
			s.name = md.name;
			s.type_code = md.type_code;
			s.dimension_count = md.dimension_count;
			s.record_count = md.record_count;
			s.data_position = md.data_position;
			s.domain_labels = md.domain_labels;
			s.cached_domain_values = md.cached_domain_values;
			sc.symbols.emplace_back(std::move(s));
		}
		auto json = WriteSidecar(sc);
		std::string out_path = entry->resolved_path + ".gdxi";
		fs.TryRemoveFile(out_path);
		auto out_handle = fs.OpenFile(out_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		if (out_handle && !json.empty()) {
			fs.Write(*out_handle, const_cast<char *>(json.data()), json.size(), 0);
			fs.FileSync(*out_handle);
		}
	} catch (...) {
		// best-effort; ignore failures
	}
#endif // GDX_SIDECAR_ENABLED
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
