#include "huffman.h"

#include "varint.h"

#include <util/generic/algorithm.h>

namespace NCloud::NStorage::NLsmCompress {

namespace {

////////////////////////////////////////////////////////////////////////////////

/**
 * Computes Huffman code lengths with the two-queue method.
 *
 * @param weights - Symbol weights in ascending order.
 * @return - Code length of each symbol, in the order of weights.
 */
TVector<ui32> ComputeCodeLengths(const TVector<ui64>& weights)
{
    const size_t n = weights.size();
    if (n <= 1) {
        return TVector<ui32>(n, 0);
    }

    //
    // Nodes [0, n) are leaves, nodes [n, 2n - 1) are internal nodes in the
    // order of creation. Internal node weights grow monotonically, so the
    // two smallest nodes are always at the heads of the two queues.
    //

    const size_t nodeCount = 2 * n - 1;
    TVector<ui64> weight(nodeCount);
    TVector<size_t> parent(nodeCount);
    Copy(weights.begin(), weights.end(), weight.begin());

    size_t leafHead = 0;
    size_t internalHead = n;
    size_t next = n;

    auto pick = [&]
    {
        if (leafHead < n
            && (internalHead == next
                || weight[leafHead] <= weight[internalHead]))
        {
            return leafHead++;
        }
        return internalHead++;
    };

    for (; next < nodeCount; ++next) {
        const size_t a = pick();
        const size_t b = pick();
        weight[next] = weight[a] + weight[b];
        parent[a] = next;
        parent[b] = next;
    }

    //
    // A parent always has a greater index than its children, so depths can
    // be computed in one pass from the root down.
    //

    TVector<ui32> depth(nodeCount);
    for (size_t i = nodeCount - 1; i-- > 0;) {
        depth[i] = depth[parent[i]] + 1;
    }
    depth.resize(n);
    return depth;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

THuffmanCode THuffmanCode::Build(const THashMap<ui64, ui64>& frequencies)
{
    TVector<std::pair<ui64, ui64>> items;   // (weight, symbol)
    items.reserve(frequencies.size());
    for (const auto& [symbol, count]: frequencies) {
        items.emplace_back(Max<ui64>(count, 1), symbol);
    }
    Sort(items);

    TVector<ui64> weights(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        weights[i] = items[i].first;
    }

    //
    // Halve the weights until the longest codeword fits. Halving keeps the
    // weights in ascending order.
    //

    TVector<ui32> lengths;
    while (true) {
        lengths = ComputeCodeLengths(weights);
        if (lengths.empty() || *MaxElement(lengths.begin(), lengths.end())
                                   <= MaxCodeLength)
        {
            break;
        }
        for (auto& w: weights) {
            w = Max<ui64>(w >> 1, 1);
        }
    }

    TVector<std::pair<ui32, ui64>> canonical;   // (length, symbol)
    canonical.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        canonical.emplace_back(lengths[i], items[i].second);
    }
    Sort(canonical);

    THuffmanCode code;
    code.Symbols.reserve(canonical.size());
    for (const auto& [length, symbol]: canonical) {
        if (code.LengthCounts.size() <= length) {
            code.LengthCounts.resize(length + 1);
        }
        ++code.LengthCounts[length];
        code.Symbols.push_back(symbol);
    }

    const bool valid = code.Init();
    Y_ABORT_UNLESS(valid);

    code.Codewords.reserve(code.Symbols.size());
    for (size_t i = 0; i < canonical.size(); ++i) {
        const ui32 length = canonical[i].first;
        code.Codewords[code.Symbols[i]] = TCodeword{
            .Code = code.FirstCode[length] + (i - code.FirstIndex[length]),
            .Length = length,
        };
    }

    return code;
}

bool THuffmanCode::Init()
{
    if (Symbols.empty()) {
        LengthCounts.clear();
        MaxLength = 0;
        return true;
    }

    if (LengthCounts.empty() || LengthCounts.back() == 0) {
        return false;
    }
    MaxLength = LengthCounts.size() - 1;
    if (MaxLength > MaxCodeLength) {
        return false;
    }
    FirstCode.assign(MaxLength + 1, 0);
    FirstIndex.assign(MaxLength + 1, 0);

    //
    // Length 0 is valid only for a single-symbol code.
    //

    if (Symbols.size() == 1) {
        return MaxLength == 0;
    }
    if (LengthCounts[0] != 0) {
        return false;
    }

    ui64 code = 0;
    ui64 index = 0;
    for (ui32 length = 1; length <= MaxLength; ++length) {
        if (LengthCounts[length] > (1ULL << length)) {
            return false;
        }
        code = (code + LengthCounts[length - 1]) << 1;
        FirstCode[length] = code;
        FirstIndex[length] = index;
        index += LengthCounts[length];
        if (code + LengthCounts[length] > (1ULL << length)) {
            return false;
        }
    }

    if (index != Symbols.size()) {
        return false;
    }

    InitLookup();
    return true;
}

void THuffmanCode::InitLookup()
{
    //
    // Symbol indexes must fit in 26 bits of an entry; larger codes are
    // decoded without the table.
    //

    LookupBits = 0;
    Lookup.clear();
    if (Symbols.size() >= (1ULL << 26)) {
        return;
    }

    LookupBits = Min(MaxLength, MaxLookupBits);
    Lookup.assign(1ULL << LookupBits, 0);
    for (ui32 length = 1; length <= LookupBits; ++length) {
        const ui32 fill = LookupBits - length;
        for (ui64 i = 0; i < LengthCounts[length]; ++i) {
            const ui64 code = FirstCode[length] + i;
            const ui32 entry =
                static_cast<ui32>((FirstIndex[length] + i) << 6) | length;
            const ui64 begin = code << fill;
            const ui64 end = (code + 1) << fill;
            for (ui64 j = begin; j < end; ++j) {
                Lookup[j] = entry;
            }
        }
    }
}

TResultOrError<THuffmanCode> THuffmanCode::Load(TStringBuf data)
{
    ui64 symbolCount = 0;
    ui64 maxLength = 0;
    if (!ReadVarint(data, &symbolCount) || !ReadVarint(data, &maxLength)
        || maxLength > MaxCodeLength)
    {
        return MakeError(EBADMSG, "bad huffman table header");
    }

    THuffmanCode code;
    if (symbolCount == 0) {
        return code;
    }

    code.LengthCounts.assign(maxLength + 1, 0);
    if (symbolCount == 1) {
        code.LengthCounts[0] = 1;
    }
    for (ui64 length = 1; length <= maxLength; ++length) {
        if (!ReadVarint(data, &code.LengthCounts[length])) {
            return MakeError(EBADMSG, "bad huffman length counts");
        }
    }

    code.Symbols.reserve(Min<ui64>(symbolCount, data.size()));
    for (ui64 length = 0; length <= maxLength; ++length) {
        ui64 symbol = 0;
        for (ui64 i = 0; i < code.LengthCounts[length]; ++i) {
            ui64 delta = 0;
            if (!ReadVarint(data, &delta)) {
                return MakeError(EBADMSG, "bad huffman symbols");
            }
            symbol += delta;
            code.Symbols.push_back(symbol);
        }
    }

    if (!data.empty() || code.Symbols.size() != symbolCount || !code.Init()) {
        return MakeError(EBADMSG, "inconsistent huffman table");
    }

    return code;
}

TString THuffmanCode::Save() const
{
    TString out;
    WriteVarint(out, Symbols.size());
    WriteVarint(out, MaxLength);
    if (Symbols.empty()) {
        return out;
    }

    for (ui32 length = 1; length <= MaxLength; ++length) {
        WriteVarint(out, LengthCounts[length]);
    }

    size_t index = 0;
    for (ui32 length = 0; length <= MaxLength; ++length) {
        ui64 prev = 0;
        for (ui64 i = 0; i < LengthCounts[length]; ++i) {
            const ui64 symbol = Symbols[index++];
            WriteVarint(out, symbol - prev);
            prev = symbol;
        }
    }

    return out;
}

void THuffmanCode::Encode(ui64 value, TBitWriter& writer) const
{
    const auto* codeword = Codewords.FindPtr(value);
    Y_ABORT_UNLESS(codeword);
    writer.Write(codeword->Code, codeword->Length);
}

ui32 THuffmanCode::GetCodeLength(ui64 value) const
{
    const auto* codeword = Codewords.FindPtr(value);
    Y_ABORT_UNLESS(codeword);
    return codeword->Length;
}

bool THuffmanCode::Decode(TBitReader& reader, ui64* value) const
{
    if (Symbols.empty()) {
        return false;
    }
    if (MaxLength == 0) {
        *value = Symbols[0];
        return true;
    }

    //
    // Short codewords: one lookup by the next LookupBits bits.
    //

    const ui64 bits = reader.Peek(MaxLength);
    if (LookupBits) {
        const ui32 entry = Lookup[bits >> (MaxLength - LookupBits)];
        if (entry) {
            reader.Skip(entry & 63U);
            *value = Symbols[entry >> 6];
            return true;
        }
    }

    //
    // Long codewords: the first length whose prefix of the peeked bits is
    // a codeword of that length.
    //

    for (ui32 length = LookupBits + 1; length <= MaxLength; ++length) {
        const ui64 code = bits >> (MaxLength - length);
        const ui64 offset = code - FirstCode[length];
        if (code >= FirstCode[length] && offset < LengthCounts[length]) {
            reader.Skip(length);
            *value = Symbols[FirstIndex[length] + offset];
            return true;
        }
    }

    return false;
}

}   // namespace NCloud::NStorage::NLsmCompress
