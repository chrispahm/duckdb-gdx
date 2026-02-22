import * as wasm from '../../scripts/wasm/extension_bundle';

const {
  configureDuckDBGDXHttpHeaders,
  ensureDuckDBGDXLoaded,
  initializeDuckDBGDX
} = wasm;

type DuckDBGDXModule = wasm.DuckDBGDXModule;
type DuckDBGDXDatabase = wasm.DuckDBGDXDatabase;

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

type RecordedCall = { type: 'clear' } | { type: 'set'; key: string; value: string };

class RecordingModule implements DuckDBGDXModule {
  public readonly calls: RecordedCall[] = [];

  cwrap(name: string): (...args: unknown[]) => unknown {
    if (name === 'gdx_wasm_clear_http_headers') {
      return () => {
        this.calls.push({ type: 'clear' });
      };
    }
    if (name === 'gdx_wasm_set_http_header') {
      return (key: unknown, value: unknown) => {
        this.calls.push({ type: 'set', key: String(key), value: String(value) });
      };
    }
    throw new Error(`Unexpected cwrap lookup: ${name}`);
  }
}

class RecordingDatabase implements DuckDBGDXDatabase {
  public readonly calls: string[] = [];

  async installExtension(name: string): Promise<void> {
    this.calls.push(`install:${name}`);
  }

  async loadExtension(name: string): Promise<void> {
    this.calls.push(`load:${name}`);
  }
}

async function run(): Promise<void> {
  // configureDuckDBGDXHttpHeaders should be a no-op when no module is provided
  configureDuckDBGDXHttpHeaders(null, { Authorization: 'token' });

  const module = new RecordingModule();
  configureDuckDBGDXHttpHeaders(module, { 'X-Test': 'alpha', Authorization: 'beta' });
  assertEqual(module.calls.length, 3);
  assertDeepEqual(module.calls[0], { type: 'clear' } as RecordedCall);
  assertDeepEqual(module.calls[1], { type: 'set', key: 'X-Test', value: 'alpha' } as RecordedCall);
  assertDeepEqual(module.calls[2], { type: 'set', key: 'Authorization', value: 'beta' } as RecordedCall);

  const db = new RecordingDatabase();
  await ensureDuckDBGDXLoaded(db);
  assertDeepEqual(db.calls, [
    'install:gdx',
    'load:gdx'
  ]);

  const module2 = new RecordingModule();
  const db2 = new RecordingDatabase();
  await initializeDuckDBGDX(db2, module2, { 'X-Env': 'ci' });
  assertDeepEqual(db2.calls, [
    'install:gdx',
    'load:gdx'
  ]);
  assertEqual(module2.calls.length, 2);
  assertDeepEqual(module2.calls[0], { type: 'clear' } as RecordedCall);
  assertDeepEqual(module2.calls[1], { type: 'set', key: 'X-Env', value: 'ci' } as RecordedCall);
}

run().catch((error) => {
  console.error(error);
  setTimeout(() => {
    throw error;
  }, 0);
});
