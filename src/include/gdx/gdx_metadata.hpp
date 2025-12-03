#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
class ClientContext;

namespace gdx {

struct GDXSymbolMetadata {
	std::string name;
	int32_t type_code {0};
	uint64_t dimension_count {0};
	uint64_t record_count {0};
	std::string description;
	std::vector<std::string> domain_labels;
	int32_t symbol_index {0};
};

//! Cache of unique domain values per dimension for a symbol.
//! Key: symbol name (lowercase), Value: vector of vectors (one per dimension)
struct GDXSymbolDomainValuesCache {
	mutable std::mutex mutex;
	//! Map from symbol name (lowercase) to vector of unique values per dimension
	std::unordered_map<std::string, std::vector<std::vector<std::string>>> symbol_domain_values;
	
	bool HasCachedValues(const std::string &symbol_name) const;
	const std::vector<std::vector<std::string>> *GetCachedValues(const std::string &symbol_name) const;
	void SetCachedValues(const std::string &symbol_name, std::vector<std::vector<std::string>> values);
};

struct GDXMetadataEntry {
	std::string resolved_path;
	bool is_remote {false};
	std::vector<GDXSymbolMetadata> symbols;
	
	//! Cache for domain values - populated lazily on first request
	GDXSymbolDomainValuesCache domain_values_cache;
};

class GDXFileRandomAccessProvider;

//! Load the metadata (symbol list and basic attributes) for a GDX file using an already initialized provider.
std::shared_ptr<GDXMetadataEntry> LoadGDXMetadata(ClientContext &context, GDXFileRandomAccessProvider &provider);

//! Convenience wrapper that instantiates a provider based on the supplied path/URL.
std::shared_ptr<GDXMetadataEntry> LoadGDXMetadata(ClientContext &context, const std::string &file_or_url);

} // namespace gdx
} // namespace duckdb
