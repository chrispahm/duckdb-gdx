# /// script
# requires-python = ">=3.10"
# dependencies = ["duckdb==1.3.2"]
# ///

import time
import duckdb

gdx_path = (
    "/Users/pahmeyer/Documents/GitHub.nosync/capri-course/model250512114725/dat/fao/FAO_trade_matrix_1986_2021.gdx"
)
extension_path = (
    "/Users/pahmeyer/Documents/GitHub.nosync/duckdb-gdx/build/release/extension/duckdb_gdx/duckdb_gdx.duckdb_extension"
)

conn = duckdb.connect(config={"allow_unsigned_extensions": True})
conn.execute(f"LOAD '{extension_path}'")

# query = f"""
#     SELECT * FROM read_gdx('{gdx_path}', 'p_faoTradeMatrix')
# """
query = f"""
COPY (
    SELECT *
    FROM read_gdx('{gdx_path}', 'p_faoTradeMatrix')
)
TO 'p_faoTradeMatrix.parquet'
(FORMAT PARQUET, OVERWRITE TRUE);
"""
# WHERE dim_1 = '1' AND dim_2 = '2' AND dim_3 = '561' AND dim_4 = 'Import'
start = time.perf_counter()
result = conn.execute(query).fetchall()
elapsed = (time.perf_counter() - start) * 1000

print(f"Query took {elapsed:.2f} ms")
print(f"Rows: {len(result)}")
# for row in result:
#    print(row)

conn.close()
