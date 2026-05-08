# DuckDB GDX Extension

The DuckDB GDX extension exposes [GAMS](https://gams.com/) Data eXchange (`.gdx`) data sets through DuckDB table functions. It bundles the [native GDX
runtime](https://github.com/GAMS-dev/gdx) and adds a small metadata cache so repeated scans stay fast across CLI, Python, and JS/WASM clients.

Used in the [VSCode GDX extension](https://github.com/chrispahm/gdx-viewer) for viewing GDX files.

## Quick start

```sql
LOAD 'gdx';
PRAGMA gdx_preload('test/data/gdx/transport.gdx', force_reload=true);
SELECT symbol_name, symbol_type, record_count
FROM gdx_symbols('test/data/gdx/transport.gdx');

SELECT *
FROM read_gdx('test/data/gdx/transport.gdx', 'd')
LIMIT 5;
```

The `gdx_symbols` table function lists every symbol together with its GAMS type, dimensionality, and record count. Use
`read_gdx` to materialize a specific symbol; named parameters let you restrict the exported columns:

```sql
SELECT SUM(value)
FROM read_gdx(
  'test/data/gdx/transport.gdx',
  'a',
  value_columns => ['value']
);
```

## Building the extension

```sh
make debug       # or make release
make test_debug  # runs extension SQL tests and metadata unit tests
```

The CI workflow mirrors these commands on Linux, macOS, Windows, and the WASM toolchain. Generated binaries land under
`build/<config>/extension/gdx/`.

## Documentation

- [`docs/usage.md`](docs/usage.md) — table function signatures, parameters, and performance hints
- [`test/sql/gdx/read_gdx.test`](test/sql/gdx/read_gdx.test) — end-to-end regression coverage

For implementation details, see the in-source headers under `src/include/gdx/` and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Examples

Check out the [Observable Notebook](https://observablehq.com/@chrispahm/gdx-dateien-im-browser-lesen) demonstrating how to use the extension to read GDX files in a browser environment.
