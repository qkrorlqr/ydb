#pragma once

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/system/types.h>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

/**
 * Appends a value as LEB128: 7 bits per byte, low bits first, high bit of
 * each byte set if more bytes follow.
 *
 * @param out - (out) Buffer to append to.
 * @param value - Value to append.
 */
inline void WriteVarint(TString& out, ui64 value)
{
    while (value >= 0x80U) {
        out.push_back(static_cast<char>((value & 0x7FU) | 0x80U));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

/**
 * Reads a value written by WriteVarint and advances the buffer.
 *
 * @param in - (out) Buffer to read from, advanced past the value.
 * @param value - (out) The value.
 * @return - False if the buffer ends early or the value is too long.
 */
inline bool ReadVarint(TStringBuf& in, ui64* value)
{
    ui64 result = 0;
    for (ui32 shift = 0; shift < 64; shift += 7) {
        if (in.empty()) {
            return false;
        }
        const auto byte = static_cast<ui8>(in[0]);
        in.Skip(1);
        result |= static_cast<ui64>(byte & 0x7FU) << shift;
        if (!(byte & 0x80U)) {
            *value = result;
            return true;
        }
    }
    return false;
}

}   // namespace NCloud::NStorage::NLsmCompress
