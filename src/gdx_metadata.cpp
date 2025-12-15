#include "gdx/gdx_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include "gdx/gdx_error.hpp"
#include "gdx/gdx_file_provider.hpp"
#define NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_handle.hpp"
#undef NO_SET_LOAD_PATH_DEF
#include "gdx/gdx_symbol_utils.hpp"

#include "gdx_random_access.h"

// Optional API (added in gdx core) to fetch symbol data offset.
// Weak symbols don't work reliably in WASM, so disable this feature for Emscripten builds.
// On native platforms with GCC/Clang, we can use weak symbols to check at runtime.
#if defined(__EMSCRIPTEN__)
#define HAS_GDX_SYMBOL_POSITION_CHECK 0
#elif defined(__GNUC__) || defined(__clang__)
extern "C" {
__attribute__((weak)) int gdxSymbolPosition(TGXFileRec_t *gdx, int SyNr, int64_t *Position, int *Dimen, int *RecCnt);
}
#define HAS_GDX_SYMBOL_POSITION_CHECK 0
#else
#define HAS_GDX_SYMBOL_POSITION_CHECK 0
#endif

#include <algorithm>
#include <array>

namespace duckdb {
namespace gdx {

// GDXSymbolDomainValuesCache implementation
bool GDXSymbolDomainValuesCache::HasCachedValues(const std::string &symbol_name) const {
	std::string key = StringUtil::Lower(symbol_name);
	std::lock_guard<std::mutex> lock(mutex);
	return symbol_domain_values.find(key) != symbol_domain_values.end();
}

const std::vector<std::vector<std::string>> *GDXSymbolDomainValuesCache::GetCachedValues(const std::string &symbol_name) const {
	std::string key = StringUtil::Lower(symbol_name);
	std::lock_guard<std::mutex> lock(mutex);
	auto it = symbol_domain_values.find(key);
	if (it == symbol_domain_values.end()) {
		return nullptr;
	}
	return &it->second;
}

void GDXSymbolDomainValuesCache::SetCachedValues(const std::string &symbol_name, std::vector<std::vector<std::string>> values) {
	std::string key = StringUtil::Lower(symbol_name);
	std::lock_guard<std::mutex> lock(mutex);
	symbol_domain_values[key] = std::move(values);
}

namespace {

static std::shared_ptr<GDXMetadataEntry> LoadGDXMetadataInternal(GDXFileRandomAccessProvider &provider,
    UniqueGDXHandle &handle) {
	auto entry = std::make_shared<GDXMetadataEntry>();
	entry->resolved_path = provider.ResolvedPath().empty() ? provider.Location() : provider.ResolvedPath();
	entry->is_remote = provider.IsRemote();

	int symbol_count = 0;
	int uel_count = 0;
	if (!gdxSystemInfo(handle.get(), &symbol_count, &uel_count)) {
		int error_code = gdxGetLastError(handle.get());
		GDXErrorContext context("gdxSystemInfo");
		context.WithFile(entry->resolved_path);
		ThrowGDXError(error_code, context);
	}

	entry->symbols.reserve(static_cast<size_t>(std::max(0, symbol_count)));

	for (int sy_nr = 1; sy_nr <= symbol_count; ++sy_nr) {
		std::array<char, 64> name_buffer {};
		int dimension = 0;
		int type = 0;
		if (!gdxSymbolInfo(handle.get(), sy_nr, name_buffer.data(), &dimension, &type)) {
			int error_code = gdxGetLastError(handle.get());
			GDXErrorContext context("gdxSymbolInfo");
			context.WithFile(entry->resolved_path).WithSymbol(std::string(name_buffer.data())).WithOffset(sy_nr);
			ThrowGDXError(error_code, context);
		}

		std::array<char, 256> description_buffer {};
		int record_count = 0;
		int user_info = 0;
		if (!gdxSymbolInfoX(handle.get(), sy_nr, &record_count, &user_info, description_buffer.data())) {
			int error_code = gdxGetLastError(handle.get());
			GDXErrorContext context("gdxSymbolInfoX");
			context.WithFile(entry->resolved_path).WithSymbol(std::string(name_buffer.data())).WithOffset(sy_nr);
			ThrowGDXError(error_code, context);
		}

		GDXSymbolMetadata metadata;
		metadata.name = std::string(name_buffer.data());
		metadata.type_code = type;
		metadata.dimension_count = dimension < 0 ? 0 : static_cast<uint64_t>(dimension);
		metadata.data_position = 0;
		metadata.record_count = record_count < 0 ? 0 : static_cast<uint64_t>(record_count);
		metadata.description = std::string(description_buffer.data());
		metadata.symbol_index = sy_nr;

		// Try to fetch the byte offset of the symbol data block when the API is available.
#if HAS_GDX_SYMBOL_POSITION_CHECK
		int64_t pos = 0;
		int pos_dim = 0;
		int pos_rec = 0;
		if (gdxSymbolPosition != nullptr && gdxSymbolPosition(handle.get(), sy_nr, &pos, &pos_dim, &pos_rec)) {
			metadata.data_position = pos < 0 ? 0 : static_cast<uint64_t>(pos);
			// Trust record_count from SymbolInfoX by default; if positive from API, prefer that.
			if (pos_rec > 0) {
				metadata.record_count = static_cast<uint64_t>(pos_rec);
			}
		}
#endif

		if (dimension > 0) {
			size_t dim = static_cast<size_t>(dimension);
			std::vector<std::array<char, 256>> domain_buffers(dim);
			std::vector<char *> domain_ptrs;
			domain_ptrs.reserve(dim);
			for (auto &buffer : domain_buffers) {
				buffer.fill('\0');
				domain_ptrs.push_back(buffer.data());
			}

			int domain_result = gdxSymbolGetDomainX(handle.get(), sy_nr, domain_ptrs.data());
			if (domain_result == 0) {
				int error_code = gdxGetLastError(handle.get());
				GDXErrorContext context("gdxSymbolGetDomainX");
				context.WithFile(entry->resolved_path).WithSymbol(metadata.name);
				ThrowGDXError(error_code, context);
			}

			metadata.domain_labels.reserve(domain_ptrs.size());
			for (idx_t dim_idx = 0; dim_idx < domain_ptrs.size(); ++dim_idx) {
				if (domain_result == 1 || domain_ptrs[dim_idx][0] == '\0') {
					metadata.domain_labels.emplace_back("*");
				} else {
					metadata.domain_labels.emplace_back(domain_ptrs[dim_idx]);
				}
			}
		} else {
			metadata.domain_labels.clear();
		}

		entry->symbols.emplace_back(std::move(metadata));
	}

	return entry;
}

} // namespace

std::shared_ptr<GDXMetadataEntry> LoadGDXMetadata(ClientContext &, GDXFileRandomAccessProvider &provider) {
	auto handle = CreateGDXHandle();
	int open_error = 0;
	if (!gdxOpenReadFromRandomAccess(handle.get(), &provider.GetCallbacks(), &open_error)) {
		GDXErrorContext context("gdxOpenReadFromRandomAccess");
		std::string resolved = provider.ResolvedPath().empty() ? provider.Location() : provider.ResolvedPath();
		context.WithFile(resolved);
		ThrowGDXError(open_error, context);
	}

	std::shared_ptr<GDXMetadataEntry> entry;
	try {
		entry = LoadGDXMetadataInternal(provider, handle);
	} catch (...) {
		gdxClose(handle.get());
		throw;
	}

	int close_error = gdxClose(handle.get());
	if (close_error != 0) {
		GDXErrorContext context("gdxClose");
		context.WithFile(entry->resolved_path);
		ThrowGDXError(close_error, context);
	}
	return entry;
}

std::shared_ptr<GDXMetadataEntry> LoadGDXMetadata(ClientContext &context, const std::string &file_or_url) {
	GDXFileRandomAccessProvider provider;
	provider.Initialize(context, file_or_url);
	return LoadGDXMetadata(context, provider);
}

} // namespace gdx
} // namespace duckdb
