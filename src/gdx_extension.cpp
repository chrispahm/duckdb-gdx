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

void GdxExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string GdxExtension::Name() {
	return "gdx";
}

std::string GdxExtension::Version() const {
#ifdef EXT_VERSION_GDX
	return EXT_VERSION_GDX;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(gdx, loader) {
	duckdb::LoadInternal(loader);
}
}
