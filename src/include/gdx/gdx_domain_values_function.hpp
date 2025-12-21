#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gdx {

//! Register the gdx_domain_values table function with DuckDB.
//! This function returns all unique values for a specific dimension of a symbol,
//! which is useful for building filter dropdowns in UI applications.
__attribute__((used)) void RegisterGDXDomainValuesFunction(ExtensionLoader &loader);

} // namespace gdx
} // namespace duckdb
