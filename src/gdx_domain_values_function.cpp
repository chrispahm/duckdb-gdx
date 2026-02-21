#include "gdx/gdx_domain_values_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/execution/execution_context.hpp"

#include "gdx/gdx_error.hpp"
#include "gdx/gdx_file_provider.hpp"
#define NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_handle.hpp"
#undef NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_metadata_cache.hpp"
#include "gdx/gdx_symbol_utils.hpp"

#include "gclgms.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace duckdb {
namespace gdx {

namespace {

struct DomainValuesBindData : public TableFunctionData {
	std::string file_or_url;
	std::string resolved_path;
	std::string symbol;
	idx_t dimension_index {0}; // 0-based dimension index to extract values from
	idx_t dimension_count {0};
	std::string dimension_name; // For error messages
	// Pointer to cached metadata (for accessing domain values cache)
	std::shared_ptr<GDXMetadataEntry> metadata;
	// Optional dimension filters for cascading filter support
	std::vector<idx_t> dimension_filter_indices;
	std::vector<std::string> dimension_filter_values;
	bool has_dimension_filters {false};
};

struct DomainValuesGlobalState : public GlobalTableFunctionState {
	// Cached unique values for the requested dimension
	const std::vector<std::string> *cached_values {nullptr};

	idx_t MaxThreads() const override {
		return 1;
	}
};

struct DomainValuesLocalState : public LocalTableFunctionState {
	idx_t current_index {0};
};

// Scan dimensions for a symbol with optional filtering
// When filters are provided, only records matching the filters are scanned
// This enables cascading filter behavior (e.g., selecting Country limits available Cities)
void ScanAndCacheDomainValues(ClientContext &context, const std::string &file_or_url, const std::string &symbol,
                              idx_t dimension_count, GDXMetadataEntry &metadata_entry,
                              const std::vector<idx_t> &filter_indices = {},
                              const std::vector<std::string> &filter_values = {}) {
	// Open file
	GDXFileRandomAccessProvider provider;
	provider.Initialize(context, file_or_url);

	auto handle = CreateGDXHandle();
	int open_error = 0;
	if (!gdxOpenReadFromRandomAccess(handle.get(), &provider.GetCallbacks(), &open_error)) {
		GDXErrorContext error_context("gdxOpenReadFromRandomAccess");
		error_context.WithFile(file_or_url);
		ThrowGDXError(open_error, error_context);
	}

	// Find symbol
	int sym_nr = 0;
	if (!gdxFindSymbol(handle.get(), symbol.c_str(), &sym_nr)) {
		gdxClose(handle.get());
		throw InvalidInputException("Symbol '%s' not found in GDX file", symbol);
	}

	// Preload UEL table using gdxUMUelGet (takes internal entry numbers directly)
	// Note: Raw indices from gdxDataReadRaw are internal entry numbers, not user UEL numbers
	int uel_count = 0, high_map = 0;
	gdxUMUelInfo(handle.get(), &uel_count, &high_map);

	std::vector<std::string> uel_table(static_cast<size_t>(uel_count) + 1);
	std::array<char, GMS_SSSIZE> uel_buffer {};
	int uel_map = 0;
	for (int i = 1; i <= uel_count; ++i) {
		// gdxUMUelGet takes an internal entry number (UelNr) and returns the UEL string
		if (gdxUMUelGet(handle.get(), i, uel_buffer.data(), &uel_map)) {
			uel_table[i] = std::string(uel_buffer.data());
		} else {
			uel_table[i] = std::to_string(i);
		}
	}

	// Prepare sets for each dimension
	std::vector<std::unordered_set<int>> unique_indices_per_dim(dimension_count);

	std::vector<int> key_buffer(dimension_count);
	std::array<double, GMS_VAL_MAX> value_buffer {};
	int afdim = 0;

	bool use_filtered_read = !filter_indices.empty() && filter_indices.size() == filter_values.size();

	if (use_filtered_read) {
		// Register UEL mappings for filter values
		if (!gdxUELRegisterMapStart(handle.get())) {
			gdxClose(handle.get());
			GDXErrorContext error_context("gdxUELRegisterMapStart");
			error_context.WithFile(file_or_url);
			ThrowGDXError(gdxGetLastError(handle.get()), error_context);
		}

		std::unordered_map<std::string, int> uel_user_map;
		int next_user_idx = 1;
		for (const auto &filter_value : filter_values) {
			if (!gdxUELRegisterMap(handle.get(), next_user_idx, filter_value.c_str())) {
				gdxClose(handle.get());
				GDXErrorContext error_context("gdxUELRegisterMap");
				error_context.WithFile(file_or_url).WithSymbol(filter_value);
				ThrowGDXError(gdxGetLastError(handle.get()), error_context);
			}
			uel_user_map[filter_value] = next_user_idx;
			next_user_idx++;
		}

		if (!gdxUELRegisterDone(handle.get())) {
			gdxClose(handle.get());
			GDXErrorContext error_context("gdxUELRegisterDone");
			error_context.WithFile(file_or_url);
			ThrowGDXError(gdxGetLastError(handle.get()), error_context);
		}

		// Set up filter_actions array - default to DOMC_EXPAND for all dimensions
		std::vector<int> filter_actions(dimension_count, ::gdx::DOMC_EXPAND);
		int next_filter_nr = 1000;

		// Register a filter for each dimension that has a filter value
		for (size_t i = 0; i < filter_indices.size(); i++) {
			int filter_nr = next_filter_nr++;
			if (!gdxFilterRegisterStart(handle.get(), filter_nr)) {
				gdxClose(handle.get());
				GDXErrorContext error_context("gdxFilterRegisterStart");
				error_context.WithFile(file_or_url);
				ThrowGDXError(gdxGetLastError(handle.get()), error_context);
			}

			int user_idx = uel_user_map[filter_values[i]];
			if (!gdxFilterRegister(handle.get(), user_idx)) {
				gdxClose(handle.get());
				GDXErrorContext error_context("gdxFilterRegister");
				error_context.WithFile(file_or_url).WithSymbol(filter_values[i]);
				ThrowGDXError(gdxGetLastError(handle.get()), error_context);
			}

			if (!gdxFilterRegisterDone(handle.get())) {
				gdxClose(handle.get());
				GDXErrorContext error_context("gdxFilterRegisterDone");
				error_context.WithFile(file_or_url);
				ThrowGDXError(gdxGetLastError(handle.get()), error_context);
			}

			if (filter_indices[i] < dimension_count) {
				filter_actions[filter_indices[i]] = filter_nr;
			}
		}

		// Start filtered read
		int nr_records = 0;
		if (!gdxDataReadFilteredStart(handle.get(), sym_nr, filter_actions.data(), &nr_records)) {
			gdxClose(handle.get());
			GDXErrorContext error_context("gdxDataReadFilteredStart");
			error_context.WithFile(file_or_url).WithSymbol(symbol);
			ThrowGDXError(gdxGetLastError(handle.get()), error_context);
		}

		// Read filtered records using gdxDataReadMap (indices are user-mapped)
		// We'll collect them first, then translate using gdxUMUelGet
		std::vector<std::unordered_set<int>> mapped_indices_per_dim(dimension_count);
		while (gdxDataReadMap(handle.get(), 0, key_buffer.data(), value_buffer.data(), &afdim)) {
			for (idx_t d = 0; d < dimension_count; ++d) {
				mapped_indices_per_dim[d].insert(key_buffer[d]);
			}
		}
		// Translate mapped indices to strings directly using gdxUMUelGet
		std::array<char, GMS_SSSIZE> uel_buffer {};
		int uel_map_out = 0;
		for (idx_t d = 0; d < dimension_count; ++d) {
			for (int mapped_idx : mapped_indices_per_dim[d]) {
				if (gdxUMUelGet(handle.get(), mapped_idx, uel_buffer.data(), &uel_map_out)) {
					unique_indices_per_dim[d].insert(mapped_idx);
				}
			}
		}

		// For filtered reads, collect strings separately since indices are user-mapped
		gdxDataReadDone(handle.get());

		// Convert mapped indices to sorted string vectors using gdxGetUEL
		std::vector<std::vector<std::string>> all_dim_values(dimension_count);
		std::array<char, GMS_SSSIZE> uel_buffer_filtered {};
		for (idx_t d = 0; d < dimension_count; ++d) {
			auto &indices = mapped_indices_per_dim[d];
			auto &values = all_dim_values[d];
			values.reserve(indices.size());

			for (int idx : indices) {
				// Use gdxGetUEL for user-mapped indices from gdxDataReadMap
				if (gdxGetUEL(handle.get(), idx, uel_buffer_filtered.data())) {
					values.push_back(std::string(uel_buffer_filtered.data()));
				}
			}
			std::sort(values.begin(), values.end());
		}

		gdxClose(handle.get());

		// Store directly in metadata for immediate use (don't cache since filter-specific)
		for (auto &sym : metadata_entry.symbols) {
			if (StringUtil::CIEquals(sym.name, symbol)) {
				sym.cached_domain_values = std::move(all_dim_values);
				break;
			}
		}
		return;
	} else {
		// Unfiltered read path
		int nr_records = 0;
		if (!gdxDataReadRawStart(handle.get(), sym_nr, &nr_records)) {
			gdxClose(handle.get());
			GDXErrorContext error_context("gdxDataReadRawStart");
			error_context.WithFile(file_or_url).WithSymbol(symbol);
			ThrowGDXError(gdxGetLastError(handle.get()), error_context);
		}

		while (gdxDataReadRaw(handle.get(), key_buffer.data(), value_buffer.data(), &afdim)) {
			for (idx_t d = 0; d < dimension_count; ++d) {
				unique_indices_per_dim[d].insert(key_buffer[d]);
			}
		}
	}

	gdxDataReadDone(handle.get());
	gdxClose(handle.get());

	// Convert to sorted string vectors (for unfiltered reads using raw indices)
	std::vector<std::vector<std::string>> all_dim_values(dimension_count);
	for (idx_t d = 0; d < dimension_count; ++d) {
		auto &indices = unique_indices_per_dim[d];
		auto &values = all_dim_values[d];
		values.reserve(indices.size());

		for (int idx : indices) {
			if (idx > 0 && static_cast<size_t>(idx) < uel_table.size()) {
				values.push_back(uel_table[idx]);
			}
		}
		std::sort(values.begin(), values.end());
	}

	// When filters are used, DON'T cache - results are filter-specific
	// Store directly in metadata for immediate use
	if (use_filtered_read) {
		for (auto &sym : metadata_entry.symbols) {
			if (StringUtil::CIEquals(sym.name, symbol)) {
				sym.cached_domain_values = std::move(all_dim_values);
				break;
			}
		}
	} else {
		// Store in cache for unfiltered results
		metadata_entry.domain_values_cache.SetCachedValues(symbol, all_dim_values);

		// Also persist on metadata entry so sidecar can pick it up
		for (auto &sym : metadata_entry.symbols) {
			if (StringUtil::CIEquals(sym.name, symbol)) {
				sym.cached_domain_values = std::move(all_dim_values);
				break;
			}
		}
	}
}

unique_ptr<FunctionData> DomainValuesBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<DomainValuesBindData>();

	// Required: file path
	if (input.inputs.empty()) {
		throw InvalidInputException("gdx_domain_values requires a file_or_url argument");
	}
	bind_data->file_or_url = input.inputs[0].GetValue<std::string>();

	// Required: symbol name
	if (input.inputs.size() < 2) {
		throw InvalidInputException("gdx_domain_values requires a symbol_name argument");
	}
	bind_data->symbol = input.inputs[1].GetValue<std::string>();

	// Required: dimension (either by name or 1-based index)
	if (input.inputs.size() < 3) {
		throw InvalidInputException("gdx_domain_values requires a dimension argument (name or 1-based index)");
	}

	// Load metadata to resolve dimension
	GDXFileRandomAccessProvider provider;
	provider.Initialize(context, bind_data->file_or_url);
	bind_data->resolved_path = provider.ResolvedPath().empty() ? provider.Location() : provider.ResolvedPath();

	// Get or load metadata (this is cached)
	bind_data->metadata = GDXMetadataCache::Get().GetOrLoad(context, provider);

	// Find symbol
	const GDXSymbolMetadata *symbol_meta = nullptr;
	for (const auto &sym : bind_data->metadata->symbols) {
		if (StringUtil::CIEquals(sym.name, bind_data->symbol)) {
			symbol_meta = &sym;
			break;
		}
	}

	if (!symbol_meta) {
		throw InvalidInputException("Symbol '%s' not found in GDX file '%s'", bind_data->symbol,
		                            bind_data->file_or_url);
	}

	bind_data->dimension_count = symbol_meta->dimension_count;

	// Resolve dimension argument
	auto &dim_arg = input.inputs[2];
	if (dim_arg.type().id() == LogicalTypeId::VARCHAR) {
		// Dimension by name
		std::string dim_name = dim_arg.GetValue<std::string>();
		bind_data->dimension_name = dim_name;

		bool found = false;
		for (idx_t i = 0; i < symbol_meta->domain_labels.size(); ++i) {
			if (StringUtil::CIEquals(symbol_meta->domain_labels[i], dim_name)) {
				bind_data->dimension_index = i;
				found = true;
				break;
			}
		}

		if (!found) {
			std::ostringstream oss;
			for (size_t i = 0; i < symbol_meta->domain_labels.size(); ++i) {
				if (i > 0)
					oss << ", ";
				oss << symbol_meta->domain_labels[i];
			}
			throw InvalidInputException("Dimension '%s' not found in symbol '%s'. Available dimensions: %s", dim_name,
			                            bind_data->symbol, oss.str());
		}
	} else {
		// Dimension by 1-based index
		int64_t dim_idx = dim_arg.GetValue<int64_t>();
		if (dim_idx < 1 || static_cast<uint64_t>(dim_idx) > symbol_meta->dimension_count) {
			throw InvalidInputException("Dimension index %lld out of range for symbol '%s' (has %llu dimensions)",
			                            dim_idx, bind_data->symbol, symbol_meta->dimension_count);
		}
		bind_data->dimension_index = static_cast<idx_t>(dim_idx - 1); // Convert to 0-based
		bind_data->dimension_name = symbol_meta->domain_labels[bind_data->dimension_index];
	}

	// Optional: dimension_filters parameter for cascading filter support
	auto filter_it = input.named_parameters.find("dimension_filters");
	if (filter_it != input.named_parameters.end()) {
		auto &filter_map = filter_it->second;
		if (filter_map.type().id() == LogicalTypeId::MAP) {
			auto map_children = MapValue::GetChildren(filter_map);
			for (auto &entry : map_children) {
				auto kv = StructValue::GetChildren(entry);
				if (kv.size() == 2) {
					std::string dim_name = kv[0].ToString();
					std::string filter_value = kv[1].ToString();

					// Resolve dimension name to index
					for (idx_t i = 0; i < symbol_meta->domain_labels.size(); ++i) {
						if (StringUtil::CIEquals(symbol_meta->domain_labels[i], dim_name)) {
							bind_data->dimension_filter_indices.push_back(i);
							bind_data->dimension_filter_values.push_back(filter_value);
							break;
						}
					}
				}
			}
			bind_data->has_dimension_filters = !bind_data->dimension_filter_indices.empty();
		}
	}

	// Output schema: single column with the dimension values
	names.push_back("value");
	return_types.push_back(LogicalType::VARCHAR);

	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> DomainValuesInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DomainValuesBindData>();
	auto state = make_uniq<DomainValuesGlobalState>();

	// When dimension filters are present, always scan (results are filter-specific, not cached)
	if (bind.has_dimension_filters) {
		ScanAndCacheDomainValues(context, bind.file_or_url, bind.symbol, bind.dimension_count, *bind.metadata,
		                         bind.dimension_filter_indices, bind.dimension_filter_values);
		// Get values from metadata (not cached, just stored temporarily)
		for (auto &sym : bind.metadata->symbols) {
			if (StringUtil::CIEquals(sym.name, bind.symbol) && !sym.cached_domain_values.empty()) {
				if (bind.dimension_index < sym.cached_domain_values.size()) {
					state->cached_values = &sym.cached_domain_values[bind.dimension_index];
				}
				break;
			}
		}
		return std::move(state);
	}

	// Check if we have cached values for this symbol (memory or persisted)
	auto cached = bind.metadata->domain_values_cache.GetCachedValues(bind.symbol);

	if (!cached) {
		// If metadata carries cached domain values (from sidecar), hydrate cache
		for (auto &sym : bind.metadata->symbols) {
			if (StringUtil::CIEquals(sym.name, bind.symbol) && !sym.cached_domain_values.empty()) {
				bind.metadata->domain_values_cache.SetCachedValues(bind.symbol, sym.cached_domain_values);
				break;
			}
		}
		cached = bind.metadata->domain_values_cache.GetCachedValues(bind.symbol);
	}

	if (!cached) {
		// Not cached anywhere - scan and cache ALL dimensions for this symbol
		ScanAndCacheDomainValues(context, bind.file_or_url, bind.symbol, bind.dimension_count, *bind.metadata);
		cached = bind.metadata->domain_values_cache.GetCachedValues(bind.symbol);
	}

	if (cached && bind.dimension_index < cached->size()) {
		state->cached_values = &(*cached)[bind.dimension_index];
	}

	return std::move(state);
}

unique_ptr<LocalTableFunctionState> DomainValuesInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                          GlobalTableFunctionState *) {
	return make_uniq<DomainValuesLocalState>();
}

void DomainValuesFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<DomainValuesGlobalState>();
	auto &local = input.local_state->Cast<DomainValuesLocalState>();

	if (!state.cached_values || local.current_index >= state.cached_values->size()) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.cached_values->size() - local.current_index);

	auto &value_vector = output.data[0];

	for (idx_t i = 0; i < count; ++i) {
		auto &val = (*state.cached_values)[local.current_index + i];
		value_vector.SetValue(i, Value(val));
	}

	local.current_index += count;
	output.SetCardinality(count);
}

} // namespace

void RegisterGDXDomainValuesFunction(ExtensionLoader &loader) {
	// gdx_domain_values(file, symbol, dimension) -> returns all unique values for that dimension
	// First call scans and caches ALL dimensions, subsequent calls are instant
	// Optional: dimension_filters parameter for cascading filter support

	fprintf(stderr, "[GDX] Registering gdx_domain_values function\n");

	// Use same registration pattern as gdx_symbols which works in WASM
	auto function = TableFunction(
	    "gdx_domain_values", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT}, DomainValuesFunction);
	function.bind = DomainValuesBind;
	function.init_global = DomainValuesInitGlobal;
	function.init_local = DomainValuesInitLocal;
	function.projection_pushdown = false;

	// Register named parameters
	function.named_parameters["dimension_filters"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

	loader.RegisterFunction(function);
	fprintf(stderr, "[GDX] Registered gdx_domain_values function done\n");
}

} // namespace gdx
} // namespace duckdb
