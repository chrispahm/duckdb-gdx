#pragma once

#include <cstdint>
#include <memory>
#include <string>
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

struct GDXMetadataEntry {
	std::string resolved_path;
	bool is_remote {false};
	std::vector<GDXSymbolMetadata> symbols;
};

class GDXFileRandomAccessProvider;

//! Load the metadata (symbol list and basic attributes) for a GDX file using an already initialized provider.
std::shared_ptr<GDXMetadataEntry> LoadGDXMetadata(ClientContext &context, GDXFileRandomAccessProvider &provider);

//! Convenience wrapper that instantiates a provider based on the supplied path/URL.
std::shared_ptr<GDXMetadataEntry> LoadGDXMetadata(ClientContext &context, const std::string &file_or_url);

} // namespace gdx
} // namespace duckdb
