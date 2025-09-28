#include "gdx/gdx_symbol_utils.hpp"

#include "gclgms.h"

#include "duckdb/common/string_util.hpp"
#include <string>
#include <unordered_set>

namespace duckdb {
namespace gdx {
namespace {

const std::vector<ValueColumnDefinition> &GetSetValueColumns() {
	static const std::vector<ValueColumnDefinition> columns = {
		{"is_member", LogicalType::BOOLEAN, ValueColumnKind::SetMembership}
	};
	return columns;
}

const std::vector<ValueColumnDefinition> &GetParameterValueColumns() {
	static const std::vector<ValueColumnDefinition> columns = {
		{"value", LogicalType::DOUBLE, ValueColumnKind::Level}
	};
	return columns;
}

const std::vector<ValueColumnDefinition> &GetVariableValueColumns() {
	static const std::vector<ValueColumnDefinition> columns = {
		{"level", LogicalType::DOUBLE, ValueColumnKind::Level},
		{"marginal", LogicalType::DOUBLE, ValueColumnKind::Marginal},
		{"lower", LogicalType::DOUBLE, ValueColumnKind::Lower},
		{"upper", LogicalType::DOUBLE, ValueColumnKind::Upper},
		{"scale", LogicalType::DOUBLE, ValueColumnKind::Scale}
	};
	return columns;
}

const std::vector<ValueColumnDefinition> &GetEquationValueColumns() {
	static const std::vector<ValueColumnDefinition> columns = {
		{"level", LogicalType::DOUBLE, ValueColumnKind::Level},
		{"marginal", LogicalType::DOUBLE, ValueColumnKind::Marginal},
		{"lower", LogicalType::DOUBLE, ValueColumnKind::Lower},
		{"upper", LogicalType::DOUBLE, ValueColumnKind::Upper},
		{"scale", LogicalType::DOUBLE, ValueColumnKind::Scale}
	};
	return columns;
}

const std::vector<ValueColumnDefinition> &GetFallbackValueColumns() {
	static const std::vector<ValueColumnDefinition> columns = {
		{"value", LogicalType::DOUBLE, ValueColumnKind::RawValue}
	};
	return columns;
}

} // namespace

const std::vector<ValueColumnDefinition> &GetValueColumnDefinitions(int symbol_type) {
	switch (symbol_type) {
	case GMS_DT_SET:
		return GetSetValueColumns();
	case GMS_DT_PAR:
		return GetParameterValueColumns();
	case GMS_DT_VAR:
		return GetVariableValueColumns();
	case GMS_DT_EQU:
		return GetEquationValueColumns();
	default:
		return GetFallbackValueColumns();
	}
}

void BuildReadGDXSchema(const std::vector<std::string> &domain_labels, int symbol_type,
						const std::vector<ValueColumnDefinition> &value_columns,
						std::vector<LogicalType> &return_types, std::vector<std::string> &names) {
	return_types.clear();
	names.clear();

	std::unordered_set<std::string> used_names;

	auto add_unique_column = [&](const std::string &base_name, LogicalType type) {
		std::string candidate = base_name;
		idx_t suffix = 1;
		while (candidate.empty() || used_names.count(candidate) > 0) {
			candidate = StringUtil::Format("%s_%lld", base_name.c_str(), static_cast<long long>(suffix++));
		}
		used_names.insert(candidate);
		names.emplace_back(candidate);
		return_types.emplace_back(type);
	};

	idx_t dimensionality = domain_labels.size();
	for (idx_t i = 0; i < dimensionality; ++i) {
		std::string name = domain_labels[i];
		if (name.empty() || name == "*") {
			name = StringUtil::Format("dim_%d", static_cast<int>(i + 1));
		}
		add_unique_column(name, LogicalType::VARCHAR);
	}

	if (dimensionality > 1) {
		add_unique_column("is_sparse_break", LogicalType::BOOLEAN);
		add_unique_column("is_dense_run", LogicalType::BOOLEAN);
	}

	const auto &columns_to_add = value_columns.empty() ? GetValueColumnDefinitions(symbol_type) : value_columns;
	for (auto &col : columns_to_add) {
		add_unique_column(col.name, col.type);
	}
}

void BuildGDXSymbolsSchema(std::vector<LogicalType> &return_types, std::vector<std::string> &names) {
	return_types.clear();
	names.clear();

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("symbol_name");

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("symbol_type");

	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("dimension_count");

	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("record_count");

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("description");

	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("domain_labels");
}

std::string SymbolTypeToString(int symbol_type) {
	if (symbol_type >= 0 && symbol_type < GMS_DT_MAX) {
		const auto *text = gmsGdxTypeText[symbol_type];
		if (text && text[0] != '\0') {
			return std::string(text);
		}
	}
	return "Unknown";
}

} // namespace gdx
} // namespace duckdb
