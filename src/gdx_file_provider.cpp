#include "gdx/gdx_file_provider.hpp"

// #ifdef __EMSCRIPTEN__
#include "gdx/gdx_wasm_support.hpp"
// #endif

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <memory>
#include <utility>

namespace duckdb {
namespace gdx {

void GDXFileRandomAccessProvider::Reset() {
	adapter.Reset();
	requested_path.clear();
	resolved_path.clear();
	is_remote = false;
}

void GDXFileRandomAccessProvider::Initialize(ClientContext &context, const std::string &file_or_url) {
	Reset();
	requested_path = file_or_url;

#ifdef __EMSCRIPTEN__
	if (InitializeWasmRandomAccess(adapter, file_or_url)) {
		resolved_path = file_or_url;
		is_remote = true;
		return;
	}
#endif

	auto &fs = FileSystem::GetFileSystem(context);
	is_remote = FileSystem::IsRemoteFile(file_or_url);

	std::string open_path = file_or_url;
	if (!is_remote) {
		try {
			resolved_path = fs.NormalizeAbsolutePath(file_or_url);
			open_path = resolved_path;
		} catch (const Exception &) {
			resolved_path = file_or_url;
		}
	} else {
		resolved_path = file_or_url;
	}

	auto flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS |
	             FileFlags::FILE_FLAGS_MULTI_CLIENT_ACCESS;

	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(open_path, flags);
	} catch (const Exception &ex) {
		throw IOException(StringUtil::Format("Unable to open '%s' for GDX random access: %s", file_or_url, ex.what()));
	}
	if (!handle) {
		throw IOException(StringUtil::Format("Unable to open '%s' for GDX random access", file_or_url));
	}

	if (!handle->CanSeek()) {
		throw IOException(StringUtil::Format(
		    "The file system backing '%s' does not support random access, which is required by GDX", file_or_url));
	}

	if (resolved_path.empty()) {
		resolved_path = handle->GetPath();
	}

	adapter.InitializeFromFileHandle(std::move(handle));
}

} // namespace gdx
} // namespace duckdb
