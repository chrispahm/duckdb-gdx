# Using `duckdb_gdx` from JavaScript via DuckDB-WASM

This guide explains how to bundle and load the `duckdb_gdx` extension inside a browser or Node.js project powered by [DuckDB-WASM](https://github.com/duckdb/duckdb-wasm). Follow it end-to-end if you are starting a new JavaScript project and need to query `.gdx` files when the DuckDB runtime is compiled to WebAssembly.

> **tl;dr**
>
> 1. Produce (or download) `duckdb_gdx.duckdb_extension.wasm` for the WASM runtime variant you intend to ship.
> 2. Serve that binary from your application's static assets (for example `public/extensions/wasm_eh/duckdb_gdx.duckdb_extension.wasm`).
> 3. Instantiate DuckDB-WASM, install/load the extension, and optionally configure HTTP headers through the helpers in `scripts/wasm/extension_bundle.ts`.

## Prerequisites

- Node.js 18+ (or any runtime that supports ES modules and Web Workers).
- A package manager (`npm`, `pnpm`, or `yarn`).
- Access to the built DuckDB GDX extension repository (this repo) for obtaining artifacts, or a GitHub release that already contains the WASM bundle.
- For building the WASM binaries yourself: [emscripten/emsdk](https://emscripten.org/docs/getting_started/downloads.html) available on your local machine.

## Step 1 — Obtain the WASM bundle

You need the loadable WASM artifact named `duckdb_gdx.duckdb_extension.wasm`. There are two common paths to get it.

### Option A: Download a pre-built artifact

1. Head to the extension's GitHub releases or CI artifacts.
2. Download the archive for the DuckDB version and WASM flavor you need (typically `wasm_eh` unless you target MVP or threads).
3. Extract the bundle; you should end up with a file at `duckdb_gdx.duckdb_extension.wasm`.
4. (Optional) Some releases also provide a Brotli-compressed variant (`.wasm.br`) signed for direct use with DuckDB's extension repository layout. Keep the plain `.wasm` copy for local development and the `.wasm.br` for hosting in production CDNs.

> **Note:** When hosting in a custom repository, place the file under `<base-url>/<duckdb-version>/<platform>/duckdb_gdx.duckdb_extension.wasm`. For example, if you publish to `https://cdn.example.com/extensions`, DuckDB expects `https://cdn.example.com/extensions/v1.4.0/wasm_eh/duckdb_gdx.duckdb_extension.wasm`.

### Option B: Build the bundle from source

If you cannot find a suitable artifact, build it locally:

```sh
# Clone (or update) the extension repo together with DuckDB
git clone --recurse-submodules git@github.com:chrispahm/duckdb-gdx.git
cd duckdb-gdx

# Install and activate emsdk (skip if you already have a working installation)
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh

# Produce the exception-handling WebAssembly build (recommended for browsers)
make wasm_eh

# Optional: also build the MVP or threads variants
make wasm_mvp
make wasm_threads
```

After the build succeeds, the bundles are written to:

```text
build/wasm_eh/extension/duckdb_gdx/duckdb_gdx.duckdb_extension.wasm
build/wasm_mvp/extension/duckdb_gdx/duckdb_gdx.duckdb_extension.wasm
build/wasm_threads/extension/duckdb_gdx/duckdb_gdx.duckdb_extension.wasm
```

For CI deployment you can additionally run `make output_distribution_matrix` and the helper scripts in `duckdb/scripts/extension-upload-wasm.sh` to sign and Brotli-compress the artifact before uploading it to a static hosting location.

## Step 2 — Add the bundle to your JavaScript project

- Create (or use) a directory that is served verbatim by your bundler/dev server. In Vite, for example, files placed under `public/` are copied as-is to the final build output.
- Copy the WASM binary into a versioned subdirectory. A simple convention that mirrors DuckDB's layout is:

    ```text
    public/
      extensions/
        v1.4.0/
          wasm_eh/
            duckdb_gdx.duckdb_extension.wasm
    ```

- When shipping Brotli-compressed bundles, configure your web server or CDN to respond with `Content-Type: application/wasm` and `Content-Encoding: br`.
- If you rely on DuckDB's `INSTALL ... FROM` syntax, the directory structure must remain accessible over HTTP exactly as laid out above.

## Step 3 — Wire up DuckDB-WASM

Install the DuckDB-WASM runtime and (optionally) copy the pre-built TypeScript helpers shipped with this repository:

```sh
npm install @duckdb/duckdb-wasm
# Optional: reuse the helper instead of rewriting it from scratch
mkdir -p src/lib
cp ../duckdb-gdx/scripts/wasm/extension_bundle.ts src/lib/duckdb_gdx.ts
```

A minimal bootstrap (ESM) that installs and loads the extension looks like this:

```ts
import {
  AsyncDuckDB,
  ConsoleLogger,
  DuckDBDataProtocol,
  createWorker,
  getJsDelivrBundles,
  selectBundle
} from '@duckdb/duckdb-wasm';
import { initializeDuckDBGDX } from './lib/duckdb_gdx';

const EXTENSION_BASE_URL = new URL('/extensions/v1.4.0/wasm_eh/', window.location.origin);

async function bootstrap(): Promise<void> {
  const bundles = getJsDelivrBundles();
  const bundle = await selectBundle(bundles);

  // Inform DuckDB where to download the custom WASM extension from
  const extensionUrl = new URL('duckdb_gdx.duckdb_extension.wasm', EXTENSION_BASE_URL).toString();
  bundle.extensions = [
    ...(bundle.extensions ?? []),
    { name: 'duckdb_gdx', mainModule: extensionUrl }
  ];

  const logger = new ConsoleLogger();
  const worker = await createWorker(bundle.mainWorker);
  const db = new AsyncDuckDB(logger, worker);

  await db.instantiate(bundle.mainModule, bundle.pthreadWorker);

  const connection = await db.connect();
  try {
    await initializeDuckDBGDX(connection, db.module, {
      Authorization: 'Bearer <optional-token>'
    });

    await connection.useUnsafe(async (bindings) => {
      await bindings.registerFileURL(
        'transport.gdx',
        'https://example.com/data/transport.gdx',
        DuckDBDataProtocol.HTTP,
        true
      );
    });

    const result = await connection.query(`
      SELECT *
      FROM read_gdx('transport.gdx', 'd')
      LIMIT 5;
    `);
    console.table(result.toArray());
  } finally {
    await connection.close();
    await db.terminate();
    worker.terminate();
  }
}

bootstrap().catch((error) => {
  console.error('DuckDB-WASM bootstrap failed', error);
});
```

Key points:

- Populate `bundle.extensions` before calling `instantiate` so DuckDB knows where to download `duckdb_gdx.duckdb_extension.wasm`.
- `initializeDuckDBGDX`
- `registerFileURL` maps a logical filename (`transport.gdx`) to a real HTTP endpoint; the `DuckDBDataProtocol.HTTP` hint ensures the WASM backend drives HTTP range reads through the default HTTP module.

If you prefer the SQL approach, replace the helper call with:

```sql
INSTALL duckdb_gdx FROM 'https://cdn.example.com/extensions';
LOAD duckdb_gdx;
```

Ensure that the URL you pass exposes the `<version>/<platform>/<extension>.duckdb_extension.wasm` layout described earlier.

## Optional — Configure HTTP headers and caching

- Use `configureDuckDBGDXHttpHeaders(module, headers)` directly when you need full control (for example, rotating tokens without reinstalling the extension).
- Headers are applied to _every_ HTTP range request issued by the GDX random-access adapter. Avoid adding large numbers of headers to keep requests small.
- Consider setting `Cache-Control` on the hosted `.wasm` file so browsers reuse it between sessions.

## Verifying your setup

1. Start your dev server (e.g., `npm run dev`).
2. Open the app in a browser and watch the network tab:
   - DuckDB should request `duckdb_gdx.duckdb_extension.wasm` from your static asset location.
   - Subsequent range requests against your `.gdx` data source should include any headers you configured.
3. The console output should contain query results similar to the demo in `examples/js/wasm-read-gdx.js`.

## Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `INSTALL duckdb_gdx` fails with `HTTP 404` | The extension file is not reachable under the expected `<version>/<platform>/` URL | Verify hosting path and version. Remember to match the DuckDB version baked into your DuckDB-WASM bundle. |
| `WebAssembly.instantiate()` throws a MIME-type error | Server responds without `application/wasm` (or missing Brotli encoding header) | Add `Content-Type: application/wasm` (and `Content-Encoding: br` if you serve the compressed variant). |
| Queries hang on the first `read_gdx` call | The default HTTP module was not loaded, or headers are missing for authenticated endpoints | Ensure `initializeDuckDBGDX` (or the SQL snippet) runs before registering files, and double-check your header configuration. |
| `TypeError: DuckDBBundles.extendBundle is not a function` | Using an older DuckDB-WASM version | Upgrade `@duckdb/duckdb-wasm` or mutate `bundle.extensions` manually: `bundle.extensions.push({ name: 'duckdb_gdx', mainModule: '...' })`. |

For a full worked example, consult `examples/js/wasm-read-gdx.js` in this repository. It demonstrates the same concepts without relying on the helper utilities.
