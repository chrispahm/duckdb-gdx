#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace gdx {

enum class ValueColumnKind {
	SetMembership,
	SetText,
	Level,
	Marginal,
	Lower,
	Upper,
	Scale,
	RawValue
};

struct ValueColumnDefinition {
	std::string name;
	duckdb::LogicalType type;
	ValueColumnKind kind;
};

//! Returns logical type definitions for value columns based on the GDX symbol type.
const std::vector<ValueColumnDefinition> &GetValueColumnDefinitions(int symbol_type);

//! Builds the output schema for read_gdx based on the domain labels and symbol type.
void BuildReadGDXSchema(const std::vector<std::string> &domain_labels, int symbol_type,
						 const std::vector<ValueColumnDefinition> &value_columns,
						 std::vector<duckdb::LogicalType> &return_types, std::vector<std::string> &names);

//! Builds the output schema for the gdx_symbols table function.
void BuildGDXSymbolsSchema(std::vector<duckdb::LogicalType> &return_types, std::vector<std::string> &names);

//! Maps a raw GDX symbol type constant (GMS_DT_*) to a friendly name.
std::string SymbolTypeToString(int symbol_type);

} // namespace gdx
} // namespace duckdb
