#include "gdx/gdx_symbols_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/table_function.hpp"
#include "gdx/gdx_error.hpp"
#include "gdx/gdx_metadata_cache.hpp"
#include "gdx/gdx_symbol_utils.hpp"

#include "gdx_random_access.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdx {

namespace {

struct GDXSymbolsBindData : public TableFunctionData {
	std::string file_or_url;
	std::shared_ptr<const GDXMetadataEntry> metadata;
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
	bind_data->metadata = GDXMetadataCache::Get().GetOrLoad(context, bind_data->file_or_url);
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> GDXSymbolsInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<GDXSymbolsGlobalState>();
}

unique_ptr<LocalTableFunctionState> GDXSymbolsInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                        GlobalTableFunctionState *) {
	return make_uniq<GDXSymbolsLocalState>();
}

void GDXSymbolsFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<GDXSymbolsBindData>();
	auto &local = input.local_state->Cast<GDXSymbolsLocalState>();
	const auto &symbols = bind.metadata->symbols;

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
		const auto &meta = symbols[local.offset + i];
		name_data[i] = StringVector::AddString(output.data[0], meta.name);
		type_data[i] = StringVector::AddString(output.data[1], SymbolTypeToString(meta.type_code));
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
