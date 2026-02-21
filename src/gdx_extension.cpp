#define DUCKDB_EXTENSION_MAIN

#include "gdx/gdx_extension.hpp"
#include "gdx/gdx_read_function.hpp"
#include "gdx/gdx_symbols_function.hpp"
#include "gdx/gdx_domain_values_function.hpp"
#include "gdx/gdx_preload_pragma.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	fprintf(stderr, "[GDX] LoadInternal called\n");
	// Register domain values FIRST to see if order matters
	gdx::RegisterGDXDomainValuesFunction(loader);
	fprintf(stderr, "[GDX] Registered gdx_domain_values\n");
	gdx::RegisterReadTableFunction(loader);
	fprintf(stderr, "[GDX] Registered read_gdx\n");
	gdx::RegisterSymbolsTableFunction(loader);
	fprintf(stderr, "[GDX] Registered gdx_symbols\n");
	gdx::RegisterPreloadPragma(loader);
	fprintf(stderr, "[GDX] Registered preload pragma\n");
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
