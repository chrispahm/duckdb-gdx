#include "gdx/gdx_symbol_utils.hpp"

#include "gclgms.h"

#include "duckdb/common/string_util.hpp"
#include <string>
#include <unordered_set>

namespace duckdb {
namespace gdx {
namespace {

std::string MakeValueColumnName(int symbol_type) {
	switch (symbol_type) {
	case GMS_DT_SET:
		return "is_member";
	case GMS_DT_PAR:
		return "value";
	case GMS_DT_VAR:
		return "level";
	case GMS_DT_EQU:
		return "level";
	default:
		return "value";
	}
}

} // namespace

void BuildReadGDXSchema(const std::vector<std::string> &domain_labels, int symbol_type,
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

	auto add_numeric_value_columns = [&](const std::vector<std::pair<std::string, LogicalType>> &cols) {
		for (auto &col : cols) {
			add_unique_column(col.first, col.second);
		}
	};

	switch (symbol_type) {
	case GMS_DT_SET: {
		add_unique_column(MakeValueColumnName(symbol_type), LogicalType::BOOLEAN);
		break;
	}
	case GMS_DT_PAR: {
		add_unique_column(MakeValueColumnName(symbol_type), LogicalType::DOUBLE);
		break;
	}
	case GMS_DT_VAR:
	case GMS_DT_EQU: {
		auto cols = std::vector<std::pair<std::string, LogicalType>> {
			{"level", LogicalType::DOUBLE},
			{"marginal", LogicalType::DOUBLE},
			{"lower", LogicalType::DOUBLE},
			{"upper", LogicalType::DOUBLE},
			{"scale", LogicalType::DOUBLE}
		};
		add_numeric_value_columns(cols);
		break;
	}
	default: {
		add_unique_column("value", LogicalType::DOUBLE);
		break;
	}
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
