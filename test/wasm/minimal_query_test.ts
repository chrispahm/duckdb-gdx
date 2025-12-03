import http from 'node:http';
import path from 'node:path';
import fs from 'node:fs/promises';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { AsyncDuckDB, ConsoleLogger, createWorker } from '@duckdb/duckdb-wasm';

const PORT = 19877;
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repositoryDir = path.resolve(__dirname, '../../build/wasm_eh/repository');

const server = http.createServer(async (req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  const urlPath = req.url || '';
  try {
    const filePath = path.join(repositoryDir, urlPath);
    const content = await fs.readFile(filePath);
    res.setHeader('Content-Type', 'application/wasm');
    res.writeHead(200);
    res.end(content);
  } catch {
    res.writeHead(404);
    res.end('Not found');
  }
});

server.listen(PORT, async () => {
  const workerPath = path.resolve(__dirname, 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-node-eh.worker.cjs');
  const wasmPath = path.resolve(__dirname, 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-eh.wasm');
  const workerFileUrl = pathToFileURL(workerPath).href;

  const originalFetch = globalThis.fetch;
  const originalCreateObjectURL = globalThis.URL.createObjectURL;
  let shouldOverride = false;

  globalThis.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : (input as Request).url;
    if (url === workerFileUrl) {
      shouldOverride = true;
      const contents = await fs.readFile(fileURLToPath(url));
      return new Response(contents.toString('utf8'), { status: 200, headers: { 'Content-Type': 'application/javascript' } });
    }
    return originalFetch(input, init);
  };
  globalThis.URL.createObjectURL = (blob: Blob) => shouldOverride ? workerFileUrl : originalCreateObjectURL(blob);

  const worker = await createWorker(workerFileUrl);
  const db = new AsyncDuckDB(new ConsoleLogger(), worker);
  await db.instantiate(wasmPath);
  await db.open({ allowUnsignedExtensions: true });

  const conn = await db.connect();

  await conn.query(`SET custom_extension_repository = 'http://localhost:${PORT}'`);
  await conn.query('LOAD duckdb_gdx');

  // Register the GDX file
  const gdxPath = '/Users/pahmeyer/Documents/GitHub.nosync/capri-course/model250512114725/dat/fao/FAO_trade_matrix_1986_2021.gdx';
  const gdxBuffer = await fs.readFile(gdxPath);
  const gdxBytes = new Uint8Array(gdxBuffer.buffer, gdxBuffer.byteOffset, gdxBuffer.byteLength);
  await db.registerFileBuffer('fao_trade.gdx', gdxBytes);

  // Optimized query using dimension_filters parameter (filter pushdown to GDX level)
  const optimizedQuery = `
    SELECT * FROM read_gdx('fao_trade.gdx', 'p_faoTradeMatrix',
      dimension_filters => map(['dim_1', 'dim_2', 'dim_3', 'dim_4'], ['1', '2', '561', 'Import'])
    )
  `;

  // NOT optimized query using WHERE clause (full scan + post-filter)
  const unoptimizedQuery = `
    SELECT * FROM read_gdx('fao_trade.gdx', 'p_faoTradeMatrix')
    WHERE dim_1 = '1' AND dim_2 = '2' AND dim_3 = '561' AND dim_4 = 'Import'
  `;

  console.log('=== Running OPTIMIZED query (dimension_filters parameter) ===');
  const startOptimized = performance.now();
  const resultOptimized = await conn.query(optimizedQuery);
  const elapsedOptimized = performance.now() - startOptimized;

  console.log(`Optimized query took ${elapsedOptimized.toFixed(2)} ms`);
  console.log(`Rows: ${resultOptimized.numRows}`);
  console.log(resultOptimized.toArray());

  console.log('\n=== Running NOT OPTIMIZED query (WHERE clause) ===');
  const startUnoptimized = performance.now();
  const resultUnoptimized = await conn.query(unoptimizedQuery);
  const elapsedUnoptimized = performance.now() - startUnoptimized;

  console.log(`Unoptimized query took ${elapsedUnoptimized.toFixed(2)} ms`);
  console.log(`Rows: ${resultUnoptimized.numRows}`);
  console.log(resultUnoptimized.toArray());

  console.log('\n=== Summary ===');
  console.log(`Optimized (dimension_filters): ${elapsedOptimized.toFixed(2)} ms`);
  console.log(`Unoptimized (WHERE clause):    ${elapsedUnoptimized.toFixed(2)} ms`);
  if (elapsedUnoptimized > 0) {
    const speedup = elapsedUnoptimized / elapsedOptimized;
    console.log(`Speedup: ${speedup.toFixed(2)}x`);
  }

  await conn.close();
  await db.terminate();
  server.close();
});
