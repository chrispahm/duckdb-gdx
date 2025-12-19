#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace gdx {
class TGXFileObj;
}

namespace duckdb {
namespace gdx {

struct GDXErrorContext {
	std::string operation;
	std::optional<std::string> file_name;
	std::optional<std::string> symbol;
	std::optional<uint64_t> offset;
	std::optional<uint64_t> length;

	GDXErrorContext() = default;
	explicit GDXErrorContext(std::string op) : operation(std::move(op)) {
	}

	GDXErrorContext &WithFile(std::string value) {
		file_name = std::move(value);
		return *this;
	}

	GDXErrorContext &WithSymbol(std::string value) {
		symbol = std::move(value);
		return *this;
	}

	GDXErrorContext &WithOffset(uint64_t value) {
		offset = value;
		return *this;
	}

	GDXErrorContext &WithLength(uint64_t value) {
		length = value;
		return *this;
	}
};

std::string FormatGDXErrorMessage(int error_code, const GDXErrorContext &context = GDXErrorContext(),
                                  ::gdx::TGXFileObj *handle = nullptr);

[[noreturn]] void ThrowGDXError(int error_code, const GDXErrorContext &context = GDXErrorContext(),
                                ::gdx::TGXFileObj *handle = nullptr);

inline void ThrowIfGDXError(int status_code, const GDXErrorContext &context = GDXErrorContext(),
                            ::gdx::TGXFileObj *handle = nullptr) {
	if (status_code != 0) {
		ThrowGDXError(status_code, context, handle);
	}
}

} // namespace gdx
} // namespace duckdb
