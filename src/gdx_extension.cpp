#define DUCKDB_EXTENSION_MAIN

#include "gdx/gdx_extension.hpp"
#include "gdx/gdx_read_function.hpp"
#include "gdx/gdx_symbols_function.hpp"
#include "gdx/gdx_domain_values_function.hpp"
#include "gdx/gdx_preload_pragma.hpp"

namespace duckdb {

static void LoadInternal(DatabaseInstance &db) {
	fprintf(stderr, "[GDX] LoadInternal called\n");
	// Register domain values FIRST to see if order matters
	gdx::RegisterGDXDomainValuesFunction(db);
	fprintf(stderr, "[GDX] Registered gdx_domain_values\n");
	gdx::RegisterReadTableFunction(db);
	fprintf(stderr, "[GDX] Registered read_gdx\n");
	gdx::RegisterSymbolsTableFunction(db);
	fprintf(stderr, "[GDX] Registered gdx_symbols\n");
	gdx::RegisterPreloadPragma(db);
	fprintf(stderr, "[GDX] Registered preload pragma\n");
}

void DuckdbGdxExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
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

// Force the linker to include RegisterGDXDomainValuesFunction by making it an exported function
DUCKDB_EXTENSION_API void duckdb_gdx_force_domain_values_registration(duckdb::DatabaseInstance &db) {
	duckdb::gdx::RegisterGDXDomainValuesFunction(db);
}

DUCKDB_EXTENSION_API void duckdb_gdx_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::DuckdbGdxExtension>();
}

DUCKDB_EXTENSION_API const char *duckdb_gdx_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}
