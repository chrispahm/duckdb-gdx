#include "gdx/gdx_wasm_support.hpp"

#ifdef __EMSCRIPTEN__

#include "gdx/gdx_random_access_adapter.hpp"

#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gxfile.hpp"

namespace duckdb {
namespace gdx {

namespace {

using HttpHeaderList = std::vector<std::pair<std::string, std::string>>;


class GlobalHeaderRegistry {
public:
	static GlobalHeaderRegistry &Get() {
		static GlobalHeaderRegistry instance;
		return instance;
	}

	void Set(std::string name, std::string value) {
		StringUtil::Trim(name);
		StringUtil::Trim(value);
		if (name.empty()) {
			return;
		}
		auto normalized = StringUtil::Lower(name);
		std::lock_guard<std::mutex> lock(mutex);
		headers[normalized] = std::make_pair(std::move(name), std::move(value));
	}

	void Clear() {
		std::lock_guard<std::mutex> lock(mutex);
		headers.clear();
	}

	HttpHeaderList Snapshot() const {
		std::lock_guard<std::mutex> lock(mutex);
		HttpHeaderList result;
		result.reserve(headers.size());
		for (const auto &entry : headers) {
			result.push_back(entry.second);
		}
		return result;
	}

private:
	mutable std::mutex mutex;
	std::unordered_map<std::string, std::pair<std::string, std::string>> headers;
};

std::optional<uint64_t> ParseContentLengthHeader(const char *headers) {
	if (!headers) {
		return std::nullopt;
	}
	std::stringstream stream(headers);
	std::string line;
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		auto name = line.substr(0, colon);
		auto value = line.substr(colon + 1);
		StringUtil::Trim(name);
		StringUtil::Trim(value);
		if (StringUtil::CIEquals(name, "Content-Length")) {
			try {
				return std::stoull(value);
			} catch (const std::exception &) {
				return std::nullopt;
			}
		}
	}
	return std::nullopt;
}

std::optional<uint64_t> ParseContentRangeHeader(const char *headers) {
	if (!headers) {
		return std::nullopt;
	}
	std::stringstream stream(headers);
	std::string line;
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		auto name = line.substr(0, colon);
		auto value = line.substr(colon + 1);
		StringUtil::Trim(name);
		StringUtil::Trim(value);
		if (StringUtil::CIEquals(name, "Content-Range")) {
			auto slash = value.find('/');
			if (slash == std::string::npos) {
				return std::nullopt;
			}
			auto total_str = value.substr(slash + 1);
			StringUtil::Trim(total_str);
			if (total_str.empty() || total_str == "*") {
				return std::nullopt;
			}
			try {
				return std::stoull(total_str);
			} catch (const std::exception &) {
				return std::nullopt;
			}
		}
	}
	return std::nullopt;
}

std::string FetchResponseHeaders(emscripten_fetch_t *fetch) {
	if (!fetch) {
		return {};
	}
	const size_t length = emscripten_fetch_get_response_headers_length(fetch);
	if (length == 0) {
		return {};
	}
	std::vector<char> buffer(length + 1, '\0');
	emscripten_fetch_get_response_headers(fetch, buffer.data(), buffer.size());
	return std::string(buffer.data());
}

bool IsHttpUrl(const std::string &resource) {
	auto trimmed = resource;
	StringUtil::Trim(trimmed);
	auto lowered = StringUtil::Lower(trimmed);
	return StringUtil::StartsWith(lowered, "http://") || StringUtil::StartsWith(lowered, "https://");
}

class WasmHttpRandomAccess {
public:
	explicit WasmHttpRandomAccess(std::string resource) : url(std::move(resource)) {
	}

	bool Read(uint64_t offset, void *dst, size_t requested, size_t &out_read) {
		std::lock_guard<std::mutex> lock(mutex);
		return PerformFetchLocked(offset, requested, dst, out_read);
	}

	bool GetSize(uint64_t &out_size) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!EnsureContentLengthLocked()) {
			return false;
		}
		out_size = *content_length;
		return true;
	}

private:
	bool EnsureContentLengthLocked() {
		if (content_length) {
			return true;
		}
		if (TryHeadLocked()) {
			return content_length.has_value();
		}
		size_t dummy = 0;
		if (PerformFetchLocked(0, 1, nullptr, dummy)) {
			return content_length.has_value();
		}
		return false;
	}

	bool TryHeadLocked() {
		emscripten_fetch_attr_t attr;
		emscripten_fetch_attr_init(&attr);
		std::memset(attr.requestMethod, 0, sizeof(attr.requestMethod));
		std::strncpy(attr.requestMethod, "HEAD", sizeof(attr.requestMethod) - 1);
		attr.attributes = EMSCRIPTEN_FETCH_SYNCHRONOUS;
		attr.onsuccess = nullptr;
		attr.onerror = nullptr;
		auto *fetch = emscripten_fetch(&attr, url.c_str());
		if (!fetch) {
			return false;
		}
		const bool ok = fetch->status >= 200 && fetch->status < 400;
		if (ok) {
			if (fetch->totalBytes >= 0) {
				content_length = static_cast<uint64_t>(fetch->totalBytes);
			} else {
				auto headers = FetchResponseHeaders(fetch);
				auto len = ParseContentLengthHeader(headers.c_str());
				if (len) {
					content_length = *len;
				}
			}
		}
		emscripten_fetch_close(fetch);
		return ok && content_length.has_value();
	}

	bool PerformFetchLocked(uint64_t offset, size_t requested, void *dst, size_t &out_read) {
		out_read = 0;
		if (requested == 0) {
			return true;
		}

		emscripten_fetch_attr_t attr;
		emscripten_fetch_attr_init(&attr);
		std::memset(attr.requestMethod, 0, sizeof(attr.requestMethod));
		std::strncpy(attr.requestMethod, "GET", sizeof(attr.requestMethod) - 1);
		attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
		attr.onsuccess = nullptr;
		attr.onerror = nullptr;

		auto headers_snapshot = GlobalHeaderRegistry::Get().Snapshot();
		std::vector<std::string> header_storage;
		header_storage.reserve(2 + headers_snapshot.size() * 2);

		uint64_t end = offset + static_cast<uint64_t>(requested) - 1;
		std::string range_value = "bytes=" + std::to_string(offset) + "-" + std::to_string(end);
		header_storage.emplace_back("Range");
		header_storage.emplace_back(range_value);

		for (auto &entry : headers_snapshot) {
			header_storage.push_back(entry.first);
			header_storage.push_back(entry.second);
		}

		std::vector<const char *> header_ptrs;
		header_ptrs.reserve(header_storage.size() + 1);
		for (auto &entry : header_storage) {
			header_ptrs.push_back(entry.c_str());
		}
		header_ptrs.push_back(nullptr);
		attr.requestHeaders = header_ptrs.data();

		auto *fetch = emscripten_fetch(&attr, url.c_str());
		if (!fetch) {
			return false;
		}

		bool success = false;
		if (fetch->status == 206 || fetch->status == 200) {
			size_t available = static_cast<size_t>(fetch->numBytes);
			if (fetch->status == 200 && offset > 0) {
				success = false;
			} else {
				size_t to_copy = std::min(requested, available);
				if (dst && to_copy > 0) {
					std::memcpy(dst, fetch->data, to_copy);
				}
				out_read = to_copy;
				success = true;
			}
		} else if (fetch->status == 416) {
			success = true;
			out_read = 0;
		} else {
			success = false;
		}

		if (success) {
			if (!content_length) {
				if (fetch->totalBytes >= 0) {
					content_length = static_cast<uint64_t>(fetch->totalBytes);
				} else {
					auto headers = FetchResponseHeaders(fetch);
					auto total = ParseContentRangeHeader(headers.c_str());
					if (total) {
						content_length = *total;
					} else {
						auto len = ParseContentLengthHeader(headers.c_str());
						if (len) {
							content_length = *len;
						} else if (fetch->status == 200) {
							content_length = static_cast<uint64_t>(fetch->numBytes);
						}
					}
				}
			}
		}

		emscripten_fetch_close(fetch);
		return success;
	}

	std::string url;
	std::optional<uint64_t> content_length;
	std::mutex mutex;
};

struct WasmHttpRandomAccessHolder {
	std::shared_ptr<WasmHttpRandomAccess> state;

	static WasmHttpRandomAccessHolder *FromUserData(void *user_data) {
		return reinterpret_cast<WasmHttpRandomAccessHolder *>(user_data);
	}

	static int ReadShim(void *user_data, uint64_t offset, void *dst, size_t requested, size_t *out_read) {
		auto *holder = FromUserData(user_data);
		if (!holder || !holder->state) {
			if (out_read) {
				*out_read = 0;
			}
			return 0;
		}
		size_t bytes = 0;
		if (!holder->state->Read(offset, dst, requested, bytes)) {
			if (out_read) {
				*out_read = 0;
			}
			return 0;
		}
		if (out_read) {
			*out_read = bytes;
		}
		return 1;
	}

	static int GetSizeShim(void *user_data, uint64_t *out_size) {
		auto *holder = FromUserData(user_data);
		if (!holder || !holder->state || !out_size) {
			return 0;
		}
		uint64_t size = 0;
		if (!holder->state->GetSize(size)) {
			return 0;
		}
		*out_size = size;
		return 1;
	}

	static void CloseShim(void *user_data) {
		auto *holder = FromUserData(user_data);
		delete holder;
	}
};

} // namespace

bool InitializeWasmRandomAccess(RandomAccessAdapter &adapter, const std::string &resource) {
	if (!IsHttpUrl(resource)) {
		return false;
	}
	auto *holder = new WasmHttpRandomAccessHolder();
	holder->state = std::make_shared<WasmHttpRandomAccess>(resource);
	gdx_random_access callbacks {};
	callbacks.user_data = holder;
	callbacks.read_at = &WasmHttpRandomAccessHolder::ReadShim;
	callbacks.get_size = &WasmHttpRandomAccessHolder::GetSizeShim;
	callbacks.close = &WasmHttpRandomAccessHolder::CloseShim;
	adapter.InitializeFromProvider(callbacks);
	return true;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void duckdb_gdx_wasm_set_http_header(const char *name, const char *value) {
	if (!name) {
		return;
	}
	GlobalHeaderRegistry::Get().Set(name, value ? value : "");
}

EMSCRIPTEN_KEEPALIVE void duckdb_gdx_wasm_clear_http_headers() {
	GlobalHeaderRegistry::Get().Clear();
}

} // extern "C"

} // namespace gdx
} // namespace duckdb

#endif // __EMSCRIPTEN__
