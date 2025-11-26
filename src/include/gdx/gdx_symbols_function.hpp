#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gdx {

//! Register the gdx_symbols table function with DuckDB.
void RegisterSymbolsTableFunction(DatabaseInstance &db);

} // namespace gdx
} // namespace duckdb
