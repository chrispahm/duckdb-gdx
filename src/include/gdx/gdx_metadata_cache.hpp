#pragma once

#include "gdx/gdx_metadata.hpp"
#include "gdx/gdx_file_provider.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace duckdb {
class ClientContext;

namespace gdx {

class GDXMetadataCache {
public:
	static GDXMetadataCache &Get();

	std::shared_ptr<const GDXMetadataEntry> Lookup(const std::string &resolved_path) const;

	std::shared_ptr<GDXMetadataEntry> GetOrLoad(ClientContext &context, const std::string &file_or_url,
	                                            bool force_reload = false);

	std::shared_ptr<GDXMetadataEntry> GetOrLoad(ClientContext &context, GDXFileRandomAccessProvider &provider,
	                                            bool force_reload = false);

	void Invalidate(const std::string &resolved_path);

private:
	mutable std::mutex mutex;
	std::unordered_map<std::string, std::shared_ptr<GDXMetadataEntry>> entries;
};

} // namespace gdx
} // namespace duckdb
