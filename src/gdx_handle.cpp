#include "gdx/gdx_handle.hpp"

#include "gdx/gdx_error.hpp"

#include "duckdb/common/exception.hpp"

#include <array>

namespace duckdb {
namespace gdx {

void GDXHandleDeleter::operator()(TGXFileRec_t *handle) const noexcept {
	if (!handle) {
		return;
	}
	auto local = handle;
	gdxFree(&local);
}

UniqueGDXHandle CreateGDXHandle(const std::string &system_directory) {
	TGXFileRec_t *raw_handle = nullptr;
	std::array<char, 512> message_buffer {};
	int rc = 0;
	if (system_directory.empty()) {
		rc = gdxCreate(&raw_handle, message_buffer.data(), static_cast<int>(message_buffer.size()));
	} else {
		rc = gdxCreateD(&raw_handle, system_directory.c_str(), message_buffer.data(),
		                static_cast<int>(message_buffer.size()));
	}

	if (rc == 0 || !raw_handle) {
		std::string message = message_buffer.data();
		if (message.empty()) {
			message = "(no error message provided by GDX)";
		}
		throw IOException("Failed to create GDX handle: " + message);
	}

	return UniqueGDXHandle(raw_handle);
}

} // namespace gdx
} // namespace duckdb
