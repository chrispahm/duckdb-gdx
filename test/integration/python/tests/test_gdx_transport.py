from __future__ import annotations

import os
from pathlib import Path
from typing import Generator, Iterable

import duckdb
import pytest

REPO_ROOT = Path(__file__).resolve().parents[4]
TRANSPORT_GDX = REPO_ROOT / "test" / "data" / "gdx" / "transport.gdx"


EXTENSION_FILENAMES = ("gdx.duckdb_extension",)
ASAN_MARKERS = (b"libclang_rt.asan", b"__asan_init")


def _iter_extension_candidates(root: Path) -> Iterable[Path]:
    build_root = root / "build"
    if not build_root.exists():
        return []
    patterns = [f"**/{name}" for name in EXTENSION_FILENAMES]
    candidates: list[Path] = []
    for pattern in patterns:
        candidates.extend(build_root.glob(pattern))
    return candidates


def _requires_address_sanitizer(path: Path) -> bool:
    """Return True when the binary contains AddressSanitizer hooks."""
    try:
        with path.open("rb") as candidate:
            while True:
                chunk = candidate.read(64 * 1024)
                if not chunk:
                    break
                if any(marker in chunk for marker in ASAN_MARKERS):
                    return True
    except OSError as exc:  # propagate missing/corrupted artifacts
        raise FileNotFoundError(f"Unable to read compiled artifact at '{path}'") from exc
    return False


def locate_extension_path() -> Path:
    """Locate the compiled gdx.duckdb_extension artifact that can be loaded in-process."""
    override = os.environ.get("DUCKDB_GDX_EXTENSION_PATH")
    if override:
        candidate = Path(override).expanduser().resolve()
        if candidate.is_file():
            if _requires_address_sanitizer(candidate):
                raise FileNotFoundError(
                    "DUCKDB_GDX_EXTENSION_PATH points to an AddressSanitizer-instrumented binary, "
                    "which cannot be loaded into this Python process. "
                    "Rebuild the extension with sanitizers disabled or point to an unsanitized artifact."
                )
            return candidate
        raise FileNotFoundError(f"DUCKDB_GDX_EXTENSION_PATH is set to '{candidate}', but the file does not exist.")

    def _is_supported_target(path: Path) -> bool:
        return not any("wasm" in part for part in path.parts)

    candidates = [
        candidate
        for candidate in _iter_extension_candidates(REPO_ROOT)
        if candidate.is_file() and _is_supported_target(candidate)
    ]
    if not candidates:
        raise FileNotFoundError(
            "Unable to locate gdx.duckdb_extension under 'build/'. "
            "Set DUCKDB_GDX_EXTENSION_PATH to point to a compiled artifact."
        )

    usable_candidates: list[Path] = []
    for candidate in candidates:
        try:
            if _requires_address_sanitizer(candidate):
                continue
        except FileNotFoundError:
            continue
        usable_candidates.append(candidate)

    if not usable_candidates:
        raise FileNotFoundError(
            "All detected gdx.duckdb_extension artifacts require AddressSanitizer. "
            "Reconfigure your build (e.g., cmake -DENABLE_SANITIZER=OFF -DENABLE_UBSAN=OFF) "
            "or provide an unsanitized artifact via DUCKDB_GDX_EXTENSION_PATH."
        )

    try:
        return max(usable_candidates, key=lambda path: path.stat().st_mtime)
    except OSError as exc:
        raise FileNotFoundError("Unable to stat gdx.duckdb_extension candidates") from exc


@pytest.fixture(scope="module")
def duckdb_connection() -> Generator[duckdb.DuckDBPyConnection, None, None]:
    if not TRANSPORT_GDX.exists():
        pytest.skip(f"Sample GDX data set not found at {TRANSPORT_GDX}")

    try:
        extension_path = locate_extension_path()
    except FileNotFoundError as exc:
        pytest.skip(str(exc))

    connection = duckdb.connect(database=":memory:", config={"allow_unsigned_extensions": "true"})
    connection.load_extension(str(extension_path))
    connection.execute("PRAGMA threads=1")

    yield connection

    connection.close()


def test_transport_gdx_metadata_and_data(duckdb_connection: duckdb.DuckDBPyConnection) -> None:
    conn = duckdb_connection

    loaded_row = conn.execute("""
        SELECT COUNT(*)
        FROM duckdb_extensions()
        WHERE extension_name = 'gdx' AND loaded
        """).fetchone()
    assert loaded_row is not None
    assert loaded_row[0] >= 1

    symbol_rows = conn.execute(
        """
        SELECT symbol_name, record_count
        FROM gdx_symbols(?)
        WHERE symbol_name IN ('d', 'i', 'j')
        ORDER BY symbol_name
        """,
        [str(TRANSPORT_GDX)],
    ).fetchall()
    assert symbol_rows == [("d", 6), ("i", 2), ("j", 3)]

    demand_row = conn.execute(
        "SELECT COUNT(*) FROM read_gdx(?, ?) WHERE is_member",
        [str(TRANSPORT_GDX), "i"],
    ).fetchone()
    assert demand_row is not None
    assert demand_row[0] == 2

    supply_row = conn.execute(
        "SELECT COUNT(*) FROM read_gdx(?, ?) WHERE is_member",
        [str(TRANSPORT_GDX), "j"],
    ).fetchone()
    assert supply_row is not None
    assert supply_row[0] == 3

    cost_row = conn.execute(
        "SELECT SUM(value)::INTEGER FROM read_gdx(?, ?)",
        [str(TRANSPORT_GDX), "a"],
    ).fetchone()
    assert cost_row is not None
    assert cost_row[0] == 950

    routes_row = conn.execute(
        """
        SELECT COUNT(*)
        FROM read_gdx(?, ?, dimension_filters => map(['i'], ['seattle']))
        """,
        [str(TRANSPORT_GDX), "d"],
    ).fetchone()
    assert routes_row is not None
    assert routes_row[0] == 3

    is_member_row = conn.execute(
        """
        SELECT COUNT(*)
        FROM read_gdx(?, ?, dimension_filters => map(['i'], ['seattle']))
        WHERE is_member
        """,
        [str(TRANSPORT_GDX), "i"],
    ).fetchone()
    assert is_member_row is not None
    assert is_member_row[0] == 1

    filtered_rows = conn.execute(
        """
        SELECT level AS value, marginal
        FROM read_gdx(?, ?)
        WHERE i = 'seattle' AND j LIKE 'new-%'
        """,
        [str(TRANSPORT_GDX), "x"],
    ).fetchall()
    print(filtered_rows)
    assert filtered_rows == [(50.0, 0.0)]
    print("DuckDB GDX transport tests passed.")


if __name__ == "__main__":
    import sys

    sys.exit(pytest.main(["-v", "-s", __file__]))
