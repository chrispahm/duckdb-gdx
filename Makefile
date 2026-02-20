PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckdb_gdx
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Workaround for GCC linker errors with DuckDB on Linux:
#
# DuckDB defaults to C++11 (CMAKE_CXX_STANDARD=11). In C++11, static constexpr data
# members are NOT inline variables, and GCC may optimize away their out-of-class
# definitions while other translation units still emit references to them, causing
# "undefined reference" errors (e.g. ColumnWriter::PARQUET_DEFINE_VALID).
# Additionally, GCC emits strong (non-COMDAT) symbols for these members, causing
# "multiple definition" errors when the same symbol exists in both libduckdb_static.a
# and extension .a files.
#
# Three fixes are applied via the cmake command line (EXT_FLAGS) since the extension
# CMakeLists.txt is processed too late (DuckDB core targets are already configured):
#
# 1. CMAKE_CXX_STANDARD=17: Makes static constexpr members implicitly inline variables.
#    The compiler emits them as weak/COMDAT symbols in every TU that uses them, and the
#    linker merges duplicates correctly. Fixes both undefined reference and multiple
#    definition issues at their root cause.
#
# 2. DISABLE_UNITY=1: Compiles each .cpp individually instead of merging into unity TUs.
#    Prevents constexpr definitions from being optimized away in unity builds.
#
# 3. --allow-multiple-definition (Linux only): Tells the GNU linker to accept the first
#    definition and silently ignore duplicates, as a safety net for any remaining
#    strong-symbol conflicts.
EXT_FLAGS=-DCMAKE_CXX_STANDARD=17 -DDISABLE_UNITY=1
ifeq ($(shell uname -s),Linux)
EXT_FLAGS += -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-multiple-definition" -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--allow-multiple-definition"
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

wasm_pre_build_step:
	@if [ -n "$(EMSDK)" ] && [ ! -f "$(EMSDK)/upstream/emscripten/cache/sysroot/include/zlib.h" ]; then \
		python3 "$(EMSDK)/upstream/emscripten/embuilder.py" build zlib; \
	fi

# Clean cached duckdb_gdx extension files to avoid stale symbol issues
wasm_clean_cache:
	@echo "Cleaning cached duckdb_gdx extensions..."
	@rm -rf ~/.duckdb/extensions/ 2>/dev/null || true

# Override wasm_eh to clean cache before building
wasm_eh: wasm_clean_cache