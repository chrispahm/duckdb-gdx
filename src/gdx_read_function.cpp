#include "gdx/gdx_read_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include "gdx/gdx_error.hpp"
#include "gdx/gdx_file_provider.hpp"
#define NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_handle.hpp"
#undef NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_metadata_cache.hpp"
#include "gdx/gdx_symbol_utils.hpp"
#include "duckdb/main/extension_util.hpp"

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

struct DimensionFilterEntry {
	std::string value;
	std::string display_name;
	std::string source;
};

using DimensionFilterMap = std::unordered_map<std::string, DimensionFilterEntry>;

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
	DimensionFilterMap dimension_filters;
	std::vector<std::string> requested_value_columns;
	std::vector<idx_t> dimension_filter_indices;
	std::vector<std::string> dimension_filter_values;
	bool has_dimension_filters {false};
	std::vector<std::string> column_names;
	bool dimension_filter_bindings_prepared {false};
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

string NormalizeDimensionKey(const string &input) {
	auto normalized = StringUtil::Lower(input);
	StringUtil::Trim(normalized);
	return normalized;
}

bool TryAddDimensionFilter(ReadGDXBindData &bind, const string &dimension_name, const string &raw_value,
	                         const string &source) {
	if (dimension_name.empty()) {
		return false;
	}
	auto normalized_key = NormalizeDimensionKey(dimension_name);
	if (normalized_key.empty()) {
		return false;
	}
	auto value = raw_value;
	StringUtil::Trim(value);
	if (value.empty()) {
		return false;
	}
	auto existing = bind.dimension_filters.find(normalized_key);
	if (existing != bind.dimension_filters.end()) {
		if (!StringUtil::CIEquals(existing->second.value, value)) {
			throw InvalidInputException(
			    "Conflicting filters for dimension '%s': %s requires '%s' but %s requires '%s'", dimension_name.c_str(),
			    source.c_str(), value.c_str(), existing->second.source.c_str(), existing->second.value.c_str());
		}
		return false;
	}
	DimensionFilterEntry entry;
	entry.value = value;
	entry.display_name = dimension_name;
	entry.source = source;
	bind.dimension_filters.emplace(normalized_key, std::move(entry));
	bind.dimension_filter_bindings_prepared = false;
	return true;
}

void PrepareDimensionFilters(ReadGDXBindData &bind) {
	bind.dimension_filter_indices.clear();
	bind.dimension_filter_values.clear();
	bind.has_dimension_filters = !bind.dimension_filters.empty();
	if (!bind.has_dimension_filters) {
		bind.dimension_filter_bindings_prepared = true;
		return;
	}
	if (bind.column_names.empty()) {
		throw InternalException("read_gdx: column metadata unavailable when preparing dimension filters");
	}

	std::unordered_map<string, idx_t> dimension_name_map;
	dimension_name_map.reserve(bind.domain_column_count * 3 + 2);
	auto build_generated_name = [](idx_t index) {
		return StringUtil::Format("dim_%d", static_cast<int>(index + 1));
	};

	string valid_dimensions_str;
	for (idx_t i = 0; i < bind.domain_column_count && i < bind.column_names.size(); ++i) {
		if (!valid_dimensions_str.empty()) {
			valid_dimensions_str += ", ";
		}
		valid_dimensions_str += bind.column_names[i];
		auto normalized_name = NormalizeDimensionKey(bind.column_names[i]);
		dimension_name_map[normalized_name] = i;
		const auto &label = bind.domain_labels[i];
		if (!label.empty() && label != "*") {
			dimension_name_map[NormalizeDimensionKey(label)] = i;
		} else {
			auto generated = build_generated_name(i);
			dimension_name_map[NormalizeDimensionKey(generated)] = i;
			if (bind.domain_column_count == 1 && bind.dimension == 1) {
				dimension_name_map[NormalizeDimensionKey(bind.symbol)] = i;
			}
		}
	}

	std::unordered_set<idx_t> used_indices;
	for (auto &entry : bind.dimension_filters) {
		auto lookup = dimension_name_map.find(entry.first);
		if (lookup == dimension_name_map.end()) {
			throw InvalidInputException("Unknown dimension '%s' supplied via %s. Valid dimensions: %s",
			                             entry.second.display_name.c_str(), entry.second.source.c_str(),
			                             valid_dimensions_str.c_str());
		}
		auto dim_index = lookup->second;
		if (!used_indices.insert(dim_index).second) {
			throw InvalidInputException("Duplicate dimension filter supplied for '%s'", bind.column_names[dim_index].c_str());
		}
		bind.dimension_filter_indices.push_back(dim_index);
		bind.dimension_filter_values.push_back(entry.second.value);
	}

	bind.dimension_filter_bindings_prepared = true;
}

void EnsureDimensionFiltersPrepared(ReadGDXBindData &bind) {
	if (!bind.dimension_filter_bindings_prepared) {
		PrepareDimensionFilters(bind);
	}
}


DimensionFilterMap ParseDimensionFilters(const Value &parameter) {
	DimensionFilterMap filters;
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
		auto trimmed_key = key.ToString();
		auto trimmed_value = value.ToString();
		StringUtil::Trim(trimmed_key);
		StringUtil::Trim(trimmed_value);
		auto normalized_key = NormalizeDimensionKey(trimmed_key);
		if (normalized_key.empty()) {
			throw InvalidInputException("dimension_filters keys must contain at least one non-whitespace character");
		}
		if (trimmed_value.empty()) {
			throw InvalidInputException("dimension_filters values must contain at least one non-whitespace character");
		}
		auto inserted = filters.emplace(normalized_key,
		                                DimensionFilterEntry {trimmed_value, trimmed_key, "dimension_filters parameter"});
		if (!inserted.second) {
			throw InvalidInputException("Duplicate dimension filter supplied for '%s'", key.ToString().c_str());
		}
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

Expression *StripCasts(Expression &expr) {
	Expression *current = &expr;
	while (current->expression_class == ExpressionClass::BOUND_CAST) {
		current = current->Cast<BoundCastExpression>().child.get();
	}
	return current;
}

bool ExtractColumnBinding(Expression &expr, LogicalGet &get, idx_t &column_index, string &column_name) {
	auto *node = StripCasts(expr);
	if (node->expression_class != ExpressionClass::BOUND_REF) {
		return false;
	}
	auto &ref = node->Cast<BoundReferenceExpression>();
	const auto &column_ids = get.GetColumnIds();
	if (ref.index < 0 || static_cast<idx_t>(ref.index) >= column_ids.size()) {
		return false;
	}
	auto physical_index = column_ids[ref.index].GetPrimaryIndex();
	if (physical_index >= get.names.size()) {
		return false;
	}
	column_index = physical_index;
	column_name = get.names[physical_index];
	return true;
}

bool ExtractConstantString(Expression &expr, string &value) {
	auto *node = StripCasts(expr);
	if (node->expression_class != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &constant = node->Cast<BoundConstantExpression>();
	if (constant.value.IsNull()) {
		return false;
	}
	Value string_value;
	try {
		string_value = constant.value.DefaultCastAs(LogicalType::VARCHAR);
	} catch (...) {
		return false;
	}
	value = string_value.GetValue<string>();
	StringUtil::Trim(value);
	return true;
}

bool ExtractColumnConstantPair(Expression &left, Expression &right, LogicalGet &get, idx_t &column_index,
	                           string &column_name, string &value) {
	if (ExtractColumnBinding(left, get, column_index, column_name) && ExtractConstantString(right, value)) {
		return true;
	}
	if (ExtractColumnBinding(right, get, column_index, column_name) && ExtractConstantString(left, value)) {
		return true;
	}
	return false;
}

bool ExtractComparisonFilter(BoundComparisonExpression &expr, LogicalGet &get, ReadGDXBindData &bind) {
	switch (expr.type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		break;
	default:
		return false;
	}
	idx_t column_index = 0;
	string column_name;
	string constant_value;
	if (!ExtractColumnConstantPair(*expr.left, *expr.right, get, column_index, column_name, constant_value)) {
		return false;
	}
	if (column_index >= bind.domain_column_count || constant_value.empty()) {
		return false;
	}
	return TryAddDimensionFilter(bind, column_name, constant_value, "WHERE clause");
}

bool ExtractFiltersFromExpression(Expression &expr, LogicalGet &get, ReadGDXBindData &bind) {
	switch (expr.type) {
	case ExpressionType::CONJUNCTION_AND: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		bool added = false;
		for (auto &child : conjunction.children) {
			added |= ExtractFiltersFromExpression(*child, get, bind);
		}
		return added;
	}
	default:
		break;
	}
	if (expr.expression_class == ExpressionClass::BOUND_COMPARISON) {
		auto &comparison = expr.Cast<BoundComparisonExpression>();
		return ExtractComparisonFilter(comparison, get, bind);
	}
	return false;
}

void ReadGDXPushdownComplexFilter(ClientContext &, LogicalGet &get, FunctionData *bind_data_p,
	                               vector<unique_ptr<Expression>> &filters) {
	if (!bind_data_p) {
		return;
	}
	auto &bind = bind_data_p->Cast<ReadGDXBindData>();
	if (bind.domain_column_count == 0) {
		return;
	}
	bool added_filters = false;
	for (auto &expr : filters) {
		if (expr) {
			added_filters |= ExtractFiltersFromExpression(*expr, get, bind);
		}
	}
	if (added_filters) {
		PrepareDimensionFilters(bind);
	}
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
	bind_data->column_names = names;
	PrepareDimensionFilters(*bind_data);

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

void RegisterReadTableFunction(DatabaseInstance &db) {
	auto function = TableFunction("read_gdx", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ReadGDXFunction);
	function.bind = ReadGDXBind;
	function.init_global = ReadGDXInitGlobal;
	function.init_local = ReadGDXInitLocal;
	function.pushdown_complex_filter = ReadGDXPushdownComplexFilter;
 	function.named_parameters["dimension_filters"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
 	function.named_parameters["value_columns"] = LogicalType::LIST(LogicalType::VARCHAR);

	ExtensionUtil::RegisterFunction(db, function);
}

} // namespace gdx
} // namespace duckdb
