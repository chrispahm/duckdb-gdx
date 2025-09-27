#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gdx {

//! Register the read_gdx table function with DuckDB.
void RegisterReadTableFunction(ExtensionLoader &loader);

} // namespace gdx
} // namespace duckdb
