#include "gdx/gdx_sidecar.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/json.hpp"

namespace duckdb {
namespace gdx {

using nlohmann::json;

GDXSidecarFile LoadSidecar(const std::string &json_text) {
	json j = json::parse(json_text);
	GDXSidecarFile sc;
	sc.source_path = j.value("source_path", "");
	sc.file_size = j.value("file_size", 0ULL);
	sc.modified_time = j.value("modified_time", static_cast<int64_t>(0));
	sc.hash_hint = j.value("hash_hint", "");
	for (auto &sym : j["symbols"]) {
		GDXSidecarSymbol s;
		s.name = sym.value("name", "");
		s.type_code = sym.value("type_code", 0);
		s.dimension_count = sym.value("dimension_count", 0ULL);
		s.record_count = sym.value("record_count", 0ULL);
		s.data_position = sym.value("data_position", 0ULL);
		s.domain_labels = sym.value("domain_labels", std::vector<std::string>{});
		if (sym.contains("cached_domain_values")) {
			s.cached_domain_values = sym["cached_domain_values"].get<std::vector<std::vector<std::string>>>();
		}
		sc.symbols.emplace_back(std::move(s));
	}
	return sc;
}

std::string WriteSidecar(const GDXSidecarFile &sc) {
	json j;
	j["source_path"] = sc.source_path;
	j["file_size"] = sc.file_size;
	j["modified_time"] = sc.modified_time;
	j["hash_hint"] = sc.hash_hint;
	json syms = json::array();
	for (auto &s : sc.symbols) {
		json js;
		js["name"] = s.name;
		js["type_code"] = s.type_code;
		js["dimension_count"] = s.dimension_count;
		js["record_count"] = s.record_count;
		js["data_position"] = s.data_position;
		js["domain_labels"] = s.domain_labels;
		if (!s.cached_domain_values.empty()) {
			js["cached_domain_values"] = s.cached_domain_values;
		}
		syms.push_back(std::move(js));
	}
	j["symbols"] = std::move(syms);
	return j.dump();
}

bool IsSidecarFresh(const GDXSidecarFile &sc, const std::string &resolved_path, uint64_t size, int64_t mtime) {
	if (sc.source_path != resolved_path) {
		return false;
	}
	if (sc.file_size != size) {
		return false;
	}
	if (sc.modified_time != mtime) {
		return false;
	}
	return true;
}

} // namespace gdx
} // namespace duckdb