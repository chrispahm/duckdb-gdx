# GDX Test Fixtures

The regression tests rely on the classic GAMS `transport.gdx` sample bundled in this directory. The file contains the
following symbol types, giving coverage for the scenarios exercised by the DuckDB integration tests:

- `i`, `j`: one-dimensional sets
- `d`, `c`: two-dimensional parameters
- `a`, `b`, `f`: one-dimensional parameters
- `x`, `z`: variables
- `cost`, `supply`, `demand`: equations

The `test/sql/gdx/read_gdx.test` script loads the file through `gdx_preload`, enumerates symbols with
`gdx_symbols`, and scans representative parameters to validate the table function pipeline.
