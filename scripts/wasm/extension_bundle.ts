/**
 * Helper utilities for bundling the gdx extension with duckdb-wasm.
 *
 * The C++ side accepts optional HTTP headers that are appended to every
 * range request performed by the WASM random access backend. The helpers
 * below expose a minimal TypeScript surface for configuring those headers
 * and ensuring the extension is available inside a DuckDB-Wasm instance.
 */

export interface DuckDBGDXModule {
  cwrap?: (name: string, returnType: string | null, argTypes: string[]) => (...args: unknown[]) => unknown;
}

export type DuckDBGDXHeaderMap = Record<string, string>;

/**
 * Configures HTTP headers that should be attached to every WASM range
 * request issued by the extension.
 */
export function configureDuckDBGDXHttpHeaders(module: DuckDBGDXModule | null | undefined, headers: DuckDBGDXHeaderMap): void {
  if (!module || typeof module.cwrap !== 'function') {
    return;
  }

  const clearHeaders = module.cwrap('gdx_wasm_clear_http_headers', null, []);
  if (typeof clearHeaders === 'function') {
    clearHeaders();
  }

  const setHeader = module.cwrap('gdx_wasm_set_http_header', null, ['string', 'string']);
  if (typeof setHeader !== 'function') {
    return;
  }

  for (const [key, value] of Object.entries(headers)) {
    setHeader(key, value);
  }
}

export interface DuckDBGDXDatabase {
  installExtension(name: string, options?: { force?: boolean; throwOnError?: boolean }): Promise<void>;
  loadExtension(name: string): Promise<void>;
}

/**
 * Installs and loads the required extensions for WASM usage.
 */
export async function ensureDuckDBGDXLoaded(db: DuckDBGDXDatabase): Promise<void> {
  await db.installExtension('gdx', { force: true });
  await db.loadExtension('gdx');
}

/**
 * High-level helper that loads the extension and applies optional HTTP header configuration.
 */
export async function initializeDuckDBGDX(
  db: DuckDBGDXDatabase,
  module: DuckDBGDXModule | null | undefined,
  headers?: DuckDBGDXHeaderMap
): Promise<void> {
  await ensureDuckDBGDXLoaded(db);
  if (headers) {
    configureDuckDBGDXHttpHeaders(module, headers);
  }
}
