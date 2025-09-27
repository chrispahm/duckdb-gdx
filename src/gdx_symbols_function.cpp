#include "gdx/gdx_symbols_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/table_function.hpp"
#include "gdx/gdx_error.hpp"
#include "gdx/gdx_file_provider.hpp"
#define NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_handle.hpp"
#undef NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_symbol_utils.hpp"

#include "gdx_random_access.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdx {

namespace {

struct GDXSymbolMetadata {
	std::string name;
	std::string type_name;
	uint64_t dimension_count;
	uint64_t record_count;
	std::string description;
	std::vector<std::string> domain_labels;
};

struct GDXSymbolsBindData : public TableFunctionData {
	std::string file_or_url;
	std::string resolved_path;
	bool is_remote {false};
	std::vector<GDXSymbolMetadata> symbols;
};

struct GDXSymbolsGlobalState : public GlobalTableFunctionState {
	idx_t MaxThreads() const override {
		return 1;
	}
};

struct GDXSymbolsLocalState : public LocalTableFunctionState {
	idx_t offset {0};
};

unique_ptr<FunctionData> GDXSymbolsBind(ClientContext &context, TableFunctionBindInput &input,
	vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty()) {
		throw InvalidInputException("gdx_symbols requires a file_or_url argument");
	}

	auto bind_data = make_uniq<GDXSymbolsBindData>();
	bind_data->file_or_url = input.inputs[0].ToString();

	BuildGDXSymbolsSchema(return_types, names);

	GDXFileRandomAccessProvider provider;
	provider.Initialize(context, bind_data->file_or_url);
	bind_data->resolved_path = provider.ResolvedPath();
	bind_data->is_remote = provider.IsRemote();

	auto handle = CreateGDXHandle();
	int open_error = 0;
	if (!gdxOpenReadFromRandomAccess(handle.get(), &provider.GetCallbacks(), &open_error)) {
		GDXErrorContext context("gdxOpenReadFromRandomAccess");
		context.WithFile(bind_data->resolved_path);
		ThrowGDXError(open_error, context);
	}

	int symbol_count = 0;
	int uel_count = 0;
	if (!gdxSystemInfo(handle.get(), &symbol_count, &uel_count)) {
		int error_code = gdxGetLastError(handle.get());
		GDXErrorContext context("gdxSystemInfo");
		context.WithFile(bind_data->resolved_path);
		ThrowGDXError(error_code, context);
	}

	bind_data->symbols.reserve(static_cast<size_t>(std::max(0, symbol_count)));

	for (int sy_nr = 1; sy_nr <= symbol_count; ++sy_nr) {
		std::array<char, 64> name_buffer {};
		int dimension = 0;
		int type = 0;
		if (!gdxSymbolInfo(handle.get(), sy_nr, name_buffer.data(), &dimension, &type)) {
			int error_code = gdxGetLastError(handle.get());
			GDXErrorContext context("gdxSymbolInfo");
			context.WithFile(bind_data->resolved_path)
			    .WithSymbol(std::string(name_buffer.data()))
			    .WithOffset(static_cast<uint64_t>(sy_nr));
			ThrowGDXError(error_code, context);
		}

		std::array<char, 256> description_buffer {};
		int record_count = 0;
		int user_info = 0;
		if (!gdxSymbolInfoX(handle.get(), sy_nr, &record_count, &user_info, description_buffer.data())) {
			int error_code = gdxGetLastError(handle.get());
			GDXErrorContext context("gdxSymbolInfoX");
			context.WithFile(bind_data->resolved_path)
			    .WithSymbol(std::string(name_buffer.data()))
			    .WithOffset(static_cast<uint64_t>(sy_nr));
			ThrowGDXError(error_code, context);
		}

		GDXSymbolMetadata meta;
		meta.name = std::string(name_buffer.data());
		meta.type_name = SymbolTypeToString(type);
		meta.dimension_count = dimension < 0 ? 0 : static_cast<uint64_t>(dimension);
		meta.record_count = record_count < 0 ? 0 : static_cast<uint64_t>(record_count);
		meta.description = std::string(description_buffer.data());

		if (dimension > 0) {
			std::vector<std::array<char, 256>> domain_buffers(static_cast<size_t>(dimension));
			std::vector<char *> domain_ptrs;
			domain_ptrs.reserve(domain_buffers.size());
			for (auto &buffer : domain_buffers) {
				buffer.fill('\0');
				domain_ptrs.push_back(buffer.data());
			}

			int domain_result = gdxSymbolGetDomainX(handle.get(), sy_nr, domain_ptrs.data());
			if (domain_result == 0) {
				int error_code = gdxGetLastError(handle.get());
				GDXErrorContext context("gdxSymbolGetDomainX");
				context.WithFile(bind_data->resolved_path).WithSymbol(meta.name);
				ThrowGDXError(error_code, context);
			}

			meta.domain_labels.reserve(domain_ptrs.size());
			for (idx_t dim_idx = 0; dim_idx < domain_ptrs.size(); ++dim_idx) {
				if (domain_result == 1 || domain_ptrs[dim_idx][0] == '\0') {
					meta.domain_labels.emplace_back("*");
				} else {
					meta.domain_labels.emplace_back(domain_ptrs[dim_idx]);
				}
			}
		} else {
			meta.domain_labels.clear();
		}

		bind_data->symbols.emplace_back(std::move(meta));
	}

	int close_error = gdxClose(handle.get());
	if (close_error != 0) {
		GDXErrorContext context("gdxClose");
		context.WithFile(bind_data->resolved_path);
		ThrowGDXError(close_error, context);
	}
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> GDXSymbolsInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<GDXSymbolsGlobalState>();
}

unique_ptr<LocalTableFunctionState> GDXSymbolsInitLocal(ExecutionContext &, TableFunctionInitInput &, GlobalTableFunctionState *) {
	return make_uniq<GDXSymbolsLocalState>();
}

void GDXSymbolsFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<GDXSymbolsBindData>();
	auto &local = input.local_state->Cast<GDXSymbolsLocalState>();
	auto &symbols = bind.symbols;

	if (local.offset >= symbols.size()) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = std::min<idx_t>(STANDARD_VECTOR_SIZE, symbols.size() - local.offset);

	auto name_data = FlatVector::GetData<string_t>(output.data[0]);
	auto type_data = FlatVector::GetData<string_t>(output.data[1]);
	auto dimension_data = FlatVector::GetData<uint64_t>(output.data[2]);
	auto record_data = FlatVector::GetData<uint64_t>(output.data[3]);
	auto description_data = FlatVector::GetData<string_t>(output.data[4]);

	auto &domain_vector = output.data[5];
	auto list_entries = FlatVector::GetData<list_entry_t>(domain_vector);
	auto &domain_child = ListVector::GetEntry(domain_vector);

	idx_t current_list_size = ListVector::GetListSize(domain_vector);
	idx_t total_new_entries = 0;
	for (idx_t i = 0; i < count; ++i) {
		total_new_entries += symbols[local.offset + i].domain_labels.size();
	}
	if (total_new_entries > 0) {
		ListVector::Reserve(domain_vector, current_list_size + total_new_entries);
	}
	ListVector::SetListSize(domain_vector, current_list_size + total_new_entries);
	auto child_data = FlatVector::GetData<string_t>(domain_child);

	idx_t child_offset = current_list_size;
	for (idx_t i = 0; i < count; ++i) {
		auto &meta = symbols[local.offset + i];
		name_data[i] = StringVector::AddString(output.data[0], meta.name);
		type_data[i] = StringVector::AddString(output.data[1], meta.type_name);
		dimension_data[i] = meta.dimension_count;
		record_data[i] = meta.record_count;
		description_data[i] = StringVector::AddString(output.data[4], meta.description);

		auto &labels = meta.domain_labels;
		list_entries[i].offset = child_offset;
		list_entries[i].length = labels.size();
		for (auto &label : labels) {
			child_data[child_offset++] = StringVector::AddString(domain_child, label);
		}
	}

	output.SetCardinality(count);
	local.offset += count;
}

} // namespace

void RegisterSymbolsTableFunction(ExtensionLoader &loader) {
	auto function = TableFunction("gdx_symbols", {LogicalType::VARCHAR}, GDXSymbolsFunction);
	function.bind = GDXSymbolsBind;
	function.init_global = GDXSymbolsInitGlobal;
	function.init_local = GDXSymbolsInitLocal;

	loader.RegisterFunction(function);
}

} // namespace gdx
} // namespace duckdb
