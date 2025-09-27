#pragma once

#include "gdx_random_access.h"
#include "gdxcwrap.h"

#include <memory>
#include <string>

namespace duckdb {
namespace gdx {

struct GDXHandleDeleter {
	void operator()(TGXFileRec_t *handle) const noexcept;
};

using UniqueGDXHandle = std::unique_ptr<TGXFileRec_t, GDXHandleDeleter>;

//! Create a new GDX handle using the C API. When @p system_directory is empty the default lookup path is used.
UniqueGDXHandle CreateGDXHandle(const std::string &system_directory = "");

} // namespace gdx
} // namespace duckdb
