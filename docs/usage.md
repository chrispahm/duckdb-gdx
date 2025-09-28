# Using the DuckDB GDX Extension

The extension surfaces the GAMS `.gdx` format through two table functions and a supporting pragma. All entry points live
in the `duckdb::gdx` namespace on the C++ side and are available once the extension is loaded.

## Loading and preloading metadata

```sql
LOAD 'duckdb_gdx';
PRAGMA gdx_preload('s3://bucket/model.gdx', force_reload=true);
```

`PRAGMA gdx_preload` eagerly loads symbol metadata into the shared cache. Parameters:

| Name          | Type     | Default | Description |
|---------------|----------|---------|-------------|
| `file_or_url` | `VARCHAR`| —       | Path or URL pointing to the `.gdx` file. Remote paths delegate to DuckDB's `httpfs` or the active VFS. |
| `force_reload`| `BOOLEAN`| `false` | Drop any cached metadata before reloading from disk. |
| `symbol`      | `VARCHAR`| `NULL`  | Optional symbol name to check for existence. The pragma throws if the symbol cannot be found. |

The metadata cache is shared by both table functions. Preloading is optional—`read_gdx` and `gdx_symbols` will populate
entries on demand when the pragma is not invoked.

## Listing symbols

```sql
SELECT *
FROM gdx_symbols('model.gdx')
ORDER BY symbol_name;
```

`gdx_symbols` exposes one row per symbol with the following schema:

| Column            | Type        | Description |
|-------------------|-------------|-------------|
| `symbol_name`     | `VARCHAR`   | Original case-sensitive GAMS symbol name. |
| `symbol_type`     | `VARCHAR`   | Human-readable type (`Set`, `Parameter`, `Variable`, `Equation`, `Alias`). |
| `dimension_count` | `UBIGINT`   | Number of domain indices. |
| `record_count`    | `UBIGINT`   | Cardinality reported by GDX. |
| `description`     | `VARCHAR`   | Symbol description stored in the GAMS model. |
| `domain_labels`   | `VARCHAR[]` | Array with one entry per domain index (`'*'` when unspecified). |

## Reading symbols

```sql
SELECT origin, destination, value
FROM read_gdx(
  'model.gdx',
  'transport_costs',
  dimension_filters => map(['origin'], ['seattle']),
  value_columns     => ['value']
);
```

The `read_gdx` table function materializes GDX symbol records. The output schema is composed dynamically:

- One column per domain index (`VARCHAR`). Generated names fall back to `dim_<N>` when the symbol omits labels.
- Optional metadata columns for sparse/dense indicators on multi-dimensional symbols: `is_sparse_break`, `is_dense_run`.
- Value columns depend on the symbol type:
  - Sets: `is_member` (`BOOLEAN`).
  - Parameters: `value` (`DOUBLE`), `value_text` (`VARCHAR`).
  - Variables/Equations: `value`, `lower`, `upper`, `marginal`, `scale` (`DOUBLE`).

### Named parameters

| Parameter            | Type              | Description |
|----------------------|-------------------|-------------|
| `dimension_filters`  | `MAP<VARCHAR,VARCHAR>` | Restrict emitted rows by matching domain values. Keys are case-insensitive; duplicate keys raise an error. |
| `value_columns`      | `LIST<VARCHAR>`   | Subset of value columns to project (e.g., `['value']`). |

### Error handling

`read_gdx` surfaces detailed exception messages including the originating file, symbol name, and failing GDX API call.
Special values flagged by GDX (`NA`, `LO`, `UP`, etc.) are mapped to `NULL` in numeric outputs to preserve DuckDB's
semantics. The original special value codes stay accessible through auxiliary value columns (`value_text`).

## Performance tips

- **Preload metadata** with `PRAGMA gdx_preload` when a workload repeatedly scans the same `.gdx` file. The cache keeps
  symbol descriptors and resolves case-insensitive lookups without reopening the file.
- **Filter early** via `dimension_filters` and `value_columns`. Because the filter set is compiled into the scan state,
  unnecessary coordinates are skipped before materializing vectors.
- **Remote files** rely on DuckDB's `httpfs` or custom VFS providers. When using WASM, pair `initializeDuckDBGDX` from
  `scripts/wasm/extension_bundle.ts` with header configuration to pass authentication tokens for HTTP range requests.
  A full JavaScript walkthrough that targets a hosted dataset lives in
  `examples/js/wasm-read-gdx.js`.
- **Threading**: the current implementation processes a single stream per table function invocation (`MaxThreads = 1`).
  For highly dimensional symbols, prefer batching work across independent DuckDB queries.

## Supported symbol types

The extension supports all core GAMS symbol types required by the transport test fixture:

| GAMS type  | Support status | Notes |
|------------|----------------|-------|
| Set        | ✅ | Exposes membership as `BOOLEAN`. |
| Parameter  | ✅ | Returns numeric value and companion text column when provided. |
| Variable   | ✅ | Emits five value columns (`value`, `lower`, `upper`, `marginal`, `scale`). |
| Equation   | ✅ | Same column layout as variables. |
| Alias      | ⚠️ | Rejected at bind time—aliases can be resolved through their target symbols. |

Future releases will add optional ENUM mapping for domain labels and metadata about sparse/dense runs.
