#pragma once

#include "gdx/gdx_random_access_adapter.hpp"

#include <string>

namespace duckdb {

class ClientContext;

namespace gdx {

class GDXFileRandomAccessProvider {
public:
	GDXFileRandomAccessProvider() = default;

	//! Reset the provider and release any owned handles.
	void Reset();

	//! Opens \p file_or_url using DuckDB's FileSystem and wires the result into the adapter.
	void Initialize(ClientContext &context, const std::string &file_or_url);

	//! Returns the random-access callbacks backed by the DuckDB file handle.
	const gdx_random_access &GetCallbacks() const {
		return adapter.GetCallbacks();
	}

	//! Access to the underlying adapter (e.g., for direct reads or status checks).
	RandomAccessAdapter &Adapter() {
		return adapter;
	}

	const RandomAccessAdapter &Adapter() const {
		return adapter;
	}

	//! The user-supplied location string passed to Initialize.
	const std::string &Location() const {
		return requested_path;
	}

	//! The resolved physical path when available (may remain identical to the request for remote URLs).
	const std::string &ResolvedPath() const {
		return resolved_path;
	}

	//! Indicates whether the resolved resource is remote according to DuckDB's FileSystem heuristics.
	bool IsRemote() const {
		return is_remote;
	}

private:
	RandomAccessAdapter adapter;
	std::string requested_path;
	std::string resolved_path;
	bool is_remote {false};
};

} // namespace gdx
} // namespace duckdb
