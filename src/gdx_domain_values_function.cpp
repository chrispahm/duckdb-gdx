#include "gdx/gdx_domain_values_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_util.hpp"
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

// Scan ALL dimensions for a symbol and cache them
// This is O(n) where n = records, but only needs to run once per symbol
void ScanAndCacheDomainValues(ClientContext &context, const std::string &file_or_url,
							   const std::string &symbol, idx_t dimension_count,
							   GDXMetadataEntry &metadata_entry) {
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
	
	// Scan all records, collecting unique UEL indices for ALL dimensions at once
	int nr_records = 0;
	if (!gdxDataReadRawStart(handle.get(), sym_nr, &nr_records)) {
		gdxClose(handle.get());
		GDXErrorContext error_context("gdxDataReadRawStart");
		error_context.WithFile(file_or_url).WithSymbol(symbol);
		ThrowGDXError(gdxGetLastError(handle.get()), error_context);
	}
	
	std::vector<int> key_buffer(dimension_count);
	std::array<double, GMS_VAL_MAX> value_buffer {};
	int afdim = 0;
	
	while (gdxDataReadRaw(handle.get(), key_buffer.data(), value_buffer.data(), &afdim)) {
		for (idx_t d = 0; d < dimension_count; ++d) {
			unique_indices_per_dim[d].insert(key_buffer[d]);
		}
	}
	
	gdxDataReadDone(handle.get());
	gdxClose(handle.get());
	
	// Convert to sorted string vectors
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
	
	// Store in cache
	metadata_entry.domain_values_cache.SetCachedValues(symbol, all_dim_values);

	// Also persist on metadata entry so sidecar can pick it up
	for (auto &sym : metadata_entry.symbols) {
		if (StringUtil::CIEquals(sym.name, symbol)) {
			sym.cached_domain_values = std::move(all_dim_values);
			break;
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
		throw InvalidInputException("Symbol '%s' not found in GDX file '%s'", 
		                           bind_data->symbol, bind_data->file_or_url);
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
				if (i > 0) oss << ", ";
				oss << symbol_meta->domain_labels[i];
			}
			throw InvalidInputException(
			    "Dimension '%s' not found in symbol '%s'. Available dimensions: %s",
			    dim_name, bind_data->symbol, oss.str());
		}
	} else {
		// Dimension by 1-based index
		int64_t dim_idx = dim_arg.GetValue<int64_t>();
		if (dim_idx < 1 || static_cast<uint64_t>(dim_idx) > symbol_meta->dimension_count) {
			throw InvalidInputException(
			    "Dimension index %lld out of range for symbol '%s' (has %llu dimensions)",
			    dim_idx, bind_data->symbol, symbol_meta->dimension_count);
		}
		bind_data->dimension_index = static_cast<idx_t>(dim_idx - 1); // Convert to 0-based
		bind_data->dimension_name = symbol_meta->domain_labels[bind_data->dimension_index];
	}
	
	// Output schema: single column with the dimension values
	names.push_back("value");
	return_types.push_back(LogicalType::VARCHAR);
	
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> DomainValuesInitGlobal(ClientContext &context, 
                                                             TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DomainValuesBindData>();
	auto state = make_uniq<DomainValuesGlobalState>();
	
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
		ScanAndCacheDomainValues(context, bind.file_or_url, bind.symbol, 
		                         bind.dimension_count, *bind.metadata);
		cached = bind.metadata->domain_values_cache.GetCachedValues(bind.symbol);
	}
	
	if (cached && bind.dimension_index < cached->size()) {
		state->cached_values = &(*cached)[bind.dimension_index];
	}
	
	return std::move(state);
}

unique_ptr<LocalTableFunctionState> DomainValuesInitLocal(ExecutionContext &, TableFunctionInitInput &, GlobalTableFunctionState *) {
	return make_uniq<DomainValuesLocalState>();
}

void DomainValuesFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<DomainValuesGlobalState>();
	auto &local = input.local_state->Cast<DomainValuesLocalState>();
	
	if (!state.cached_values || local.current_index >= state.cached_values->size()) {
		output.SetCardinality(0);
		return;
	}
	
	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, 
	                              state.cached_values->size() - local.current_index);
	
	auto &value_vector = output.data[0];
	
	for (idx_t i = 0; i < count; ++i) {
		auto &val = (*state.cached_values)[local.current_index + i];
		value_vector.SetValue(i, Value(val));
	}
	
	local.current_index += count;
	output.SetCardinality(count);
}

} // namespace

__attribute__((used))
void RegisterGDXDomainValuesFunction(DatabaseInstance &db) {
	// gdx_domain_values(file, symbol, dimension) -> returns all unique values for that dimension
	// First call scans and caches ALL dimensions, subsequent calls are instant
	
	fprintf(stderr, "[GDX] Registering gdx_domain_values function\n");
	
	// Use same registration pattern as gdx_symbols which works in WASM
	auto function = TableFunction("gdx_domain_values", 
	                              {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT},
	                              DomainValuesFunction);
	function.bind = DomainValuesBind;
	function.init_global = DomainValuesInitGlobal;
	function.init_local = DomainValuesInitLocal;
	function.projection_pushdown = false;
	
	ExtensionUtil::RegisterFunction(db, function);
	fprintf(stderr, "[GDX] Registered gdx_domain_values function done\n");
}

} // namespace gdx
} // namespace duckdb
