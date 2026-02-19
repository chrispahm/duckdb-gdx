# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
set(DUCKDB_GDX_EXTENSION_VERSION "v0.1.0" CACHE STRING "duckdb_gdx extension metadata version")

duckdb_extension_load(duckdb_gdx
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    EXTENSION_VERSION ${DUCKDB_GDX_EXTENSION_VERSION}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)