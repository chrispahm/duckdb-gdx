async function run(): Promise<void> {
  await import('./extension_bundle.test');
  await import('./duckdb_gdx_wasm.integration.test');
}

run().catch((error) => {
  console.error(error);
  setTimeout(() => {
    throw error;
  }, 0);
});
