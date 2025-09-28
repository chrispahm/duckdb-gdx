#include "gdx/gdx_read_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/value.hpp"

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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdx {

namespace {

struct ReadGDXBindData : public TableFunctionData {
	std::string file_or_url;
	std::string resolved_path;
	std::string symbol;
	std::string requested_symbol;
	bool is_remote {false};
	int symbol_type {0};
	idx_t dimension {0};
	idx_t record_count {0};
	idx_t domain_column_count {0};
	idx_t metadata_column_offset {0};
	idx_t metadata_column_count {0};
	idx_t value_column_offset {0};
	idx_t value_column_count {0};
	std::vector<std::string> domain_labels;
	std::vector<ValueColumnDefinition> value_columns;
	std::unordered_map<std::string, std::string> dimension_filters;
	std::vector<std::string> requested_value_columns;
	std::vector<idx_t> dimension_filter_indices;
	std::vector<std::string> dimension_filter_values;
	bool has_dimension_filters {false};
	bool has_value_column_filter {false};
};

struct ReadGDXGlobalState : public GlobalTableFunctionState {
	GDXFileRandomAccessProvider provider;
	UniqueGDXHandle handle;
	int symbol_index {0};
	int symbol_type {0};
	idx_t dimension {0};
	idx_t record_count {0};
	idx_t rows_read {0};
	bool data_read_started {false};
	bool data_read_finished {false};
	bool handle_closed {false};
	bool data_exhausted {false};
	std::string resolved_path;
	std::string symbol;
	std::unordered_map<std::string, std::string> dimension_filters;
	std::vector<std::string> requested_value_columns;
	std::vector<idx_t> dimension_filter_indices;
	std::vector<std::string> dimension_filter_values;
	bool has_dimension_filters {false};
	bool has_value_column_filter {false};

	idx_t MaxThreads() const override {
		return 1;
	}

	void FinishReading(bool throw_on_error) {
		if (!handle || !data_read_started || data_read_finished) {
			data_read_finished = true;
			return;
		}
		int rc = gdxDataReadDone(handle.get());
		if (rc == 0 && throw_on_error) {
			int error_code = gdxGetLastError(handle.get());
			GDXErrorContext context("gdxDataReadDone");
			context.WithFile(resolved_path).WithSymbol(symbol);
			ThrowGDXError(error_code, context);
		}
		data_read_finished = true;
	}

	void CloseHandle(bool throw_on_error) {
		if (!handle || handle_closed) {
			return;
		}
		int rc = gdxClose(handle.get());
		if (rc != 0 && throw_on_error) {
			GDXErrorContext context("gdxClose");
			context.WithFile(resolved_path);
			ThrowGDXError(rc, context);
		}
		handle.reset();
		handle_closed = true;
	}

	~ReadGDXGlobalState() override {
		try {
			FinishReading(false);
			CloseHandle(false);
		} catch (...) {
		}
	}
};

struct ReadGDXLocalState : public LocalTableFunctionState {
	std::array<std::array<char, GMS_SSSIZE>, GMS_MAX_INDEX_DIM> key_buffer {};
	std::array<char *, GMS_MAX_INDEX_DIM> key_ptrs {};
	std::array<double, GMS_VAL_MAX> value_buffer {};

	ReadGDXLocalState() {
		for (idx_t i = 0; i < GMS_MAX_INDEX_DIM; ++i) {
			key_ptrs[i] = key_buffer[i].data();
		}
	}
};

std::unordered_map<std::string, std::string> ParseDimensionFilters(const Value &parameter) {
	std::unordered_map<std::string, std::string> filters;
	if (parameter.IsNull()) {
		return filters;
	}
	if (parameter.type().id() != LogicalTypeId::MAP) {
		throw InvalidInputException("dimension_filters must be provided as a MAP<VARCHAR, VARCHAR>");
	}
	const auto &children = MapValue::GetChildren(parameter);
	for (const auto &entry : children) {
		const auto &key_value = StructValue::GetChildren(entry);
		if (key_value.size() != 2) {
			throw InvalidInputException("Invalid entry in dimension_filters map; expected key/value struct");
		}
		const auto &key = key_value[0];
		const auto &value = key_value[1];
		if (key.IsNull() || value.IsNull()) {
			throw InvalidInputException("dimension_filters may not contain NULL keys or values");
		}
		if (key.type().id() != LogicalTypeId::VARCHAR || value.type().id() != LogicalTypeId::VARCHAR) {
			throw InvalidInputException("dimension_filters keys and values must be VARCHAR");
		}
		filters[key.ToString()] = value.ToString();
	}
	return filters;
}

std::vector<std::string> ParseValueColumnList(const Value &parameter) {
	std::vector<std::string> columns;
	if (parameter.IsNull()) {
		return columns;
	}
	if (parameter.type().id() != LogicalTypeId::LIST) {
		throw InvalidInputException("value_columns must be provided as a LIST<VARCHAR>");
	}
	const auto &children = ListValue::GetChildren(parameter);
	columns.reserve(children.size());
	for (const auto &entry : children) {
		if (entry.IsNull()) {
			throw InvalidInputException("value_columns may not contain NULL entries");
		}
		if (entry.type().id() != LogicalTypeId::VARCHAR) {
			throw InvalidInputException("value_columns entries must be VARCHAR");
		}
		columns.emplace_back(entry.ToString());
	}
	return columns;
}

unique_ptr<FunctionData> ReadGDXBind(ClientContext &context, TableFunctionBindInput &input,
		 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() < 2) {
		throw InvalidInputException("read_gdx requires a file_or_url and symbol argument");
	}

	auto bind_data = make_uniq<ReadGDXBindData>();
	bind_data->file_or_url = input.inputs[0].ToString();
	bind_data->symbol = input.inputs[1].ToString();
	bind_data->requested_symbol = bind_data->symbol;

	auto metadata_entry = GDXMetadataCache::Get().GetOrLoad(context, bind_data->file_or_url);
	bind_data->resolved_path = metadata_entry->resolved_path;
	bind_data->is_remote = metadata_entry->is_remote;

	auto normalized_symbol = StringUtil::Upper(bind_data->symbol);
	const GDXSymbolMetadata *symbol_metadata = nullptr;
	for (const auto &candidate : metadata_entry->symbols) {
		if (StringUtil::Upper(candidate.name) == normalized_symbol) {
			symbol_metadata = &candidate;
			bind_data->symbol = candidate.name;
			break;
		}
	}

	if (!symbol_metadata) {
		throw InvalidInputException("Symbol '%s' not found in '%s'", bind_data->requested_symbol.c_str(),
		                            bind_data->file_or_url.c_str());
	}

	if (symbol_metadata->type_code == GMS_DT_ALIAS) {
		throw InvalidInputException(StringUtil::Format("read_gdx does not support alias symbols: \"%s\"",
		                                             symbol_metadata->name.c_str()));
	}

	bind_data->symbol_type = symbol_metadata->type_code;
	bind_data->dimension = static_cast<idx_t>(symbol_metadata->dimension_count);
	bind_data->record_count = static_cast<idx_t>(symbol_metadata->record_count);
	bind_data->domain_labels = symbol_metadata->domain_labels;
	bind_data->value_columns = GetValueColumnDefinitions(bind_data->symbol_type);

	if (!input.named_parameters.empty()) {
		auto dimension_filters_param = input.named_parameters.find("dimension_filters");
		if (dimension_filters_param != input.named_parameters.end()) {
			bind_data->dimension_filters = ParseDimensionFilters(dimension_filters_param->second);
			bind_data->has_dimension_filters = !bind_data->dimension_filters.empty();
		}
		auto value_columns_param = input.named_parameters.find("value_columns");
		if (value_columns_param != input.named_parameters.end()) {
			bind_data->requested_value_columns = ParseValueColumnList(value_columns_param->second);
			bind_data->has_value_column_filter = !bind_data->requested_value_columns.empty();
		}
	}

	if (bind_data->has_value_column_filter) {
		std::vector<ValueColumnDefinition> filtered_columns;
		filtered_columns.reserve(bind_data->requested_value_columns.size());
		std::unordered_set<std::string> seen;
		for (auto &requested : bind_data->requested_value_columns) {
			auto match = std::find_if(bind_data->value_columns.begin(), bind_data->value_columns.end(), [&](const ValueColumnDefinition &def) {
				return def.name == requested;
			});
			if (match == bind_data->value_columns.end()) {
				throw InvalidInputException("Unknown value column '%s' for symbol '%s'", requested.c_str(), bind_data->symbol.c_str());
			}
			if (!seen.insert(requested).second) {
				throw InvalidInputException("Duplicate value column '%s' in value_columns parameter", requested.c_str());
			}
			filtered_columns.push_back(*match);
		}
		bind_data->value_columns = std::move(filtered_columns);
	}

	BuildReadGDXSchema(bind_data->domain_labels, bind_data->symbol_type, bind_data->value_columns, return_types, names);
	bind_data->domain_column_count = bind_data->domain_labels.size();
	bind_data->metadata_column_count = bind_data->dimension > 1 ? 2 : 0;
	bind_data->metadata_column_offset = bind_data->domain_column_count;
	bind_data->value_column_offset = bind_data->metadata_column_offset + bind_data->metadata_column_count;
	bind_data->value_column_count = bind_data->value_columns.size();
	if (bind_data->value_column_count == 0) {
		throw InvalidInputException("value_columns must select at least one column");
	}

	if (bind_data->has_dimension_filters) {
		auto normalize_key = [](const string &input) {
			auto copy = StringUtil::Lower(input);
			StringUtil::Trim(copy);
			return copy;
		};

		std::unordered_map<string, idx_t> dimension_name_map;
		dimension_name_map.reserve(bind_data->domain_column_count * 2 + 1);
		for (idx_t i = 0; i < bind_data->domain_column_count && i < names.size(); ++i) {
			auto normalized_name = normalize_key(names[i]);
			dimension_name_map[normalized_name] = i;
			auto label = bind_data->domain_labels[i];
			if (!label.empty() && label != "*") {
				auto normalized_label = normalize_key(label);
				dimension_name_map[normalized_label] = i;
			} else {
				auto generated = StringUtil::Format("dim_%d", static_cast<int>(i + 1));
				dimension_name_map[normalize_key(generated)] = i;
				if (bind_data->domain_column_count == 1 && bind_data->dimension == 1) {
					dimension_name_map[normalize_key(bind_data->symbol)] = i;
				}
			}
		}

		std::unordered_set<idx_t> used_indices;
		string valid_dimensions_str;
		for (idx_t i = 0; i < bind_data->domain_column_count && i < names.size(); ++i) {
			if (!valid_dimensions_str.empty()) {
				valid_dimensions_str += ", ";
			}
			valid_dimensions_str += names[i];
		}

		for (const auto &entry : bind_data->dimension_filters) {
			auto key = normalize_key(entry.first);
			auto value = entry.second;
			StringUtil::Trim(value);
			auto it = dimension_name_map.find(key);
			if (it == dimension_name_map.end()) {
				throw InvalidInputException("Unknown dimension '%s' for symbol '%s'. Valid dimensions: %s", entry.first.c_str(),
				                             bind_data->symbol.c_str(), valid_dimensions_str.c_str());
			}
			idx_t dim_index = it->second;
			if (!used_indices.insert(dim_index).second) {
				throw InvalidInputException("Duplicate dimension filter supplied for '%s'", entry.first.c_str());
			}
			bind_data->dimension_filter_indices.push_back(dim_index);
			bind_data->dimension_filter_values.push_back(value);
		}
	}

	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> ReadGDXInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadGDXBindData>();
	auto state = make_uniq<ReadGDXGlobalState>();
	state->provider.Initialize(context, bind.file_or_url);
	state->resolved_path = state->provider.ResolvedPath();
	state->symbol = bind.symbol;
	state->dimension = bind.dimension;
	state->record_count = bind.record_count;
	state->dimension_filters = bind.dimension_filters;
	state->has_dimension_filters = bind.has_dimension_filters;
	state->requested_value_columns = bind.requested_value_columns;
	state->dimension_filter_indices = bind.dimension_filter_indices;
	state->dimension_filter_values = bind.dimension_filter_values;
	state->has_value_column_filter = bind.has_value_column_filter;

	state->handle = CreateGDXHandle();
	int open_error = 0;
	if (!gdxOpenReadFromRandomAccess(state->handle.get(), &state->provider.GetCallbacks(), &open_error)) {
		GDXErrorContext error_context("gdxOpenReadFromRandomAccess");
		error_context.WithFile(state->resolved_path);
		ThrowGDXError(open_error, error_context);
	}

	if (!gdxFindSymbol(state->handle.get(), bind.symbol.c_str(), &state->symbol_index)) {
		GDXErrorContext error_context("gdxFindSymbol");
		error_context.WithFile(state->resolved_path).WithSymbol(bind.symbol);
		ThrowGDXError(gdxGetLastError(state->handle.get()), error_context);
	}

	int dimension = 0;
	int symbol_type = 0;
	std::array<char, GMS_SSSIZE> symbol_name {};
	if (!gdxSymbolInfo(state->handle.get(), state->symbol_index, symbol_name.data(), &dimension, &symbol_type)) {
		GDXErrorContext error_context("gdxSymbolInfo");
		error_context.WithFile(state->resolved_path).WithSymbol(bind.symbol);
		ThrowGDXError(gdxGetLastError(state->handle.get()), error_context);
	}
	state->symbol_type = symbol_type;

	int nr_records = 0;
	if (!gdxDataReadStrStart(state->handle.get(), state->symbol_index, &nr_records)) {
		GDXErrorContext error_context("gdxDataReadStrStart");
		error_context.WithFile(state->resolved_path).WithSymbol(bind.symbol);
		ThrowGDXError(gdxGetLastError(state->handle.get()), error_context);
	}
	state->data_read_started = true;
	if (bind.record_count == 0) {
		state->record_count = nr_records < 0 ? 0 : static_cast<idx_t>(nr_records);
	}

	return std::move(state);
}

unique_ptr<LocalTableFunctionState> ReadGDXInitLocal(ExecutionContext &, TableFunctionInitInput &, GlobalTableFunctionState *) {
	return make_uniq<ReadGDXLocalState>();
}

bool IsSpecialValue(ReadGDXGlobalState &state, double value, int &sv_index) {
	if (!state.handle) {
		sv_index = GMS_SVIDX_NORMAL;
		return false;
	}
	int rc = gdxMapValue(state.handle.get(), value, &sv_index);
	if (rc == 0) {
		sv_index = GMS_SVIDX_NORMAL;
		return false;
	}
	return sv_index != GMS_SVIDX_NORMAL;
}

void SetDoubleValue(Vector &vector, idx_t index, ReadGDXGlobalState &state, double value) {
	int special = GMS_SVIDX_NORMAL;
	if (IsSpecialValue(state, value, special)) {
		FlatVector::SetNull(vector, index, true);
		return;
	}
	FlatVector::SetNull(vector, index, false);
	FlatVector::GetData<double>(vector)[index] = value;
}

void SetBooleanValue(Vector &vector, idx_t index, ReadGDXGlobalState &state, double value, bool presence_implies_true) {
	int special = GMS_SVIDX_NORMAL;
	if (IsSpecialValue(state, value, special)) {
		FlatVector::SetNull(vector, index, true);
		return;
	}
	FlatVector::SetNull(vector, index, false);
	if (presence_implies_true) {
		FlatVector::GetData<bool>(vector)[index] = true;
	} else {
		FlatVector::GetData<bool>(vector)[index] = value != 0.0;
	}
}

void ReadGDXFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<ReadGDXBindData>();
	auto &state = input.global_state->Cast<ReadGDXGlobalState>();
	auto &local = input.local_state->Cast<ReadGDXLocalState>();

	if (state.data_exhausted) {
		output.SetCardinality(0);
		return;
	}

	idx_t rows_remaining = state.record_count > state.rows_read ? state.record_count - state.rows_read : 0;
	if (rows_remaining == 0) {
		state.data_exhausted = true;
		state.FinishReading(true);
		state.CloseHandle(true);
		output.SetCardinality(0);
		return;
	}

	idx_t target_count = std::min<idx_t>(STANDARD_VECTOR_SIZE, rows_remaining);
	idx_t produced = 0;

	std::vector<string_t *> domain_data;
	domain_data.reserve(bind.domain_column_count);
	for (idx_t col = 0; col < bind.domain_column_count; ++col) {
		domain_data.push_back(FlatVector::GetData<string_t>(output.data[col]));
	}

	Vector *sparse_break_vector = nullptr;
	Vector *dense_run_vector = nullptr;
	if (bind.metadata_column_count > 0) {
		sparse_break_vector = &output.data[bind.metadata_column_offset];
		dense_run_vector = &output.data[bind.metadata_column_offset + 1];
	}

	std::vector<Vector *> value_vectors;
	value_vectors.reserve(bind.value_column_count);
	for (idx_t i = 0; i < bind.value_column_count; ++i) {
		value_vectors.push_back(&output.data[bind.value_column_offset + i]);
	}

	while (produced < target_count) {
		int first_dim = 0;
		int read_success = gdxDataReadStr(state.handle.get(), local.key_ptrs.data(), local.value_buffer.data(), &first_dim);
		if (read_success == 0) {
			state.data_exhausted = true;
			break;
		}

		if (bind.has_dimension_filters) {
			bool matches_filters = true;
			for (idx_t filter_idx = 0; filter_idx < bind.dimension_filter_indices.size(); ++filter_idx) {
				auto dim_index = bind.dimension_filter_indices[filter_idx];
				if (dim_index >= bind.domain_column_count) {
					continue;
				}
				string actual_value(local.key_buffer[dim_index].data());
				StringUtil::Trim(actual_value);
				if (!StringUtil::CIEquals(actual_value, bind.dimension_filter_values[filter_idx])) {
					matches_filters = false;
					break;
				}
			}
			if (!matches_filters) {
				continue;
			}
		}

		for (idx_t col = 0; col < bind.domain_column_count; ++col) {
			FlatVector::SetNull(output.data[col], produced, false);
			domain_data[col][produced] = StringVector::AddString(output.data[col], local.key_buffer[col].data());
		}

		if (sparse_break_vector && dense_run_vector) {
			if (first_dim <= 0) {
				FlatVector::SetNull(*sparse_break_vector, produced, true);
				FlatVector::SetNull(*dense_run_vector, produced, true);
			} else {
				bool sparse_break = first_dim == 1;
				bool dense_run = first_dim > 1;
				FlatVector::SetNull(*sparse_break_vector, produced, false);
				FlatVector::GetData<bool>(*sparse_break_vector)[produced] = sparse_break;
				FlatVector::SetNull(*dense_run_vector, produced, false);
				FlatVector::GetData<bool>(*dense_run_vector)[produced] = dense_run;
			}
		}

		for (idx_t value_idx = 0; value_idx < bind.value_column_count; ++value_idx) {
			auto &definition = bind.value_columns[value_idx];
			switch (definition.kind) {
			case ValueColumnKind::SetMembership:
				SetBooleanValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_LEVEL], true);
				break;
			case ValueColumnKind::Level:
				SetDoubleValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_LEVEL]);
				break;
			case ValueColumnKind::Marginal:
				SetDoubleValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_MARGINAL]);
				break;
			case ValueColumnKind::Lower:
				SetDoubleValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_LOWER]);
				break;
			case ValueColumnKind::Upper:
				SetDoubleValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_UPPER]);
				break;
			case ValueColumnKind::Scale:
				SetDoubleValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_SCALE]);
				break;
			case ValueColumnKind::RawValue:
				SetDoubleValue(*value_vectors[value_idx], produced, state, local.value_buffer[GMS_VAL_LEVEL]);
				break;
			}
		}

		produced++;
	}

	if (produced == 0) {
		state.data_exhausted = true;
		state.FinishReading(true);
		state.CloseHandle(true);
		output.SetCardinality(0);
		return;
	}

	state.rows_read += produced;
	output.SetCardinality(produced);

	if (state.data_exhausted || state.rows_read >= state.record_count) {
		state.FinishReading(true);
		state.CloseHandle(true);
	}
}

} // namespace

void RegisterReadTableFunction(ExtensionLoader &loader) {
	auto function = TableFunction("read_gdx", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ReadGDXFunction);
	function.bind = ReadGDXBind;
	function.init_global = ReadGDXInitGlobal;
	function.init_local = ReadGDXInitLocal;
 	function.named_parameters["dimension_filters"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
 	function.named_parameters["value_columns"] = LogicalType::LIST(LogicalType::VARCHAR);

	loader.RegisterFunction(function);
}

} // namespace gdx
} // namespace duckdb
