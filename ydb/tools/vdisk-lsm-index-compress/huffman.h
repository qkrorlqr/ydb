#pragma once

#include "bit_stream.h"
#include "error.h"

#include <util/generic/hash.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

/**
 * Canonical Huffman code over ui64 symbols. Only the code lengths and the
 * symbols are stored; codewords are assigned in (length, symbol) order.
 * A code with a single symbol uses zero bits per value.
 */
class THuffmanCode
{
public:
    static constexpr ui32 MaxCodeLength = 48;

    // Codewords up to this length are decoded with one table lookup.
    static constexpr ui32 MaxLookupBits = 10;

private:
    struct TCodeword
    {
        ui64 Code = 0;
        ui32 Length = 0;
    };

    // Symbols in canonical order.
    TVector<ui64> Symbols;

    //
    // Indexed by code length, [0, MaxLength]. FirstCode is the codeword of
    // the first symbol of each length, FirstIndex is its index in Symbols.
    //

    TVector<ui64> LengthCounts;
    TVector<ui64> FirstCode;
    TVector<ui64> FirstIndex;
    ui32 MaxLength = 0;

    //
    // Indexed by the next LookupBits bits of the stream. An entry is
    // (symbol index << 6) | codeword length for codewords not longer than
    // LookupBits, and 0 where the codeword is longer.
    //

    TVector<ui32> Lookup;
    ui32 LookupBits = 0;

    // Filled by Build only, Load does not need it.
    THashMap<ui64, TCodeword> Codewords;

public:
    /**
     * Builds the code. Code lengths are limited to MaxCodeLength by
     * flattening the frequencies.
     *
     * @param frequencies - Number of occurrences of each symbol.
     * @return - The code.
     */
    static THuffmanCode Build(const THashMap<ui64, ui64>& frequencies);

    /**
     * Restores a code saved by Save.
     *
     * @param data - Saved code.
     * @return - The code or an error if the data is malformed.
     */
    static TResultOrError<THuffmanCode> Load(TStringBuf data);

    /**
     * Serializes symbol count, length counts and symbols as varints.
     * Symbols of one length are ascending and are stored as deltas.
     *
     * @return - Serialized code.
     */
    TString Save() const;

    /**
     * Appends the codeword of a symbol. Available only for codes made by
     * Build; the symbol must be one of the built symbols.
     *
     * @param value - Symbol.
     * @param writer - (out) Bit stream to append to.
     */
    void Encode(ui64 value, TBitWriter& writer) const;

    /**
     * Reads one codeword.
     *
     * @param reader - (out) Bit stream to read from.
     * @param value - (out) Decoded symbol.
     * @return - False if the bits do not form a codeword.
     */
    bool Decode(TBitReader& reader, ui64* value) const;

    /**
     * @param value - Symbol.
     * @return - Codeword length of the symbol. Available only for codes
     *  made by Build.
     */
    ui32 GetCodeLength(ui64 value) const;

    /**
     * @return - Number of distinct symbols.
     */
    ui64 GetSymbolCount() const
    {
        return Symbols.size();
    }

    /**
     * @return - Length of the longest codeword.
     */
    ui32 GetMaxLength() const
    {
        return MaxLength;
    }

private:
    bool Init();
    void InitLookup();
};

}   // namespace NCloud::NStorage::NLsmCompress
