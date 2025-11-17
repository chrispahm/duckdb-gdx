import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { AsyncDuckDB, ConsoleLogger, createWorker } from '@duckdb/duckdb-wasm';

import { initializeDuckDBGDX } from '../../scripts/wasm/extension_bundle';
import type { DuckDBGDXDatabase } from '../../scripts/wasm/extension_bundle';

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
  const db = await createDuckDB();
  // set allow_unsigned_extensions to true
  await db.open({
    allowUnsignedExtensions: true,
    query: {
      castTimestampToDate: true
    }
  });
  try {
    await withConnection(db, async (connection) => {

      const extensionDir = path.resolve(__dirname, '../../build/wasm_eh/extension/duckdb_gdx');
      console.log(`Using duckdb_gdx extension from ${extensionDir}`);
      const extensionVirtualDir = 'extensions/duckdb_gdx';
      const extensionPackageVirtualPath = `${extensionVirtualDir}/v1.3.2/wasm_eh/duckdb_gdx.duckdb_extension`;
      const extensionWasmVirtualPath = `${extensionVirtualDir}/v1.3.2/wasm_eh/duckdb_gdx.duckdb_extension.wasm`;
      console.log(extensionWasmVirtualPath);
      let extensionRegistered = false;
      const registerDuckDBGDX = async () => {
        if (extensionRegistered) {
          return;
        }
        const packageBytes = await fs.readFile(path.join(extensionDir, 'duckdb_gdx.duckdb_extension'));
        const packageView = new Uint8Array(packageBytes.buffer, packageBytes.byteOffset, packageBytes.byteLength);
        await db.registerFileBuffer(extensionPackageVirtualPath, packageView);

        const wasmBytes = await fs.readFile(path.join(extensionDir, 'duckdb_gdx.duckdb_extension.wasm'));
        const wasmView = new Uint8Array(wasmBytes.buffer, wasmBytes.byteOffset, wasmBytes.byteLength);
        await db.registerFileBuffer(extensionWasmVirtualPath, wasmView);
        extensionRegistered = true;
      };

      const extensionHost: DuckDBGDXDatabase = {
        async installExtension(name) {
          if (name === 'duckdb_gdx') {
            await registerDuckDBGDX();
            console.log(`Installing duckdb_gdx extension from ${extensionVirtualDir}`);
            await connection.query(`INSTALL ${name} FROM '${extensionVirtualDir}'`)
            // await connection.query(`INSTALL ${name} FROM 'https://humusklimanetz-couch.thuenen.de/datasets/duckdb_gdx'`)
            return;
          }
          await connection.query(`INSTALL ${name}`);
        },
        async loadExtension(name) {
          await connection.query(`LOAD ${name}`);
        }
      };

      await initializeDuckDBGDX(extensionHost, null);

      const transportPath = path.resolve(__dirname, '../data/gdx/transport.gdx');
      const virtualPath = 'test/data/gdx/transport.gdx';
      const gdxBuffer = await fs.readFile(transportPath);
      const gdxBytes = new Uint8Array(gdxBuffer.buffer, gdxBuffer.byteOffset, gdxBuffer.byteLength);
      await db.registerFileBuffer(virtualPath, gdxBytes);

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

  const parameterResult = await connection.query(
        `SELECT SUM(value)::INTEGER AS total FROM read_gdx('${virtualPath}', 'a')`
      );
  const parameterRows = parameterResult.toArray() as ParameterRow[];
      assertEqual(parameterRows.length, 1, 'Expected a single result row when summing transport parameter values');
      assertEqual(parameterRows[0]?.total, 950, 'Unexpected sum for parameter a in transport.gdx');
    });
  } finally {
    await db.terminate();
  }
}

run().catch((error) => {
  console.error(error);
  setTimeout(() => {
    throw error;
  }, 0);
});
