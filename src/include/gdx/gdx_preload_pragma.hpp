#pragma once

namespace duckdb {
class ExtensionLoader;

namespace gdx {

void RegisterPreloadPragma(ExtensionLoader &loader);

} // namespace gdx
} // namespace duckdb
