#include "gdx/gdx_read_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include "gdx/gdx_error.hpp"
#include "gdx/gdx_file_provider.hpp"
#define NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_handle.hpp"
#undef NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_symbol_utils.hpp"

#include "gclgms.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdx {

namespace {

struct ReadGDXBindData : public TableFunctionData {
	std::string file_or_url;
	std::string resolved_path;
	std::string symbol;
	bool is_remote {false};
	int symbol_type {0};
	idx_t dimension {0};
	idx_t record_count {0};
	idx_t domain_column_count {0};
	idx_t value_column_offset {0};
	idx_t value_column_count {0};
	std::vector<std::string> domain_labels;
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

vector<string> ExtractDomainLabels(TGXFileRec_t *handle, const std::string &resolved_path, const std::string &symbol,
		int symbol_index, idx_t dimension) {
	vector<string> labels;
	labels.reserve(dimension);
	if (dimension == 0) {
		return labels;
	}

	std::vector<std::array<char, GMS_SSSIZE>> domain_buffers(dimension);
	std::vector<char *> domain_ptrs;
	domain_ptrs.reserve(dimension);
	for (idx_t i = 0; i < dimension; ++i) {
		domain_buffers[i].fill('\0');
		domain_ptrs.push_back(domain_buffers[i].data());
	}

	int domain_rc = gdxSymbolGetDomainX(handle, symbol_index, domain_ptrs.data());
	if (domain_rc == 0) {
		int error_code = gdxGetLastError(handle);
		GDXErrorContext context("gdxSymbolGetDomainX");
		context.WithFile(resolved_path).WithSymbol(symbol);
		ThrowGDXError(error_code, context);
	}

	for (idx_t i = 0; i < dimension; ++i) {
		if (domain_rc == 1 || domain_ptrs[i][0] == '\0') {
			labels.emplace_back("*");
		} else {
			labels.emplace_back(domain_ptrs[i]);
		}
	}
	return labels;
}

unique_ptr<FunctionData> ReadGDXBind(ClientContext &context, TableFunctionBindInput &input,
		 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() < 2) {
		throw InvalidInputException("read_gdx requires a file_or_url and symbol argument");
	}

	auto bind_data = make_uniq<ReadGDXBindData>();
	bind_data->file_or_url = input.inputs[0].ToString();
	bind_data->symbol = input.inputs[1].ToString();

	GDXFileRandomAccessProvider provider;
	provider.Initialize(context, bind_data->file_or_url);
	bind_data->resolved_path = provider.ResolvedPath();
	bind_data->is_remote = provider.IsRemote();

	auto handle = CreateGDXHandle();
	int open_error = 0;
	if (!gdxOpenReadFromRandomAccess(handle.get(), &provider.GetCallbacks(), &open_error)) {
		GDXErrorContext error_context("gdxOpenReadFromRandomAccess");
		error_context.WithFile(bind_data->resolved_path);
		ThrowGDXError(open_error, error_context);
	}

	int symbol_index = 0;
	if (!gdxFindSymbol(handle.get(), bind_data->symbol.c_str(), &symbol_index)) {
		GDXErrorContext error_context("gdxFindSymbol");
		error_context.WithFile(bind_data->resolved_path).WithSymbol(bind_data->symbol);
		ThrowGDXError(gdxGetLastError(handle.get()), error_context);
	}

	int dimension = 0;
	int symbol_type = 0;
	std::array<char, GMS_SSSIZE> resolved_symbol_name {};
	if (!gdxSymbolInfo(handle.get(), symbol_index, resolved_symbol_name.data(), &dimension, &symbol_type)) {
		GDXErrorContext error_context("gdxSymbolInfo");
		error_context.WithFile(bind_data->resolved_path).WithSymbol(bind_data->symbol);
		ThrowGDXError(gdxGetLastError(handle.get()), error_context);
	}

	if (symbol_type == GMS_DT_ALIAS) {
		throw InvalidInputException(StringUtil::Format("read_gdx does not support alias symbols: \"%s\"",
		                                             bind_data->symbol.c_str()));
	}

	std::array<char, GMS_SSSIZE> description_buffer {};
	int record_count = 0;
	int user_info = 0;
	if (!gdxSymbolInfoX(handle.get(), symbol_index, &record_count, &user_info, description_buffer.data())) {
		GDXErrorContext error_context("gdxSymbolInfoX");
		error_context.WithFile(bind_data->resolved_path).WithSymbol(bind_data->symbol);
		ThrowGDXError(gdxGetLastError(handle.get()), error_context);
	}

	bind_data->symbol_type = symbol_type;
	bind_data->dimension = dimension < 0 ? 0 : static_cast<idx_t>(dimension);
	bind_data->record_count = record_count < 0 ? 0 : static_cast<idx_t>(record_count);
	bind_data->domain_labels = ExtractDomainLabels(handle.get(), bind_data->resolved_path, bind_data->symbol, symbol_index,
		bind_data->dimension);

	BuildReadGDXSchema(bind_data->domain_labels, bind_data->symbol_type, return_types, names);
	bind_data->domain_column_count = bind_data->domain_labels.size();
	bind_data->value_column_offset = bind_data->domain_column_count;

	switch (bind_data->symbol_type) {
	case GMS_DT_SET:
		bind_data->value_column_count = 1;
		break;
	case GMS_DT_PAR:
		bind_data->value_column_count = 1;
		break;
	case GMS_DT_VAR:
	case GMS_DT_EQU:
		bind_data->value_column_count = 5;
		break;
	default:
		bind_data->value_column_count = 1;
		break;
	}

	int close_error = gdxClose(handle.get());
	if (close_error != 0) {
		GDXErrorContext error_context("gdxClose");
		error_context.WithFile(bind_data->resolved_path);
		ThrowGDXError(close_error, error_context);
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

	std::array<Vector *, 5> value_vectors {};
	for (idx_t i = 0; i < bind.value_column_count; ++i) {
		value_vectors[i] = &output.data[bind.value_column_offset + i];
	}

	while (produced < target_count) {
		int first_dim = 0;
		int read_success = gdxDataReadStr(state.handle.get(), local.key_ptrs.data(), local.value_buffer.data(), &first_dim);
		if (read_success == 0) {
			state.data_exhausted = true;
			break;
		}

		for (idx_t col = 0; col < bind.domain_column_count; ++col) {
			FlatVector::SetNull(output.data[col], produced, false);
			domain_data[col][produced] = StringVector::AddString(output.data[col], local.key_buffer[col].data());
		}

		switch (bind.symbol_type) {
		case GMS_DT_SET: {
			SetBooleanValue(*value_vectors[0], produced, state, local.value_buffer[GMS_VAL_LEVEL], true);
			break;
		}
		case GMS_DT_PAR: {
			SetDoubleValue(*value_vectors[0], produced, state, local.value_buffer[GMS_VAL_LEVEL]);
			break;
		}
		case GMS_DT_VAR:
		case GMS_DT_EQU: {
			SetDoubleValue(*value_vectors[0], produced, state, local.value_buffer[GMS_VAL_LEVEL]);
			SetDoubleValue(*value_vectors[1], produced, state, local.value_buffer[GMS_VAL_MARGINAL]);
			SetDoubleValue(*value_vectors[2], produced, state, local.value_buffer[GMS_VAL_LOWER]);
			SetDoubleValue(*value_vectors[3], produced, state, local.value_buffer[GMS_VAL_UPPER]);
			SetDoubleValue(*value_vectors[4], produced, state, local.value_buffer[GMS_VAL_SCALE]);
			break;
		}
		default: {
			SetDoubleValue(*value_vectors[0], produced, state, local.value_buffer[GMS_VAL_LEVEL]);
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

	loader.RegisterFunction(function);
}

} // namespace gdx
} // namespace duckdb
