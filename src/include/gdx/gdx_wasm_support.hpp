#pragma once

#include <string>

namespace duckdb {
namespace gdx {

class RandomAccessAdapter;

#ifdef __EMSCRIPTEN__
//! Attempts to initialize the adapter with a WASM-backed random access provider.
//! Returns true when the resource is handled by the WASM provider, false otherwise.
bool InitializeWasmRandomAccess(RandomAccessAdapter &adapter, const std::string &resource);
#endif

} // namespace gdx
} // namespace duckdb
