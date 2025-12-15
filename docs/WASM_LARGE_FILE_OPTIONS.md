# Options for Handling Large GDX Files in WASM

## Problem Statement

Currently, the WASM build reads entire GDX files into memory as ArrayBuffers and processes all symbol records in memory. For files with >20-30 million entries, this causes memory exhaustion errors.

## Current Implementation

The current code uses callback-based APIs (`gdxDataReadRawFastEx` and `gdxDataReadRawFastFilt`) that buffer ALL records into a `std::vector<FilteredReadRecord>` before returning any data to DuckDB:

- **Location**: `src/gdx_read_function.cpp`
- **Issue**: `FilteredReadContext::buffered_records` stores all records in memory
- **Memory usage**: For 30M records with 4 dimensions + 5 values each ≈ 30M × (4×4 bytes + 5×8 bytes) ≈ 1.2GB+ just for the buffer

## Solution Options

### Option 1: Streaming with Incremental API (Recommended)

**Approach**: Use `gdxDataReadRawStart` + `gdxDataReadRaw` for incremental reading, directly filling DuckDB vectors without buffering.

**Pros**:
- ✅ Constant memory usage (only one vector's worth of records at a time)
- ✅ Works with any file size
- ✅ Already used successfully in `gdx_domain_values_function.cpp`
- ✅ Supports offset/limit naturally (skip during read loop)

**Cons**:
- ❌ No built-in filtering at GDX level (must filter in C++)
- ❌ Slightly slower than callback-based APIs (but acceptable for large files)

**Implementation**:
- Replace callback-based buffering with incremental read loop
- Read records in chunks (STANDARD_VECTOR_SIZE = 2048)
- Directly fill DuckDB output vectors during read
- For filtered reads: check dimension filters during incremental read

**Code Changes**:
- Modify `ReadGDXInitGlobal` to use `gdxDataReadRawStart` instead of `gdxDataReadRawFastEx`
- Modify `ReadGDXFunction` to read records incrementally using `gdxDataReadRaw`
- Remove `FilteredReadContext::buffered_records` vector
- Add filtering logic in the read loop for dimension filters

### Option 2: Streaming Callback (Hybrid)

**Approach**: Keep using `gdxDataReadRawFastEx`/`gdxDataReadRawFastFilt` but modify callbacks to directly fill DuckDB vectors instead of buffering.

**Pros**:
- ✅ Keeps GDX-level filtering for filtered reads
- ✅ Constant memory usage
- ✅ Minimal API changes

**Cons**:
- ❌ More complex: need to coordinate between callback and DuckDB's pull-based execution
- ❌ DuckDB table functions are pull-based (called when needed), but callbacks are push-based (called immediately)
- ❌ Would require a queue/buffer anyway to bridge push/pull semantics

**Implementation Challenges**:
- DuckDB calls `ReadGDXFunction` when it needs data (pull)
- GDX callbacks are called during `gdxDataReadRawFastEx` execution (push)
- Need synchronization mechanism (queue, condition variables, etc.)

### Option 3: Chunked Buffering

**Approach**: Keep current architecture but buffer in smaller chunks (e.g., 1M records at a time).

**Pros**:
- ✅ Minimal code changes
- ✅ Reduces peak memory usage

**Cons**:
- ❌ Still requires significant memory (1M records ≈ 40MB+)
- ❌ Doesn't solve the fundamental problem for very large files
- ❌ More complex state management (tracking which chunk we're in)

### Option 4: File Streaming (WASM-Specific)

**Approach**: Instead of `registerFileBuffer`, use `registerFileURL` with HTTP range requests to stream file chunks.

**Pros**:
- ✅ File itself not fully in memory
- ✅ Leverages existing HTTP range request support

**Cons**:
- ❌ Doesn't solve the record buffering problem (records still buffered)
- ❌ Requires HTTP server (not always available)
- ❌ Only helps with file loading, not record processing

**Note**: The random access adapter already supports HTTP range requests (see `gdx_wasm_support.cpp`), but the issue is record buffering, not file loading.

## Recommended Solution: Option 1 (Incremental Reading)

### Implementation Plan

1. **Remove buffering architecture**:
   - Remove `FilteredReadContext::buffered_records`
   - Remove callback-based reading paths

2. **Implement incremental reading**:
   - Use `gdxDataReadRawStart` in `ReadGDXInitGlobal`
   - Read records one-by-one in `ReadGDXFunction` using `gdxDataReadRaw`
   - Directly fill DuckDB output vectors

3. **Handle offset/limit**:
   - Skip records until offset is satisfied
   - Stop reading when limit is reached

4. **Handle filtering**:
   - For dimension filters: check each record's dimensions during read
   - For value column filters: already handled by column selection

5. **Maintain UEL cache**:
   - Keep existing UEL table preloading (already memory-efficient)

### Code Structure

```cpp
// In ReadGDXInitGlobal:
// - Call gdxDataReadRawStart to initialize reading
// - Don't read any records yet, just prepare

// In ReadGDXFunction:
// - Read records one-by-one using gdxDataReadRaw
// - Check filters (if any)
// - Skip until offset satisfied
// - Fill DuckDB vectors directly
// - Stop when limit reached or vector full
// - Call gdxDataReadDone when finished
```

### Memory Impact

- **Before**: 30M records × ~40 bytes = ~1.2GB
- **After**: STANDARD_VECTOR_SIZE (2048) × ~40 bytes = ~80KB
- **Reduction**: ~15,000x less memory

### Performance Impact

- **Before**: Fast callback-based read, but memory-bound
- **After**: Slightly slower per-record overhead, but can handle any size
- **Trade-off**: Acceptable for large files where memory is the constraint

## Alternative: Hybrid Approach

If GDX-level filtering is critical for performance, consider:

1. Use incremental reading for unfiltered queries
2. Use streaming callback for filtered queries (with careful synchronization)
3. Add a parameter to choose strategy

However, Option 1 is simpler and should work well for most use cases.

## Testing Strategy

1. Test with small files (<1M records) - should work as before
2. Test with medium files (1-10M records) - verify memory usage
3. Test with large files (>20M records) - verify no memory errors
4. Test with filters - verify correctness
5. Test with offset/limit - verify correctness
6. Performance benchmarks - compare with current implementation

## Migration Path

1. Implement Option 1 in a feature branch
2. Add runtime flag to choose old vs new implementation
3. Test thoroughly with various file sizes
4. Make new implementation default
5. Remove old implementation after validation

