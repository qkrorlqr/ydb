#pragma once

#include <util/generic/string.h>

#include <cerrno>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

/**
 * Error as an errno code and a message. Code 0 means no error.
 */
struct TError
{
    int Code = 0;
    TString Message;
};

/**
 * @param code - Errno code, not 0.
 * @param message - What failed.
 * @return - The error.
 */
inline TError MakeError(int code, TString message)
{
    return TError{.Code = code, .Message = std::move(message)};
}

/**
 * @param error - Error.
 * @return - True if the code is not 0.
 */
inline bool HasError(const TError& error)
{
    return error.Code != 0;
}

/**
 * Formats an error as perror does: "<message>: <system text>". The
 * system text comes from strerror_r.
 *
 * @param error - Error.
 * @return - The text.
 */
TString FormatError(const TError& error);

/**
 * Makes an error from the exception being handled. Must be called inside
 * a catch block. System errors keep their errno code, others get EIO.
 *
 * @return - The error.
 */
TError MakeErrorFromCurrentException();

////////////////////////////////////////////////////////////////////////////////

/**
 * A value or an error. Supports structured bindings:
 * auto [value, error] = ...
 */
template <typename T>
class TResultOrError
{
private:
    T Result;
    TError Error;

public:
    TResultOrError(T result)
        : Result(std::move(result))
    {}

    TResultOrError(TError error)
        : Result{}
        , Error(std::move(error))
    {}

    template <size_t I>
    const auto& get() const&
    {
        if constexpr (I == 0) {
            return Result;
        } else {
            return Error;
        }
    }

    template <size_t I>
    auto&& get() &&
    {
        if constexpr (I == 0) {
            return std::move(Result);
        } else {
            return std::move(Error);
        }
    }
};

}   // namespace NCloud::NStorage::NLsmCompress

////////////////////////////////////////////////////////////////////////////////

template <typename T>
struct std::tuple_size<NCloud::NStorage::NLsmCompress::TResultOrError<T>>
    : std::integral_constant<size_t, 2>
{};

template <typename T>
struct std::tuple_element<0, NCloud::NStorage::NLsmCompress::TResultOrError<T>>
{
    using type = T;
};

template <typename T>
struct std::tuple_element<1, NCloud::NStorage::NLsmCompress::TResultOrError<T>>
{
    using type = NCloud::NStorage::NLsmCompress::TError;
};
