PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckdb_gdx
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

wasm_pre_build_step:
	@if [ -n "$(EMSDK)" ] && [ ! -f "$(EMSDK)/upstream/emscripten/cache/sysroot/include/zlib.h" ]; then \
		python3 "$(EMSDK)/upstream/emscripten/embuilder.py" build zlib; \
	fi

# Clean cached duckdb_gdx extension files to avoid stale symbol issues
wasm_clean_cache:
	@echo "Cleaning cached duckdb_gdx extensions..."
	@rm -rf ~/.duckdb/extensions/*gdx* 2>/dev/null || true

# Override wasm_eh to clean cache before building
wasm_eh: wasm_clean_cache