# Python Integration Tests

This directory contains Python integration coverage for the GDX extension.

## Prerequisites

* A compiled `gdx.duckdb_extension` artifact. By default the tests look under `build/**/gdx.duckdb_extension`. To point to a different location, set the `DUCKDB_GDX_EXTENSION_PATH` environment variable before running the tests.
* `uv` is used to create an isolated Python environment and install dependencies declared in `pyproject.toml`.

## Running the tests

From the repository root:

```bash
cd test/integration/python
uv run pytest tests
```

If you would like to exercise a single test module:

```bash
cd test/integration/python
uv run pytest tests/test_gdx_transport.py
```

The suite skips automatically if the sample `transport.gdx` file or the compiled extension is not available.
