# This configuration allows DuckDB clients to auto-discover the duckdb_gdx extension

duckdb_extension_load(duckdb_gdx
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
