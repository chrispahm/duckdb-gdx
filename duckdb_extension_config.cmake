# This configuration allows DuckDB clients to auto-discover the gdx extension

duckdb_extension_load(gdx
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
