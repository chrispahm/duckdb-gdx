#define DUCKDB_EXTENSION_MAIN

#include "gdx/gdx_extension.hpp"
#include "gdx/gdx_read_function.hpp"
#include "gdx/gdx_symbols_function.hpp"
#include "gdx/gdx_preload_pragma.hpp"

namespace duckdb {

namespace gdx {
void RegisterReadTableFunction(ExtensionLoader &loader);
void RegisterSymbolsTableFunction(ExtensionLoader &loader);
} // namespace gdx

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("DuckDB GDX extension with table functions for reading GDX files");
	gdx::RegisterReadTableFunction(loader);
	gdx::RegisterSymbolsTableFunction(loader);
	gdx::RegisterPreloadPragma(loader);
}

void DuckdbGdxExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DuckdbGdxExtension::Name() {
	return "duckdb_gdx";
}

std::string DuckdbGdxExtension::Version() const {
#ifdef EXT_VERSION_DUCKDB_GDX
	return EXT_VERSION_DUCKDB_GDX;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckdb_gdx, loader) {
	duckdb::LoadInternal(loader);
}

}
