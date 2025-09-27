#include "gdx/gdx_error.hpp"

#include "duckdb/common/exception.hpp"

#include "gdx.hpp"

#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace duckdb {
namespace gdx {
namespace {

const char *LookupErrorTag(int error_code) {
	switch (error_code) {
	case -100000:
		return "ERR_NOFILE";
	case -100001:
		return "ERR_FILEERROR";
	case -100002:
		return "ERR_BADMODE";
	case -100003:
		return "ERR_BADDIMENSION";
	case -100004:
		return "ERR_BADELEMENTINDEX";
	case -100005:
		return "ERR_BADSYMBOLINDEX";
	case -100006:
		return "ERR_ELEMENTSEQUENCE";
	case -100007:
		return "ERR_DUPLICATESYMBOL";
	case -100008:
		return "ERR_DATANOTSORTED";
	case -100009:
		return "ERR_DATADUPLICATE";
	case -100010:
		return "ERR_UNKNOWNFILTER";
	case -100011:
		return "ERR_BADSTRINGFORMAT";
	case -100012:
		return "ERR_BADIDENTFORMAT";
	case -100013:
		return "ERR_UELCONFLICT";
	case -100014:
		return "ERR_DUPLICATESPECVAL";
	case -100015:
		return "ERR_BADERRORRECORD";
	case -100016:
		return "ERR_DUPLICATEUEL";
	case -100017:
		return "ERR_BADUELSTR";
	case -100018:
		return "ERR_UNDEFUEL";
	case -100019:
		return "ERR_UELSECONDWRITE";
	case -100020:
		return "ERR_UELNOTEMPTY";
	case -100021:
		return "ERR_BAD_FILTER_NR";
	case -100022:
		return "ERR_BAD_FILTER_INDX";
	case -100023:
		return "ERR_FILTER_UNMAPPED";
	case -100024:
		return "ERR_OBSOLETE_FUNCTION";
	case -100025:
		return "ERR_RAWNOTSORTED";
	case -100026:
		return "ERR_BAD_ALIAS_DIM";
	case -100029:
		return "ERR_BADDATAMARKER_DATA";
	case -100030:
		return "ERR_BADDATAMARKER_DIM";
	case -100031:
		return "ERR_OPEN_BOI";
	case -100032:
		return "ERR_OPEN_FILEHEADER";
	case -100033:
		return "ERR_OPEN_FILEVERSION";
	case -100034:
		return "ERR_OPEN_FILEMARKER";
	case -100035:
		return "ERR_OPEN_SYMBOLMARKER1";
	case -100036:
		return "ERR_OPEN_SYMBOLMARKER2";
	case -100037:
		return "ERR_OPEN_UELMARKER1";
	case -100038:
		return "ERR_OPEN_UELMARKER2";
	case -100039:
		return "ERR_OPEN_TEXTMARKER1";
	case -100040:
		return "ERR_OPEN_TEXTMARKER2";
	case -100041:
		return "ERR_BADDATAFORMAT";
	case -100043:
		return "ERR_OUT_OF_MEMORY";
	case -100044:
		return "ERR_ZLIB_NOT_FOUND";
	case -100045:
		return "ERR_OPEN_ACROMARKER1";
	case -100046:
		return "ERR_OPEN_ACROMARKER2";
	case -100047:
		return "ERR_BADACROINDEX";
	case -100048:
		return "ERR_BADACRONUMBER";
	case -100049:
		return "ERR_BADACRONAME";
	case -100050:
		return "ERR_ACRODUPEMAP";
	case -100051:
		return "ERR_ACROBADADDITION";
	case -100052:
		return "ERR_UNKNOWNDOMAIN";
	case -100053:
		return "ERR_BADDOMAIN";
	case -100054:
		return "ERR_NODOMAINDATA";
	case -100055:
		return "ERR_ALIASSETEXPECTED";
	case -100056:
		return "ERR_BADDATATYPE";
	case -100057:
		return "ERR_NOSYMBOLFORCOMMENT";
	case -100058:
		return "ERR_DOMAINVIOLATION";
	case -100059:
		return "ERR_FILEALREADYOPEN";
	case -100060:
		return "ERR_FILETOOLDFORAPPEND";
	case -100061:
		return "ERR_OPEN_DOMSMARKER1";
	case -100062:
		return "ERR_OPEN_DOMSMARKER2";
	case -100063:
		return "ERR_OPEN_DOMSMARKER3";
	case -100100:
		return "ERR_GDXCOPY";
	case -100101:
		return "ERR_PARAMETER";
	case -100102:
		return "ERR_DLL_NOT_FOUND";
	case -100103:
		return "ERR_CREATE_DIR";
	case -100104:
		return "ERR_FILE_OPEN";
	case -100105:
		return "ERR_FILE_WRITE";
	case -100106:
		return "ERR_UEL_LENGTH";
	case -100107:
		return "ERR_UEL_REGISTER";
	case -100108:
		return "ERR_EXPL_TEXT";
	case -100109:
		return "ERR_DIMENSION";
	case -100110:
		return "ERR_WRITE_SYMBOL";
	case -100111:
		return "ERR_CLOSE_FILE";
	case -100112:
		return "ERR_CANNOT_DELETE";
	case -100113:
		return "ERR_CANNOT_RENAME";
	default:
		return nullptr;
	}
}

const char *LookupFallbackDescription(int error_code) {
	switch (error_code) {
	case -100000:
		return "The GDX file could not be found";
	case -100001:
		return "A low-level I/O error occurred while accessing the GDX file";
	case -100003:
		return "The symbol's dimensionality does not match the expected domain";
	case -100004:
		return "Encountered an invalid element index while reading the GDX symbol";
	case -100041:
		return "The GDX file contains data stored in an unsupported format";
	case -100043:
		return "GDX reported an out-of-memory condition";
	case -100044:
		return "The required compression backend (zlib) was not available";
	case -100052:
		return "The GDX file references an unknown domain";
	case -100054:
		return "Required domain data is missing for the requested symbol";
	case -100058:
		return "A domain violation was detected while materialising symbol records";
	case -100104:
		return "The random-access provider could not open the requested resource";
	case -100105:
		return "The random-access provider failed while writing to a temporary file";
	case -100112:
		return "The GDX runtime could not delete an intermediate artifact";
	case -100113:
		return "Renaming a staging file for the GDX runtime failed";
	default:
		return nullptr;
	}
}

bool IsOutOfMemory(int error_code) {
	return error_code == -100043;
}

bool IsIOError(int error_code) {
	switch (error_code) {
	case -100000:
	case -100001:
	case -100031:
	case -100032:
	case -100033:
	case -100034:
	case -100035:
	case -100036:
	case -100037:
	case -100038:
	case -100039:
	case -100040:
	case -100044:
	case -100045:
	case -100046:
	case -100047:
	case -100048:
	case -100049:
	case -100050:
	case -100051:
	case -100059:
	case -100060:
	case -100061:
	case -100062:
	case -100063:
	case -100100:
	case -100101:
	case -100102:
	case -100103:
	case -100104:
	case -100105:
	case -100106:
	case -100107:
	case -100108:
	case -100111:
	case -100112:
	case -100113:
		return true;
	default:
		return error_code > 0;
	}
}

std::string FetchMessageFromHandle(::gdx::TGXFileObj *handle, int error_code) {
	if (!handle || error_code == 0) {
		return {};
	}
	std::array<char, 512> buffer {};
	if (handle->gdxErrorStr(error_code, buffer.data())) {
		return std::string(buffer.data());
	}
	return {};
}

std::string BuildContextSuffix(const GDXErrorContext &context) {
	std::vector<std::string> segments;
	segments.reserve(5);
	if (!context.operation.empty()) {
		segments.emplace_back("operation=\"" + context.operation + "\"");
	}
	if (context.file_name) {
		segments.emplace_back("file=\"" + *context.file_name + "\"");
	}
	if (context.symbol) {
		segments.emplace_back("symbol=\"" + *context.symbol + "\"");
	}
	if (context.offset && context.length) {
		const auto begin = *context.offset;
		const auto end = begin + *context.length;
		segments.emplace_back("range=[" + std::to_string(begin) + ", " + std::to_string(end) + ")");
	} else if (context.offset) {
		segments.emplace_back("offset=" + std::to_string(*context.offset));
	} else if (context.length) {
		segments.emplace_back("length=" + std::to_string(*context.length));
	}

	if (segments.empty()) {
		return {};
	}

	std::ostringstream oss;
	for (size_t idx = 0; idx < segments.size(); ++idx) {
		if (idx != 0) {
			oss << ", ";
		}
		oss << segments[idx];
	}
	return oss.str();
}

} // namespace

std::string FormatGDXErrorMessage(int error_code, const GDXErrorContext &context, ::gdx::TGXFileObj *handle) {
	std::ostringstream oss;
	if (error_code == 0) {
		oss << "GDX operation completed successfully";
	} else {
		oss << "GDX error " << error_code;
		if (const auto *tag = LookupErrorTag(error_code)) {
			oss << " (" << tag << ")";
		}
		auto source_message = FetchMessageFromHandle(handle, error_code);
		if (source_message.empty()) {
			if (const auto *fallback = LookupFallbackDescription(error_code)) {
				source_message = fallback;
			}
		}
		if (!source_message.empty()) {
			oss << ": " << source_message;
		}
	}

	auto suffix = BuildContextSuffix(context);
	if (!suffix.empty()) {
		oss << " [" << suffix << "]";
	}

	return oss.str();
}

void ThrowGDXError(int error_code, const GDXErrorContext &context, ::gdx::TGXFileObj *handle) {
	if (error_code == 0) {
		throw InternalException("ThrowGDXError invoked with a non-error code");
	}

	auto message = FormatGDXErrorMessage(error_code, context, handle);
	if (IsOutOfMemory(error_code)) {
		throw OutOfMemoryException(message);
	}
	if (IsIOError(error_code)) {
		throw IOException(message);
	}

	throw InvalidInputException(message);
}

} // namespace gdx
} // namespace duckdb
