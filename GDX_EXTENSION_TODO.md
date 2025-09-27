# DuckDB GDX Extension TODO

Last updated: 2025-09-27

## Priority legend

- **[P0]** — Critical path; must be completed before feature can function.
- **[P1]** — High priority; required for full feature parity but can follow P0.
- **[P2]** — Nice-to-have polish or optimizations after core functionality lands.

---

## 1. Extension architecture & scaffolding

- [P0] ✅ (2025-09-27) Audit the `gdx` submodule (branch `gdx_random_access`) to confirm header locations and build artifacts required for both native and WASM builds (key headers under `gdx/src`, public C API in `gdx_random_access.h`, bundled zlib optional with system fallback).
- [P0] ✅ (2025-09-27) Decide final namespace & include layout under `include/gdx/` (using `duckdb::gdx` namespace plus `include/gdx/*.hpp` mirrors for adapter, symbols, read function).
- [P0] ✅ (2025-09-27) Create new source files under `src/`:
  - `src/gdx_extension.cpp` — entry point registering all functions & dependencies.
  - `src/gdx_read_function.cpp` — implementation of `read_gdx` table function (bind/init/scan states).
  - `src/gdx_symbols_function.cpp` — implementation of `gdx_symbols` table function.
  - `src/gdx_random_access_adapter.cpp` — shared glue that wraps DuckDB `FileHandle`/range readers into `gdx_random_access` callbacks.
  - `src/gdx_symbol_utils.cpp` — helpers for type/dimension mapping and column schema construction.
- [P0] ✅ (2025-09-27) Add corresponding public headers in `include/gdx/` for the above C++ files so they can be reused by native/WASM variants.
- [P0] ✅ (2025-09-27) Extend top-level `CMakeLists.txt` and `extension_config.cmake` to compile new sources, export headers, and link the GDX static/shared libs for native builds (links against `gdx-static` with system zlib fallback).
- [P0] Update `duckdb_extension_config.cmake` (if present) to ensure the extension is auto-discovered by DuckDB tooling.
- [P1] Introduce a `docs/ARCHITECTURE.md` capturing the data flow between DuckDB table functions and the GDX random-access API for maintainers.

## 2. Random-access integration & storage wrappers

- [P0] ✅ (2025-09-27) Design a lightweight abstraction (`RandomAccessAdapter`) that satisfies the `gdx_random_access` interface while delegating reads to DuckDB’s `FileSystem` or external providers (see `src/include/gdx/gdx_random_access_adapter.hpp`).
- [P0] ✅ (2025-09-27) Implement read callbacks for `gdx_random_access` that forward requests into DuckDB `FileHandle::Read` and optionally chain to upstream providers (WASM HTTP glue still to be wired once available).
- [P0] ✅ (2025-09-27) Ensure the adapter respects the asynchronous/eager buffering requirements specified by the GDX API (document expected contract in header comments).
- [P0] ✅ (2025-09-27) Provide error translation helpers that map GDX error codes to DuckDB `Exception` types with informative context (file, symbol, byte range).
- [P1] Add configurable read-ahead & buffering knobs (chunk size, max concurrent fetches) exposed via function named parameters.
- [P2] Instrument byte-range access statistics for debugging/telemetry (guarded behind `#ifdef GDX_DEBUG`).

## 3. DuckDB table-function pipeline

- [P0] ✅ (2025-09-27) Define `read_gdx(file_or_url, symbol [, options])` table function:
  - (2025-09-27) Implemented bind/init/scan pipeline using `GDXFileRandomAccessProvider`, re-validating symbol metadata at execution time and mapping GDX types to DuckDB columns (set membership → `BOOLEAN`, parameters → `DOUBLE`, vars/equations → five value columns).
  - (2025-09-27) Added domain flattening with automatic column-name sanitization and NULL handling for GAMS special values via `gdxMapValue`.
  - TODO follow-up: expose sparse/dense metadata flags and support additional value projections beyond the primary numeric fields.
- [P0] ✅ (2025-09-27) Define `gdx_symbols(file_or_url [, options])` table function returning symbol metadata (name, dimensions, type, description, record count, domain labels).
  - (2025-09-27) Added schema helper (`BuildGDXSymbolsSchema`) and symbol type string mapping to support bind implementation.
  - (2025-09-27) Implemented bind/scan pipeline loading symbol names, metadata, and domain labels from GDX via DuckDB file access.
- [P0] For multi-dimensional symbols, flatten coordinates into separate columns and emit additional metadata columns for sparse/dense markers.
- [P0] Map GDX symbol types (set, parameter, variable, equation) to DuckDB logical types using a shared utility (see `LogicalType::DECIMAL`, `STRUCT`, `LIST` as needed).
- [P0] Support user-provided filters (e.g., `dimension_filters`, `value_columns`) via named parameters, even if initially stubbed, to future-proof API.
- [P1] Add pushdown support for DuckDB filters and projections in `read_gdx` bind data to minimize transferred ranges.
- [P1] Handle enumerated domains by resolving domain labels and returning them as DuckDB `ENUM`/`VARCHAR` with caching.
- [P2] Explore parallelization strategies for wide symbols (multi-threaded chunk reads where GDX allows concurrent random access).

## 4. Native backend implementation

- [P0] ✅ (2025-09-27) Integrate GDX native library into the CMake build:
  - Add `add_subdirectory(gdx)` or direct include paths ensuring the correct branch is used.
  - Link against required GDX dependencies (e.g., `zlib`, `iconv`) with platform-specific flags.
- [P0] ✅ (2025-09-27) Implement `GDXFileRandomAccessProvider` that wraps DuckDB’s `FileSystem` API for local files and remote protocols supported natively (S3/HTTP via `httpfs`).
- [P0] ✅ (2025-09-27) Ensure the adapter works with DuckDB’s unified file abstraction (obtain `FileHandle` via `FileSystem::OpenFile` with random access flags).
- [P0] ✅ (2025-09-27) Validate memory-management alignment between DuckDB and GDX (`gdxFree` vs smart pointers) to avoid leaks.
- [P1] Add OS-specific configuration (Windows wide-path handling, macOS rpath updates, Linux `-fPIC`) in CMake to satisfy shared library build.
- [P1] Provide native logging hooks (integrate with DuckDB’s `Profiler` or custom logger) for troubleshooting GDX reads.
- [P2] Benchmark large symbol scans on native builds; document throughput and memory footprint in `docs/perf.md`.

## 5. WASM/backend for DuckDB-Wasm

- [P0] Confirm GDX can compile to WASM: extend Emscripten build configuration (likely under `extension/wasm/` or `tools/wasm/`) to produce a `.a` or `.bc` archive.
- [P0] Implement a WASM-specific `GDXRandomAccessProvider` that issues HTTP range requests via DuckDB’s `httpfs` or custom JS glue (fetch with `Range` headers) without downloading entire files.
- [P0] Wire provider into DuckDB-WASM extension loader (`scripts/wasm/extension_bundle.ts` or equivalent) to register asynchronous byte-range fetching before invoking `gdxOpenReadFromRandomAccess`.
- [P0] Ensure WASM bundling exports necessary C functions (`extern "C"`) so the JS glue can initialize and pass callbacks.
- [P1] Implement client-side caching and chunk reuse (e.g., simple LRU) to minimize redundant range downloads for large `.gdx` files.
- [P1] Provide Node.js polyfill using `fetch`/`http` that also supports range requests for automated tests.
- [P2] Investigate streaming decompression options if `.gdx` supports compression that benefits from incremental decoding.

## 6. DuckDB integration work

- [P0] Register table functions in `gdx_extension.cpp`, including setting `extension_name = "duckdb_gdx"` and adding extension initialization entry point expected by DuckDB (`duckdb_extension_init`).
- [P0] Update `extension_config.cmake` to ensure DuckDB’s extension loader can locate the built binary for native and WASM outputs.
- [P0] Integrate with DuckDB’s extension packaging scripts (`scripts/extension-upload.sh`, `extension-ci-tools/`) to produce distributable artifacts.
- [P0] Add optional pragma or configuration function to preload metadata caches if needed.
- [P1] Ensure compatibility with DuckDB’s load/unload lifecycle (handle `duckdb_extension_unload`).
- [P1] Document and enforce thread-safety in bind/scan states (especially when DuckDB runs parallel scans).
- [P2] Expose extension versioning metadata (e.g., `SELECT gdx_version();`).

## 7. Testing strategy

- [P0] Collect or synthesize minimal `.gdx` fixtures (covering simple set, multi-dimensional parameter, equation) and commit under `test/data/gdx/`.
- [P0] Add SQL tests under `test/sql/` (e.g., `test/sql/gdx/read_gdx.test`) covering:
  - Listing symbols via `gdx_symbols`.
  - Reading dense symbols via `read_gdx`.
  - Handling nonexistent symbol/file errors.
- [P0] Create C++ unit tests under `test/unit/` for utility functions (type mapping, random access adapter edge cases).
- [P1] Add regression tests ensuring vectorized scanning returns identical results regardless of chunk size (vary `PRAGMA vector_size`).
- [P1] Implement WASM integration tests (likely Jest/Playwright) that spin up an HTTP server, load DuckDB-WASM with the extension, and read remote `.gdx` files via range requests.
- [P1] Ensure tests run deterministically in CI by pinning Emscripten and Node versions.
- [P2] Add fuzz/robustness tests for malformed `.gdx` inputs using GDX validation routines.

## 8. CI/CD updates

- [P0] Update GitHub Actions workflows (under `ci/` or `.github/workflows/`) to build the extension for Linux/macOS/Windows using the DuckDB extension template’s `make` targets.
- [P0] Integrate WASM build job (Emscripten) producing the extension bundle and artifacts for browser/Node usage.
- [P0] Add test jobs executing native SQL/unit tests and WASM integration tests (Node.js or headless browser).
- [P1] Publish built artifacts to GitHub Releases or DuckDB’s extension S3 bucket using existing `extension-upload.sh` scripts when tags are pushed.
- [P1] Cache build dependencies (CMake, Emscripten, Node modules) to reduce CI latency.
- [P2] Add optional nightly benchmark job to track performance regressions on representative `.gdx` workloads.

## 9. Documentation, samples & developer experience

- [P0] Update root `README.md` with quick-start instructions:
  - Loading the extension in DuckDB CLI and executing `SELECT * FROM read_gdx('data.gdx', 'symbol');`.
  - Listing `gdx_symbols('data.gdx');` output.
- [P0] Add a `docs/usage.md` detailing function parameters, supported symbol types, and performance tips.
- [P1] Provide a JS example under `examples/js/wasm-read-gdx.js` showing DuckDB-WASM usage against an HTTP-hosted `.gdx` file with range fetching.
- [P1] Integrate example into documentation site or `docs/README.md` with screenshots/GIFs if feasible.
- [P1] Document build steps in `docs/BUILDING.md` for native and WASM developers (toolchains, env vars, troubleshooting).
- [P2] Publish a blog-style walkthrough in `docs/tutorial.md` covering end-to-end scenario (data exploration, analytics, export).

## 10. Post-MVP polish & observability

- [P1] Implement basic statistics caching (persist symbol metadata between invocations) to speed up repeated queries.
- [P1] Surface instrumentation via `PRAGMA show_profile` or custom table function (bytes read, fetch count, cache hit rate).
- [P2] Add optional concurrency controls to throttle remote range requests based on browser resource constraints.
- [P2] Evaluate security implications for remote fetches (CORS, signed URLs) and document best practices.

---

## Cross-cutting considerations & sequencing

1. **Start with P0 tasks in sections 1–4** to get native vectorized scanning working end-to-end against local `.gdx` files.
2. **Tackle WASM P0 tasks** once native path is validated, reusing the shared random-access abstraction.
3. Layer on **testing & CI** to prevent regressions, then expand docs/examples.
4. Reserve P2 polish items for after MVP acceptance or as follow-up issues.
