#include "gdx/gdx_read_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/constants.hpp"
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
#include <atomic>
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
	idx_t offset {0};
	idx_t limit {DConstants::INVALID_INDEX};
	bool has_offset {false};
	bool has_limit {false};
};

// Structure to hold buffered records from filtered read callback
struct FilteredReadRecord {
	std::vector<int> raw_indices;
	std::array<double, GMS_VAL_MAX> values;
};

// Context for callback-based filtered reading using gdxDataReadRawFastFilt
struct FilteredReadContext {
	TGXFileRec_t *handle {nullptr};
	idx_t dimension {0};
	std::vector<FilteredReadRecord> buffered_records;
	idx_t current_record_index {0};
	bool read_complete {false};
	std::string resolved_path;
	std::string symbol;

	// Progress tracking during scan
	std::atomic<idx_t> records_scanned {0};
	idx_t total_records {0}; // Set before scan starts
	idx_t offset {0};
	idx_t limit {DConstants::INVALID_INDEX};
	idx_t skip_remaining {0};
	idx_t emitted {0};
	bool has_limit {false};
	bool stop_requested {false};

	// Build filter strings for gdxDataReadRawFastFilt
	std::vector<std::string> filter_strings;
	std::vector<const char *> filter_ptrs;

	void PrepareFilters(idx_t dim_count, const std::vector<idx_t> &filter_indices,
	                    const std::vector<std::string> &filter_values) {
		filter_strings.resize(dim_count);
		filter_ptrs.resize(dim_count);

		// Initialize all to empty strings (no filter)
		for (idx_t i = 0; i < dim_count; ++i) {
			filter_strings[i] = "";
			filter_ptrs[i] = filter_strings[i].c_str();
		}

		// Set filters for specified dimensions
		for (idx_t i = 0; i < filter_indices.size(); ++i) {
			auto dim_idx = filter_indices[i];
			if (dim_idx < dim_count) {
				filter_strings[dim_idx] = filter_values[i];
				filter_ptrs[i] = filter_strings[dim_idx].c_str();
			}
		}

		// Update pointers after all strings are set (avoids invalidation)
		for (idx_t i = 0; i < dim_count; ++i) {
			filter_ptrs[i] = filter_strings[i].c_str();
		}
	}

	bool HasMoreRecords() const {
		return current_record_index < buffered_records.size();
	}

	FilteredReadRecord *GetNextRecord() {
		if (current_record_index >= buffered_records.size()) {
			return nullptr;
		}
		return &buffered_records[current_record_index++];
	}
};

// Thread-local pointer for callback context (gdxDataReadRawFastFilt doesn't pass Uptr correctly)
static thread_local FilteredReadContext *g_filtered_read_context = nullptr;

// Callback for gdxDataReadRawFastFilt - stores matching records
static int FilteredReadCallback(const int *Indx, const double *Vals, void * /*Uptr*/) {
	auto *ctx = g_filtered_read_context;
	if (!ctx) {
		return 0; // Stop reading if no context
	}

	// Increment progress counter (atomic for thread safety)
	ctx->records_scanned.fetch_add(1, std::memory_order_relaxed);

	if (ctx->skip_remaining > 0) {
		ctx->skip_remaining--;
		return 1; // Skip until offset satisfied
	}

	if (ctx->has_limit && ctx->emitted >= ctx->limit) {
		ctx->stop_requested = true;
		return 0; // Stop once limit reached
	}

	FilteredReadRecord record;
	record.raw_indices.resize(ctx->dimension);
	for (idx_t i = 0; i < ctx->dimension; ++i) {
		record.raw_indices[i] = Indx[i];
	}
	for (idx_t i = 0; i < GMS_VAL_MAX; ++i) {
		record.values[i] = Vals[i];
	}
	ctx->buffered_records.push_back(std::move(record));
	ctx->emitted++;

	if (ctx->has_limit && ctx->emitted >= ctx->limit) {
		ctx->stop_requested = true;
		return 0; // Stop reading after limit
	}
	return 1; // Continue reading
}

// Callback for gdxDataReadRawFastEx - stores all records (unfiltered fast read)
// Uses the Uptr parameter properly
static int GDX_CALLCONV UnfilteredReadCallback(const int *Indx, const double *Vals, int /*DimFrst*/, void *Uptr) {
	auto *ctx = static_cast<FilteredReadContext *>(Uptr);
	if (!ctx) {
		return 0; // Stop reading if no context
	}

	// Increment progress counter (atomic for thread safety)
	ctx->records_scanned.fetch_add(1, std::memory_order_relaxed);

	if (ctx->skip_remaining > 0) {
		ctx->skip_remaining--;
		return 1; // Skip until offset satisfied
	}

	if (ctx->has_limit && ctx->emitted >= ctx->limit) {
		ctx->stop_requested = true;
		return 0; // Stop once limit reached
	}

	FilteredReadRecord record;
	record.raw_indices.resize(ctx->dimension);
	for (idx_t i = 0; i < ctx->dimension; ++i) {
		record.raw_indices[i] = Indx[i];
	}
	for (idx_t i = 0; i < GMS_VAL_MAX; ++i) {
		record.values[i] = Vals[i];
	}
	ctx->buffered_records.push_back(std::move(record));
	ctx->emitted++;

	if (ctx->has_limit && ctx->emitted >= ctx->limit) {
		ctx->stop_requested = true;
		return 0; // Stop reading after limit
	}
	return 1; // Continue reading
}

struct ReadGDXGlobalState : public GlobalTableFunctionState {
	GDXFileRandomAccessProvider provider;
	UniqueGDXHandle handle;
	int symbol_index {0};
	int symbol_type {0};
	idx_t dimension {0};
	idx_t record_count {0};
	std::atomic<idx_t> rows_read {0}; // Atomic for thread-safe progress reporting
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
	idx_t offset {0};
	idx_t limit {DConstants::INVALID_INDEX};
	bool has_offset {false};
	bool has_limit {false};

	// For filtered reading using gdxDataReadRawFastFilt
	bool use_filtered_read {false};
	std::unique_ptr<FilteredReadContext> filtered_context;

	// Preloaded UEL table: vector indexed by UEL number (1-based, index 0 unused)
	// This is populated once at file open time for O(1) lookups
	std::vector<std::string> uel_table;
	bool uel_table_loaded {false};

	// Preload the entire UEL table into memory for fast lookups
	// Note: Raw indices from gdxDataReadRawFastEx callbacks are internal entry numbers,
	// not user UEL numbers. We use gdxUMUelGet which takes entry numbers directly and
	// returns the corresponding UEL string.
	void PreloadUELTable() {
		if (uel_table_loaded || !handle) {
			return;
		}

		int uel_count = 0;
		int high_map = 0;
		gdxUMUelInfo(handle.get(), &uel_count, &high_map);

		// Resize to accommodate 1-based indexing (index 0 is unused)
		uel_table.resize(static_cast<size_t>(uel_count) + 1);

		std::array<char, GMS_SSSIZE> uel_buffer {};
		int uel_map = 0;
		for (int i = 1; i <= uel_count; ++i) {
			// gdxUMUelGet takes an internal entry number (UelNr) and returns the UEL string
			// This is the correct function to use with raw indices from callbacks
			if (gdxUMUelGet(handle.get(), i, uel_buffer.data(), &uel_map)) {
				uel_table[i] = std::string(uel_buffer.data());
			} else {
				// Fallback: numeric representation
				uel_table[i] = std::to_string(i);
			}
		}

		uel_table_loaded = true;
	}

	// Look up a UEL string using the preloaded table (O(1) lookup)
	const std::string &GetUELString(int uel_nr) {
		if (uel_nr > 0 && static_cast<size_t>(uel_nr) < uel_table.size()) {
			return uel_table[uel_nr];
		}
		// Out of range - return empty string (shouldn't happen with valid data)
		static const std::string empty_string;
		return empty_string;
	}

	// Look up set element text (description) by text number
	// Returns the description string, or empty string if not available
	std::string GetSetElementText(int txt_nr) {
		if (!handle || txt_nr <= 0) {
			return "";
		}
		std::array<char, GMS_SSSIZE> txt_buffer {};
		int node = 0;
		if (gdxGetElemText(handle.get(), txt_nr, txt_buffer.data(), &node)) {
			return std::string(txt_buffer.data());
		}
		return "";
	}

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
	// Empty - all reads now use the buffered path via filtered_context
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
			throw InvalidInputException("Conflicting filters for dimension '%s': %s requires '%s' but %s requires '%s'",
			                            dimension_name.c_str(), source.c_str(), value.c_str(),
			                            existing->second.source.c_str(), existing->second.value.c_str());
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
			throw InvalidInputException("Duplicate dimension filter supplied for '%s'",
			                            bind.column_names[dim_index].c_str());
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
		auto inserted = filters.emplace(
		    normalized_key, DimensionFilterEntry {trimmed_value, trimmed_key, "dimension_filters parameter"});
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
		throw InvalidInputException(
		    StringUtil::Format("read_gdx does not support alias symbols: \"%s\"", symbol_metadata->name.c_str()));
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
		auto offset_param = input.named_parameters.find("row_offset");
		if (offset_param != input.named_parameters.end()) {
			auto offset_value = offset_param->second.GetValue<int64_t>();
			if (offset_value < 0) {
				throw InvalidInputException("offset must be non-negative");
			}
			bind_data->offset = static_cast<idx_t>(offset_value);
			bind_data->has_offset = true;
		}
		auto limit_param = input.named_parameters.find("row_limit");
		if (limit_param != input.named_parameters.end()) {
			auto limit_value = limit_param->second.GetValue<int64_t>();
			if (limit_value < 0) {
				throw InvalidInputException("limit must be non-negative");
			}
			bind_data->limit = static_cast<idx_t>(limit_value);
			bind_data->has_limit = true;
		}
	}

	if (bind_data->has_value_column_filter) {
		std::vector<ValueColumnDefinition> filtered_columns;
		filtered_columns.reserve(bind_data->requested_value_columns.size());
		std::unordered_set<std::string> seen;
		for (auto &requested : bind_data->requested_value_columns) {
			auto match = std::find_if(bind_data->value_columns.begin(), bind_data->value_columns.end(),
			                          [&](const ValueColumnDefinition &def) { return def.name == requested; });
			if (match == bind_data->value_columns.end()) {
				throw InvalidInputException("Unknown value column '%s' for symbol '%s'", requested.c_str(),
				                            bind_data->symbol.c_str());
			}
			if (!seen.insert(requested).second) {
				throw InvalidInputException("Duplicate value column '%s' in value_columns parameter",
				                            requested.c_str());
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
	state->offset = bind.offset;
	state->limit = bind.limit;
	state->has_offset = bind.has_offset;
	state->has_limit = bind.has_limit;

	state->handle = CreateGDXHandle();
	int open_error = 0;
	if (!gdxOpenReadFromRandomAccess(state->handle.get(), &state->provider.GetCallbacks(), &open_error)) {
		GDXErrorContext error_context("gdxOpenReadFromRandomAccess");
		error_context.WithFile(state->resolved_path);
		ThrowGDXError(open_error, error_context);
	}

	// Preload entire UEL table for fast O(1) lookups during data reading
	state->PreloadUELTable();

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

	// Use filtered read path when dimension filters are present
	if (state->has_dimension_filters && !state->dimension_filter_indices.empty()) {
		state->use_filtered_read = true;
		state->filtered_context = make_uniq<FilteredReadContext>();
		state->filtered_context->handle = state->handle.get();
		state->filtered_context->dimension = state->dimension;
		state->filtered_context->resolved_path = state->resolved_path;
		state->filtered_context->symbol = state->symbol;
		state->filtered_context->total_records = bind.record_count; // For progress tracking
		state->filtered_context->offset = state->offset;
		state->filtered_context->skip_remaining = state->offset;
		state->filtered_context->limit = state->limit;
		state->filtered_context->has_limit = state->has_limit;

		// Prepare filter strings for gdxDataReadRawFastFilt
		state->filtered_context->PrepareFilters(state->dimension, state->dimension_filter_indices,
		                                        state->dimension_filter_values);

		// Set thread-local context for callback (gdxDataReadRawFastFilt doesn't pass Uptr correctly)
		g_filtered_read_context = state->filtered_context.get();

		if (state->has_limit) {
			state->filtered_context->buffered_records.reserve(state->limit);
		}

		// Execute filtered read - this reads until offset/limit satisfied
		int result = gdxDataReadRawFastFilt(state->handle.get(), state->symbol_index,
		                                    state->filtered_context->filter_ptrs.data(), FilteredReadCallback);

		// Clear thread-local context
		g_filtered_read_context = nullptr;

		if (result == 0 && !state->filtered_context->stop_requested) {
			GDXErrorContext error_context("gdxDataReadRawFastFilt");
			error_context.WithFile(state->resolved_path).WithSymbol(bind.symbol);
			ThrowGDXError(gdxGetLastError(state->handle.get()), error_context);
		}

		state->filtered_context->read_complete = true;
		state->record_count = state->filtered_context->buffered_records.size();
		state->data_read_started = true;
		state->data_read_finished = true; // gdxDataReadRawFastFilt calls gdxDataReadDone internally
	} else {
		// Fast unfiltered read path using gdxDataReadRawFastEx + UEL cache
		// This uses a callback to read all records at once, avoiding per-call overhead
		state->use_filtered_read = true; // Reuse the same buffered read path
		state->filtered_context = make_uniq<FilteredReadContext>();
		state->filtered_context->handle = state->handle.get();
		state->filtered_context->dimension = state->dimension;
		state->filtered_context->resolved_path = state->resolved_path;
		state->filtered_context->symbol = state->symbol;
		state->filtered_context->total_records = bind.record_count;
		state->filtered_context->offset = state->offset;
		state->filtered_context->skip_remaining = state->offset;
		state->filtered_context->limit = state->limit;
		state->filtered_context->has_limit = state->has_limit;

		// Reserve approximate capacity to avoid reallocations
		if (state->has_limit) {
			state->filtered_context->buffered_records.reserve(state->limit);
		} else {
			state->filtered_context->buffered_records.reserve(bind.record_count);
		}

		// Execute fast unfiltered read - reads until offset/limit satisfied
		int nr_records = 0;
		int result = gdxDataReadRawFastEx(state->handle.get(), state->symbol_index, UnfilteredReadCallback, &nr_records,
		                                  state->filtered_context.get());

		if (result == 0 && !state->filtered_context->stop_requested) {
			GDXErrorContext error_context("gdxDataReadRawFastEx");
			error_context.WithFile(state->resolved_path).WithSymbol(bind.symbol);
			ThrowGDXError(gdxGetLastError(state->handle.get()), error_context);
		}

		state->filtered_context->read_complete = true;
		state->record_count = state->filtered_context->buffered_records.size();
		state->data_read_started = true;
		state->data_read_finished = true; // gdxDataReadRawFastEx calls gdxDataReadDone internally
	}

	return std::move(state);
}

unique_ptr<LocalTableFunctionState> ReadGDXInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                     GlobalTableFunctionState *) {
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

	// Filtered read path: read from buffered records (already filtered by GDX)
	if (state.use_filtered_read && state.filtered_context) {
		while (produced < target_count && state.filtered_context->HasMoreRecords()) {
			auto *record = state.filtered_context->GetNextRecord();
			if (!record) {
				break;
			}

			// Convert raw UEL indices to strings using cached lookups
			for (idx_t col = 0; col < bind.domain_column_count && col < record->raw_indices.size(); ++col) {
				int uel_nr = record->raw_indices[col];
				const std::string &uel_str = state.GetUELString(uel_nr);
				FlatVector::SetNull(output.data[col], produced, false);
				domain_data[col][produced] = StringVector::AddString(output.data[col], uel_str);
			}

			// Metadata columns (sparse/dense indicators) - set to null for filtered reads
			// since we don't have first_dim info from callback
			if (sparse_break_vector && dense_run_vector) {
				FlatVector::SetNull(*sparse_break_vector, produced, true);
				FlatVector::SetNull(*dense_run_vector, produced, true);
			}

			// Value columns
			for (idx_t value_idx = 0; value_idx < bind.value_column_count; ++value_idx) {
				auto &definition = bind.value_columns[value_idx];
				switch (definition.kind) {
				case ValueColumnKind::SetMembership:
					SetBooleanValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_LEVEL], true);
					break;
				case ValueColumnKind::SetText: {
					// For sets, the level value is an index into the text table
					int txt_nr = static_cast<int>(record->values[GMS_VAL_LEVEL]);
					std::string text = state.GetSetElementText(txt_nr);
					FlatVector::SetNull(*value_vectors[value_idx], produced, text.empty());
					if (!text.empty()) {
						FlatVector::GetData<string_t>(*value_vectors[value_idx])[produced] =
						    StringVector::AddString(*value_vectors[value_idx], text);
					}
					break;
				}
				case ValueColumnKind::Level:
					SetDoubleValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_LEVEL]);
					break;
				case ValueColumnKind::Marginal:
					SetDoubleValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_MARGINAL]);
					break;
				case ValueColumnKind::Lower:
					SetDoubleValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_LOWER]);
					break;
				case ValueColumnKind::Upper:
					SetDoubleValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_UPPER]);
					break;
				case ValueColumnKind::Scale:
					SetDoubleValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_SCALE]);
					break;
				case ValueColumnKind::RawValue:
					SetDoubleValue(*value_vectors[value_idx], produced, state, record->values[GMS_VAL_LEVEL]);
					break;
				}
			}

			produced++;
		}

		if (!state.filtered_context->HasMoreRecords()) {
			state.data_exhausted = true;
		}
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

double ReadGDXProgress(ClientContext &, const FunctionData *bind_data_p, const GlobalTableFunctionState *gstate_p) {
	auto &state = gstate_p->Cast<ReadGDXGlobalState>();

	// For filtered reads, we track progress during the callback scan
	if (state.use_filtered_read && state.filtered_context) {
		// After scan complete, progress is based on buffered records consumed
		if (state.filtered_context->read_complete) {
			if (state.filtered_context->buffered_records.empty()) {
				return 1.0;
			}
			return static_cast<double>(state.filtered_context->current_record_index) /
			       static_cast<double>(state.filtered_context->buffered_records.size());
		}
		// During scan, use the atomic counter for progress
		if (state.filtered_context->total_records > 0) {
			idx_t scanned = state.filtered_context->records_scanned.load(std::memory_order_relaxed);
			return static_cast<double>(scanned) / static_cast<double>(state.filtered_context->total_records);
		}
		return -1.0; // Indeterminate if we don't know total
	}

	// For unfiltered reads, progress is rows read / total records
	if (state.record_count == 0) {
		return 1.0; // No records = complete
	}
	return static_cast<double>(state.rows_read.load(std::memory_order_relaxed)) /
	       static_cast<double>(state.record_count);
}

} // namespace

void RegisterReadTableFunction(DatabaseInstance &db) {
	auto function = TableFunction("read_gdx", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ReadGDXFunction);
	function.bind = ReadGDXBind;
	function.init_global = ReadGDXInitGlobal;
	function.init_local = ReadGDXInitLocal;
	function.pushdown_complex_filter = ReadGDXPushdownComplexFilter;
	function.table_scan_progress = ReadGDXProgress;
	function.named_parameters["dimension_filters"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	function.named_parameters["value_columns"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["row_offset"] = LogicalType::BIGINT;
	function.named_parameters["row_limit"] = LogicalType::BIGINT;

	ExtensionUtil::RegisterFunction(db, function);
}

} // namespace gdx
} // namespace duckdb
