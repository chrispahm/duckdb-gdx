PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Ensure the python bin directory is in PATH so the correct clang-format is found
export PATH := $(shell python3 -c 'import sys, os; print(os.path.dirname(sys.executable))'):$(PATH)

# Configuration of extension
EXT_NAME=duckdb_gdx
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Workaround for GCC 14+ linker errors with DuckDB:
# GCC 14 emits strong (non-COMDAT) symbols for constexpr static data members that have
# out-of-class definitions (C++11 style, still present in DuckDB's types.cpp etc.).
# When these appear in both libduckdb_static.a and extension .a files, the linker
# rejects them as multiple definitions.
#
# Two fixes are needed, BOTH passed via the cmake command line since the extension
# CMakeLists.txt is processed too late (DuckDB core targets are already configured):
#
# 1. DISABLE_UNITY=1: Compiles each .cpp individually instead of merging into unity TUs.
#    Prevents constexpr definitions from being optimized away in unity builds, which would
#    otherwise cause undefined references (e.g. ColumnWriter::PARQUET_DEFINE_VALID).
#
# 2. --allow-multiple-definition: Tells the linker to accept the first definition and
#    silently ignore duplicates, resolving the strong-symbol conflicts from GCC 14.
#    Only applied on Linux/GCC (not WASM, macOS, or Windows) since the flag is GNU ld specific.
EXT_FLAGS=-DDISABLE_UNITY=1
# --allow-multiple-definition is a GNU ld flag. Only add on Linux where GCC/GNU ld is used.
# macOS (Apple ld) and Windows (MSVC) don't need it and don't support this flag.
# WASM builds from Linux also get this flag but wasm-ld (LLD) supports it harmlessly.
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