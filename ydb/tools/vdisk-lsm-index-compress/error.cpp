#include "error.h"

#include <util/generic/yexception.h>
#include <util/string/builder.h>

#include <cstring>

namespace NCloud::NStorage::NLsmCompress {

namespace {

////////////////////////////////////////////////////////////////////////////////

//
// strerror_r has two forms: GNU returns the text, which may not be in the
// buffer; XSI returns 0 and fills the buffer. Overloads on the result type
// handle both.
//

[[maybe_unused]] const char* GetStrErrorText(const char* result, const char*)
{
    return result;
}

[[maybe_unused]] const char* GetStrErrorText(int result, const char* buffer)
{
    return result == 0 ? buffer : "Unknown error";
}

TString GetSystemErrorText(int code)
{
    char buffer[256] = {};
    return GetStrErrorText(strerror_r(code, buffer, sizeof(buffer)), buffer);
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

TString FormatError(const TError& error)
{
    return TStringBuilder()
        << error.Message << ": " << GetSystemErrorText(error.Code);
}

TError MakeErrorFromCurrentException()
{
    try {
        throw;
    } catch (const TSystemError& e) {

        //
        // The message starts with "(Error N: <system text>) "; the system
        // text is added again by FormatError, so the prefix is dropped.
        //

        TStringBuf message = e.what();
        if (message.StartsWith("(Error ")) {
            const size_t end = message.find(") ");
            if (end != TStringBuf::npos) {
                message.Skip(end + 2);
            }
        }
        return MakeError(e.Status() ? e.Status() : EIO, TString(message));
    } catch (...) {
        return MakeError(EIO, CurrentExceptionMessage());
    }
}

}   // namespace NCloud::NStorage::NLsmCompress
