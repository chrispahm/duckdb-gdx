#include "gdx/gdx_symbol_utils.hpp"

#include "catch.hpp"
#include "gclgms.h"

#include "duckdb/common/types.hpp"

using namespace duckdb;
using namespace duckdb::gdx;

TEST_CASE("Value column definitions match GDX symbol type", "[gdx][symbol_utils]") {
	const auto &set_columns = GetValueColumnDefinitions(GMS_DT_SET);
	REQUIRE(set_columns.size() == 1);
	REQUIRE(set_columns[0].name == "is_member");
	REQUIRE(set_columns[0].type == LogicalType::BOOLEAN);
	REQUIRE(set_columns[0].kind == ValueColumnKind::SetMembership);

	const auto &param_columns = GetValueColumnDefinitions(GMS_DT_PAR);
	REQUIRE(param_columns.size() == 1);
	REQUIRE(param_columns[0].name == "value");
	REQUIRE(param_columns[0].type == LogicalType::DOUBLE);
	REQUIRE(param_columns[0].kind == ValueColumnKind::Level);

	const auto &variable_columns = GetValueColumnDefinitions(GMS_DT_VAR);
	REQUIRE(variable_columns.size() == 5);
	REQUIRE(variable_columns[0].name == "level");
	REQUIRE(variable_columns[1].name == "marginal");
	REQUIRE(variable_columns[2].name == "lower");
	REQUIRE(variable_columns[3].name == "upper");
	REQUIRE(variable_columns[4].name == "scale");

	const auto &fallback_columns = GetValueColumnDefinitions(-42);
	REQUIRE(fallback_columns.size() == 1);
	REQUIRE(fallback_columns[0].name == "value");
	REQUIRE(fallback_columns[0].kind == ValueColumnKind::RawValue);
}

TEST_CASE("BuildReadGDXSchema sanitizes domain labels and enforces uniqueness", "[gdx][symbol_utils]") {
	std::vector<std::string> domain_labels = {"", "region", "*", "region"};
	std::vector<LogicalType> return_types;
	std::vector<std::string> names;

	BuildReadGDXSchema(domain_labels, GMS_DT_PAR, {}, return_types, names);

	REQUIRE(names.size() == return_types.size());
	REQUIRE(names.size() == 7);

	REQUIRE(names[0] == "dim_1");
	REQUIRE(return_types[0] == LogicalType::VARCHAR);

	REQUIRE(names[1] == "region");
	REQUIRE(return_types[1] == LogicalType::VARCHAR);

	REQUIRE(names[2] == "dim_3");
	REQUIRE(return_types[2] == LogicalType::VARCHAR);

	REQUIRE(names[3] == "region_1");
	REQUIRE(return_types[3] == LogicalType::VARCHAR);

	REQUIRE(names[4] == "is_sparse_break");
	REQUIRE(return_types[4] == LogicalType::BOOLEAN);

	REQUIRE(names[5] == "is_dense_run");
	REQUIRE(return_types[5] == LogicalType::BOOLEAN);

	REQUIRE(names[6] == "value");
	REQUIRE(return_types[6] == LogicalType::DOUBLE);
}

TEST_CASE("BuildReadGDXSchema applies overrides and deduplicates value columns", "[gdx][symbol_utils]") {
	std::vector<std::string> domain_labels = {"i"};
	std::vector<LogicalType> return_types;
	std::vector<std::string> names;

	std::vector<ValueColumnDefinition> overrides = {{"custom", LogicalType::INTEGER, ValueColumnKind::RawValue},
	                                                {"custom", LogicalType::VARCHAR, ValueColumnKind::RawValue}};

	BuildReadGDXSchema(domain_labels, GMS_DT_SET, overrides, return_types, names);

	REQUIRE(names.size() == return_types.size());
	REQUIRE(names.size() == 3);

	REQUIRE(names[0] == "i");
	REQUIRE(return_types[0] == LogicalType::VARCHAR);

	REQUIRE(names[1] == "custom");
	REQUIRE(return_types[1] == LogicalType::INTEGER);

	REQUIRE(names[2] == "custom_1");
	REQUIRE(return_types[2] == LogicalType::VARCHAR);
}

TEST_CASE("SymbolTypeToString maps known and unknown values", "[gdx][symbol_utils]") {
	REQUIRE(SymbolTypeToString(GMS_DT_SET) == "Set");
	REQUIRE(SymbolTypeToString(GMS_DT_PAR) == "Parameter");
	REQUIRE(SymbolTypeToString(GMS_DT_VAR) == "Variable");
	REQUIRE(SymbolTypeToString(123456) == "Unknown");
}
