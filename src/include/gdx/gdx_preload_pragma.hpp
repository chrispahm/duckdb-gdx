#pragma once

namespace duckdb {
class DatabaseInstance;

namespace gdx {

void RegisterPreloadPragma(DatabaseInstance &db);

} // namespace gdx
} // namespace duckdb
