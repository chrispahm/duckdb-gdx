#include "gdx/gdx_random_access_adapter.hpp"

#include "catch.hpp"

#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <random>

using namespace duckdb;
using namespace duckdb::gdx;

namespace {
struct MemoryProviderState {
	explicit MemoryProviderState(std::string data_p) : data(std::move(data_p)) {
	}

	std::string data;
	bool fail_read = false;
	bool fail_size = false;
	bool closed = false;
};

int MemoryProviderRead(void *user_data, uint64_t offset, void *dst, size_t requested, size_t *out_read) {
	auto *state = reinterpret_cast<MemoryProviderState *>(user_data);
	if (!state || state->fail_read) {
		if (out_read) {
			*out_read = 0;
		}
		return 0;
	}
	if (offset >= state->data.size()) {
		if (out_read) {
			*out_read = 0;
		}
		return 1;
	}
	size_t available = state->data.size() - static_cast<size_t>(offset);
	size_t to_copy = std::min(available, requested);
	if (to_copy > 0) {
		memcpy(dst, state->data.data() + offset, to_copy);
	}
	if (out_read) {
		*out_read = to_copy;
	}
	return 1;
}

int MemoryProviderGetSize(void *user_data, uint64_t *out_size) {
	auto *state = reinterpret_cast<MemoryProviderState *>(user_data);
	if (!state || !out_size || state->fail_size) {
		return 0;
	}
	*out_size = state->data.size();
	return 1;
}

void MemoryProviderClose(void *user_data) {
	auto *state = reinterpret_cast<MemoryProviderState *>(user_data);
	if (!state) {
		return;
	}
	state->closed = true;
}

std::string MakeTempFilePath() {
	auto fs = FileSystem::CreateLocal();
	auto base_dir = FileSystem::GetWorkingDirectory();
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<int64_t> dist(0, std::numeric_limits<int64_t>::max());
	auto random_id = dist(gen);
	auto filename = StringUtil::Format("duckdb_gdx_random_access_%lld.bin", static_cast<long long>(random_id));
	return fs->JoinPath(base_dir, filename);
}
} // namespace

TEST_CASE("RandomAccessAdapter requires valid initialization", "[gdx][random_access]") {
	RandomAccessAdapter adapter;
	REQUIRE_FALSE(adapter.IsInitialized());

	std::unique_ptr<FileHandle> empty_handle;
	REQUIRE_THROWS_AS(adapter.InitializeFromFileHandle(std::move(empty_handle)), InvalidInputException);

	gdx_random_access provider {};
	REQUIRE_THROWS_AS(adapter.InitializeFromProvider(provider), InvalidInputException);

	provider.read_at = &MemoryProviderRead;
	REQUIRE_THROWS_AS(adapter.InitializeFromProvider(provider), InvalidInputException);

	provider.get_size = &MemoryProviderGetSize;
	adapter.InitializeFromProvider(provider);
	REQUIRE(adapter.IsInitialized());

	auto callbacks = adapter.GetCallbacks();
	callbacks.close(callbacks.user_data);
	REQUIRE_FALSE(adapter.IsInitialized());
}

TEST_CASE("RandomAccessAdapter bridges external providers", "[gdx][random_access]") {
	MemoryProviderState state {"abcdef"};

	gdx_random_access provider {};
	provider.user_data = &state;
	provider.read_at = &MemoryProviderRead;
	provider.get_size = &MemoryProviderGetSize;
	provider.close = &MemoryProviderClose;

	RandomAccessAdapter adapter;
	adapter.InitializeFromProvider(provider);

	std::array<char, 8> buffer {};
	auto read_bytes = adapter.Read(buffer.data(), 0, 3);
	REQUIRE(read_bytes == 3);
	REQUIRE(std::string(buffer.data(), 3) == "abc");

	read_bytes = adapter.Read(buffer.data(), 3, 6);
	REQUIRE(read_bytes == 3);
	REQUIRE(std::string(buffer.data(), 3) == "def");

	uint64_t reported_size = 0;
	auto callbacks = adapter.GetCallbacks();
	REQUIRE(callbacks.get_size(callbacks.user_data, &reported_size) == 1);
	REQUIRE(reported_size == state.data.size());

	state.fail_read = true;
	REQUIRE_THROWS_AS(adapter.Read(buffer.data(), 0, 2), IOException);
	REQUIRE(adapter.LastError() == "Upstream random-access read failed");

	state.fail_read = false;
	state.fail_size = true;
	reported_size = 0;
	REQUIRE(callbacks.get_size(callbacks.user_data, &reported_size) == 0);
	REQUIRE(adapter.LastError() == "Upstream random-access size query failed");

	callbacks.close(callbacks.user_data);
	REQUIRE(state.closed);
}

TEST_CASE("RandomAccessAdapter wraps DuckDB FileHandle", "[gdx][random_access]") {
	auto fs = FileSystem::CreateLocal();
	auto temp_path = MakeTempFilePath();
	std::string payload = "DuckDB";

	{
		auto write_handle = fs->OpenFile(temp_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		write_handle->Write(const_cast<char *>(payload.data()), payload.size());
		write_handle->Close();
	}

	auto read_handle = fs->OpenFile(temp_path, FileFlags::FILE_FLAGS_READ);
	RandomAccessAdapter adapter;
	adapter.InitializeFromFileHandle(std::move(read_handle));

	std::array<char, 8> buffer {};
	auto read_bytes = adapter.Read(buffer.data(), 0, payload.size());
	REQUIRE(read_bytes == payload.size());
	REQUIRE(std::string(buffer.data(), payload.size()) == payload);

	read_bytes = adapter.Read(buffer.data(), payload.size(), 10);
	REQUIRE(read_bytes == 0);

	uint64_t reported_size = 0;
	auto callbacks = adapter.GetCallbacks();
	REQUIRE(callbacks.get_size(callbacks.user_data, &reported_size) == 1);
	REQUIRE(reported_size == payload.size());

	callbacks.close(callbacks.user_data);
	REQUIRE_THROWS_AS(adapter.Read(buffer.data(), 0, 1), InvalidInputException);

	fs->TryRemoveFile(temp_path);
}
