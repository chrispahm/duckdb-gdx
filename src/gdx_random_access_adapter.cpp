#include "gdx/gdx_random_access_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

#include <algorithm>
#include <utility>

namespace duckdb {
namespace gdx {

RandomAccessAdapter::RandomAccessAdapter() {
	callbacks.user_data = this;
	callbacks.read_at = &RandomAccessAdapter::ReadAtShim;
	callbacks.get_size = &RandomAccessAdapter::GetSizeShim;
	callbacks.close = &RandomAccessAdapter::CloseShim;
}

RandomAccessAdapter::~RandomAccessAdapter() {
	try {
		CloseInternal();
	} catch (...) {
		// Destructors must not throw.
	}
}

void RandomAccessAdapter::Reset() {
	CloseInternal();
	owned_handle.reset();
	handle = nullptr;
	has_cached_size = false;
	cached_size = 0;
	has_upstream_provider = false;
	upstream_provider = gdx_random_access {};
	mode = Mode::None;
	closed = false;
	last_error.clear();
}

void RandomAccessAdapter::InitializeFromFileHandle(std::unique_ptr<FileHandle> new_handle) {
	if (!new_handle) {
		throw InvalidInputException("RandomAccessAdapter::InitializeFromFileHandle received a null handle");
	}
	Reset();
	handle = new_handle.get();
	owned_handle = std::move(new_handle);
	mode = Mode::FileHandle;
	closed = false;
	has_cached_size = false;
	cached_size = 0;
	last_error.clear();
}

void RandomAccessAdapter::InitializeFromProvider(const gdx_random_access &provider) {
	if (!provider.read_at || !provider.get_size) {
		throw InvalidInputException("RandomAccessAdapter::InitializeFromProvider requires read_at and get_size callbacks");
	}
	Reset();
	upstream_provider = provider;
	has_upstream_provider = true;
	mode = Mode::ExternalProvider;
	closed = false;
	has_cached_size = false;
	cached_size = 0;
	last_error.clear();
}

const gdx_random_access &RandomAccessAdapter::GetCallbacks() const {
	return callbacks;
}

bool RandomAccessAdapter::IsInitialized() const {
	return mode != Mode::None;
}

idx_t RandomAccessAdapter::Read(void *buffer, idx_t offset, idx_t size) {
	if (!buffer && size > 0) {
		throw InvalidInputException("RandomAccessAdapter::Read called with null buffer");
	}
	if (!IsInitialized()) {
		throw InvalidInputException("RandomAccessAdapter::Read called before initialization");
	}
	if (size == 0) {
		return 0;
	}
	size_t out_read = 0;
	auto rc = ReadAt(static_cast<uint64_t>(offset), buffer, static_cast<size_t>(size), out_read);
	if (!rc) {
		if (last_error.empty()) {
			throw IOException("Failed to read from random-access provider");
		}
		throw IOException(last_error);
	}
	return static_cast<idx_t>(out_read);
}

const std::string &RandomAccessAdapter::LastError() const {
	return last_error;
}

int RandomAccessAdapter::ReadAtShim(void *user_data, uint64_t offset, void *dst, size_t requested, size_t *out_read) {
	auto *adapter = reinterpret_cast<RandomAccessAdapter *>(user_data);
	if (!adapter) {
		return 0;
	}
	size_t local_read = 0;
	if (!adapter->ReadAt(offset, dst, requested, local_read)) {
		if (out_read) {
			*out_read = 0;
		}
		return 0;
	}
	if (out_read) {
		*out_read = local_read;
	}
	return 1;
}

int RandomAccessAdapter::GetSizeShim(void *user_data, uint64_t *out_size) {
	auto *adapter = reinterpret_cast<RandomAccessAdapter *>(user_data);
	if (!adapter || !out_size) {
		return 0;
	}
	if (!adapter->QuerySize(*out_size)) {
		return 0;
	}
	return 1;
}

void RandomAccessAdapter::CloseShim(void *user_data) {
	auto *adapter = reinterpret_cast<RandomAccessAdapter *>(user_data);
	if (!adapter) {
		return;
	}
	adapter->CloseInternal();
}

int RandomAccessAdapter::ReadAt(uint64_t offset, void *dst, size_t requested, size_t &out_read) {
	out_read = 0;
	last_error.clear();
	if (!IsInitialized()) {
		last_error = "RandomAccessAdapter used before initialization";
		return 0;
	}
	if (requested == 0) {
		return 1;
	}
	// The GDX contract demands blocking behaviour. We therefore serve every call synchronously in-thread
	// and only return short reads when the logical end-of-file has been reached or the upstream provider
	// indicates exhaustion.
	try {
		switch (mode) {
		case Mode::FileHandle: {
			if (!handle) {
				last_error = "File handle is not available";
				return 0;
			}
			if (!has_cached_size) {
				cached_size = static_cast<uint64_t>(handle->GetFileSize());
				has_cached_size = true;
			}
			auto file_size = cached_size;
			if (offset >= file_size) {
				out_read = 0;
				return 1;
			}
			auto remaining = file_size - offset;
			auto to_read = static_cast<size_t>(std::min<uint64_t>(remaining, requested));
			handle->Read(dst, static_cast<idx_t>(to_read), static_cast<idx_t>(offset));
			out_read = to_read;
			return 1;
		}
		case Mode::ExternalProvider: {
			if (!has_upstream_provider || !upstream_provider.read_at) {
				last_error = "Upstream random-access provider lacks read_at callback";
				return 0;
			}
			size_t provider_read = 0;
			auto success = upstream_provider.read_at(upstream_provider.user_data, offset, dst, requested, &provider_read);
			if (!success) {
				last_error = "Upstream random-access read failed";
				return 0;
			}
			out_read = provider_read;
			return 1;
		}
		case Mode::None:
		default:
			last_error = "RandomAccessAdapter is not initialized";
			return 0;
		}
	} catch (const Exception &ex) {
		last_error = ex.what();
		return 0;
	} catch (const std::exception &ex) {
		last_error = ex.what();
		return 0;
	}
}

bool RandomAccessAdapter::QuerySize(uint64_t &out_size) {
	last_error.clear();
	if (!IsInitialized()) {
		last_error = "RandomAccessAdapter used before initialization";
		return false;
	}
	try {
		switch (mode) {
		case Mode::FileHandle: {
			if (!handle) {
				last_error = "File handle is not available";
				return false;
			}
			if (!has_cached_size) {
				cached_size = static_cast<uint64_t>(handle->GetFileSize());
				has_cached_size = true;
			}
			out_size = cached_size;
			return true;
		}
		case Mode::ExternalProvider: {
			if (!has_upstream_provider || !upstream_provider.get_size) {
				last_error = "Upstream random-access provider lacks get_size callback";
				return false;
			}
			uint64_t provider_size = 0;
			auto success = upstream_provider.get_size(upstream_provider.user_data, &provider_size);
			if (!success) {
				last_error = "Upstream random-access size query failed";
				return false;
			}
			out_size = provider_size;
			return true;
		}
		case Mode::None:
		default:
			last_error = "RandomAccessAdapter is not initialized";
			return false;
		}
	} catch (const Exception &ex) {
		last_error = ex.what();
		return false;
	} catch (const std::exception &ex) {
		last_error = ex.what();
		return false;
	}
}

void RandomAccessAdapter::CloseInternal() {
	if (closed) {
		return;
	}
	try {
		switch (mode) {
		case Mode::FileHandle:
			if (owned_handle) {
				try {
					owned_handle->Close();
				} catch (const Exception &) {
					// swallow close exceptions; resources will be released by reset
				} catch (const std::exception &) {
				}
			}
			owned_handle.reset();
			handle = nullptr;
			break;
		case Mode::ExternalProvider:
			if (has_upstream_provider && upstream_provider.close) {
				upstream_provider.close(upstream_provider.user_data);
			}
			has_upstream_provider = false;
			upstream_provider = gdx_random_access {};
			break;
		case Mode::None:
		default:
			break;
		}
	} catch (...) {
		// never propagate exceptions during close
	}
	has_cached_size = false;
	cached_size = 0;
	last_error.clear();
	mode = Mode::None;
	closed = true;
}

} // namespace gdx
} // namespace duckdb
