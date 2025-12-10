#pragma once

#include "duckdb.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace gdx {

struct GDXSidecarSymbol {
	std::string name;
	int32_t type_code {0};
	uint64_t dimension_count {0};
	uint64_t record_count {0};
	uint64_t data_position {0};
	std::vector<std::string> domain_labels;
	std::vector<std::vector<std::string>> cached_domain_values;
};

struct GDXSidecarFile {
	std::string source_path;
	uint64_t file_size {0};
	int64_t modified_time {0};
	std::string hash_hint;
	std::vector<GDXSidecarSymbol> symbols;
};

// Load a sidecar from JSON (throws on parse errors).
GDXSidecarFile LoadSidecar(const std::string &json);

// Serialize sidecar to JSON.
std::string WriteSidecar(const GDXSidecarFile &sidecar);

// Check staleness against current file stats; returns true if still valid.
bool IsSidecarFresh(const GDXSidecarFile &sidecar, const std::string &resolved_path, uint64_t size,
	int64_t mtime);

} // namespace gdx
} // namespace duckdb