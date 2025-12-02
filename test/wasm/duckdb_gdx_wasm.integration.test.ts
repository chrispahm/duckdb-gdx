import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { AsyncDuckDB, ConsoleLogger, createWorker } from '@duckdb/duckdb-wasm';

type DuckDBConnection = Awaited<ReturnType<AsyncDuckDB['connect']>>;

type SymbolRow = {
  symbol_name: string;
  record_count: number;
};

type ParameterRow = {
  total: number;
};

function assertEqual<T>(actual: T, expected: T, message?: string): void {
  if (actual !== expected) {
    throw new Error(message ?? `Assertion failed: expected ${String(expected)}, received ${String(actual)}`);
  }
}

function assertDeepEqual(actual: unknown, expected: unknown, message?: string): void {
  const actualJson = JSON.stringify(actual);
  const expectedJson = JSON.stringify(expected);
  if (actualJson !== expectedJson) {
    throw new Error(message ?? `Assertion failed: expected ${expectedJson}, received ${actualJson}`);
  }
}

const EXTENSION_SERVER_PORT = 19876;

/**
 * Start a simple HTTP server to serve the extension files.
 * DuckDB-WASM expects extensions at: {repository}/v{version}/{platform}/{name}.duckdb_extension.wasm
 */
async function startExtensionServer(): Promise<http.Server> {
  // Use the repository directory which has the correct structure (v1.3.2/wasm_eh/)
  const repositoryDir = path.resolve(__dirname, '../../build/wasm_eh/repository');
  
  console.log(`Repository dir: ${repositoryDir}`);
  
  const server = http.createServer(async (req, res) => {
    // CORS headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
    
    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
      return;
    }
    
    // Serve extension files from the repository directory
    // URL pattern: /v1.3.2/wasm_eh/duckdb_gdx.duckdb_extension.wasm
    const urlPath = req.url || '';
    console.log(`HTTP request: ${urlPath}`);
    
    try {
      const filePath = path.join(repositoryDir, urlPath);
      console.log(`Serving file: ${filePath}`);
      const content = await fs.readFile(filePath);
      console.log(`Serving ${content.length} bytes`);
      const contentType = filePath.endsWith('.wasm') ? 'application/wasm' : 'application/octet-stream';
      res.setHeader('Content-Type', contentType);
      res.writeHead(200);
      res.end(content);
    } catch (error) {
      console.log(`File not found: ${urlPath}`, error);
      res.writeHead(404);
      res.end('Not found');
    }
  });
  
  return new Promise((resolve) => {
    server.listen(EXTENSION_SERVER_PORT, () => {
      resolve(server);
    });
  });
}

async function createDuckDB(): Promise<AsyncDuckDB> {
  const workerPath = path.resolve(__dirname, 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-node-eh.worker.cjs');
  const wasmPath = path.resolve(__dirname, 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-eh.wasm');
  const workerFileUrl = pathToFileURL(workerPath).href;

  const originalFetch = globalThis.fetch;
  const originalCreateObjectURL = globalThis.URL.createObjectURL;

  let shouldOverrideCreateObjectURL = false;

  const patchedFetch = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
    if (url === workerFileUrl) {
      shouldOverrideCreateObjectURL = true;
      const contents = await fs.readFile(fileURLToPath(url));
      const scriptSource = contents.toString('utf8');
      return new Response(scriptSource, {
        status: 200,
        headers: { 'Content-Type': 'application/javascript; charset=utf-8' }
      });
    }
    return originalFetch(input as RequestInfo, init);
  };

  globalThis.fetch = patchedFetch as typeof fetch;
  globalThis.URL.createObjectURL = ((blob: Blob) => {
    if (shouldOverrideCreateObjectURL) {
      return workerFileUrl;
    }
    return originalCreateObjectURL(blob);
  }) as typeof URL.createObjectURL;

  try {
    const worker = await createWorker(workerFileUrl);
    const db = new AsyncDuckDB(new ConsoleLogger(), worker);
    await db.instantiate(wasmPath);
    return db;
  } finally {
    shouldOverrideCreateObjectURL = false;
    globalThis.URL.createObjectURL = originalCreateObjectURL;
    globalThis.fetch = originalFetch;
  }
}

async function withConnection<T>(db: AsyncDuckDB, action: (connection: DuckDBConnection) => Promise<T>): Promise<T> {
  const connection = await db.connect();
  try {
    return await action(connection);
  } finally {
    await connection.close();
  }
}

async function run(): Promise<void> {
  // Start HTTP server to serve extension files
  const extensionServer = await startExtensionServer();
  console.log(`Extension server started on port ${EXTENSION_SERVER_PORT}`);
  
  try {
    const db = await createDuckDB();
    // Set allow_unsigned_extensions to true
    await db.open({
      allowUnsignedExtensions: true,
      query: {
        castTimestampToDate: true
      }
    });
    
    try {
      await withConnection(db, async (connection) => {
        // Set custom extension repository to our local server
        console.log('Setting custom_extension_repository...');
        await connection.query(`SET custom_extension_repository = 'http://localhost:${EXTENSION_SERVER_PORT}'`);
        
        // Load the extension (INSTALL is a no-op in DuckDB-WASM)
        console.log('Loading duckdb_gdx extension...');
        await connection.query('LOAD duckdb_gdx');
        console.log('Extension loaded successfully!');

        // Register test GDX file
        const transportPath = path.resolve(__dirname, '../data/gdx/transport.gdx');
        const virtualPath = 'test/data/gdx/transport.gdx';
        const gdxBuffer = await fs.readFile(transportPath);
        const gdxBytes = new Uint8Array(gdxBuffer.buffer, gdxBuffer.byteOffset, gdxBuffer.byteLength);
        await db.registerFileBuffer(virtualPath, gdxBytes);

        // Test gdx_symbols function
        const symbolsResult = await connection.query(
          `SELECT symbol_name, record_count::INTEGER AS record_count
           FROM gdx_symbols('${virtualPath}')
           WHERE symbol_name IN ('d', 'i', 'j')
           ORDER BY symbol_name`
        );
        const symbolRows = symbolsResult.toArray() as SymbolRow[];
        assertDeepEqual(symbolRows, [
          { symbol_name: 'd', record_count: 6 },
          { symbol_name: 'i', record_count: 2 },
          { symbol_name: 'j', record_count: 3 }
        ]);
        console.log('gdx_symbols test passed!');

        // Test read_gdx function
        const parameterResult = await connection.query(
          `SELECT SUM(value)::INTEGER AS total FROM read_gdx('${virtualPath}', 'a')`
        );
        const parameterRows = parameterResult.toArray() as ParameterRow[];
        assertEqual(parameterRows.length, 1, 'Expected a single result row when summing transport parameter values');
        assertEqual(parameterRows[0]?.total, 950, 'Unexpected sum for parameter a in transport.gdx');
        console.log('read_gdx test passed!');
        
        console.log('All tests passed!');
      });
    } finally {
      await db.terminate();
    }
  } finally {
    extensionServer.close();
  }
}

run().catch((error) => {
  console.error(error);
  setTimeout(() => {
    throw error;
  }, 0);
});
