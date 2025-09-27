#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace gdx {

//! Builds the output schema for read_gdx based on the domain labels and symbol type.
void BuildReadGDXSchema(const std::vector<std::string> &domain_labels, int symbol_type,
						 std::vector<duckdb::LogicalType> &return_types, std::vector<std::string> &names);

//! Builds the output schema for the gdx_symbols table function.
void BuildGDXSymbolsSchema(std::vector<duckdb::LogicalType> &return_types, std::vector<std::string> &names);

//! Maps a raw GDX symbol type constant (GMS_DT_*) to a friendly name.
std::string SymbolTypeToString(int symbol_type);

} // namespace gdx
} // namespace duckdb
