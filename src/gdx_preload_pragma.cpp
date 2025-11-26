#include "gdx/gdx_preload_pragma.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/main/extension_util.hpp"

#include "gdx/gdx_metadata_cache.hpp"

namespace duckdb {
namespace gdx {

namespace {

void GDXPreloadPragma(ClientContext &context, const FunctionParameters &parameters) {
	if (parameters.values.empty()) {
		throw InvalidInputException("PRAGMA gdx_preload requires a file_or_url argument");
	}

	auto file_or_url = parameters.values[0].ToString();
	bool force_reload = false;
	std::string requested_symbol;
	bool has_symbol = false;

	auto force_it = parameters.named_parameters.find("force_reload");
	if (force_it != parameters.named_parameters.end()) {
		force_reload = force_it->second.GetValue<bool>();
	}

	auto symbol_it = parameters.named_parameters.find("symbol");
	if (symbol_it != parameters.named_parameters.end()) {
		if (symbol_it->second.IsNull()) {
			throw InvalidInputException("symbol parameter to gdx_preload cannot be NULL");
		}
		requested_symbol = symbol_it->second.ToString();
		has_symbol = true;
	}

	auto metadata_entry = GDXMetadataCache::Get().GetOrLoad(context, file_or_url, force_reload);

	if (has_symbol) {
		auto normalized = StringUtil::Upper(requested_symbol);
		bool found = false;
		for (auto &symbol : metadata_entry->symbols) {
			if (StringUtil::Upper(symbol.name) == normalized) {
				found = true;
				break;
			}
		}
		if (!found) {
			throw InvalidInputException("Symbol '%s' not found in '%s'", requested_symbol.c_str(),
			                            file_or_url.c_str());
		}
	}
}

} // namespace

void RegisterPreloadPragma(DatabaseInstance &db) {
	auto pragma = PragmaFunction::PragmaCall("gdx_preload", GDXPreloadPragma, {LogicalType::VARCHAR}, LogicalType::ANY);
	pragma.named_parameters["symbol"] = LogicalType::VARCHAR;
	pragma.named_parameters["force_reload"] = LogicalType::BOOLEAN;
	ExtensionUtil::RegisterFunction(db, pragma);
}

} // namespace gdx
} // namespace duckdb
