#pragma once

#include "duckdb.hpp"
#include "gdx_random_access.h"

#include <memory>
#include <string>

namespace duckdb {
namespace gdx {

//! Adapter bridging DuckDB's FileSystem API to the synchronous GDX random-access callbacks.
//!
//! GDX expects blocking reads: whenever it invokes `read_at` the implementation must either deliver the
//! requested number of bytes immediately or return fewer bytes strictly when the logical end-of-file has
//! been reached. The adapter therefore performs eager, in-thread reads against DuckDB's `FileHandle`
//! interface and never initiates background fetches. Callers that wish to layer asynchronous prefetching
//! should stage data in their own provider and delegate to `InitializeFromProvider`.
//!
//! All methods are single-threaded and the adapter is not safe to share across concurrent GDX readers.
class RandomAccessAdapter {
public:
	RandomAccessAdapter();
	~RandomAccessAdapter();

	//! Resets adapter state and associated handles.
	void Reset();

	//! Initializes the adapter from a DuckDB file handle. Requires random-access semantics.
	void InitializeFromFileHandle(std::unique_ptr<FileHandle> handle);

	//! Initializes the adapter from an arbitrary random-access provider (e.g., HTTP range source).
	//! The supplied callbacks must follow the same synchronous/blocking contract as DuckDB file handles.
	void InitializeFromProvider(const gdx_random_access &provider);

	//! Returns the callback suite that can be passed to the GDX API.
	const gdx_random_access &GetCallbacks() const;

	//! Indicates whether the adapter currently wraps a provider.
	bool IsInitialized() const;

	//! Attempts to read \p size bytes at \p offset into \p buffer. Returns the number of bytes read.
	duckdb::idx_t Read(void *buffer, duckdb::idx_t offset, duckdb::idx_t size);

	//! Returns the description for the most recent error, if any. Empty when the last operation succeeded.
	const std::string &LastError() const;

private:
	enum class Mode { None, FileHandle, ExternalProvider };

	static int ReadAtShim(void *user_data, uint64_t offset, void *dst, size_t requested, size_t *out_read);
	static int GetSizeShim(void *user_data, uint64_t *out_size);
	static void CloseShim(void *user_data);

	int ReadAt(uint64_t offset, void *dst, size_t requested, size_t &out_read);
	bool QuerySize(uint64_t &out_size);
	void CloseInternal();

	Mode mode {Mode::None};
	std::unique_ptr<FileHandle> owned_handle;
	FileHandle *handle {nullptr};
	bool has_cached_size {false};
	uint64_t cached_size {0};
	gdx_random_access callbacks {};
	gdx_random_access upstream_provider {};
	bool has_upstream_provider {false};
	bool closed {false};
	std::string last_error;
};

} // namespace gdx
} // namespace duckdb
