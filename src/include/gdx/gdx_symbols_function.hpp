#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gdx {

//! Register the gdx_symbols table function with DuckDB.
void RegisterSymbolsTableFunction(ExtensionLoader &loader);

} // namespace gdx
} // namespace duckdb
