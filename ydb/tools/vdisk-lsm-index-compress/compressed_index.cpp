#include "compressed_index.h"

#include <util/generic/array_ref.h>
#include <util/generic/bitops.h>
#include <util/stream/file.h>
#include <util/stream/str.h>
#include <util/string/builder.h>
#include <util/system/fstat.h>
#include <util/system/unaligned_mem.h>

namespace NCloud::NStorage::NLsmCompress {

namespace {

////////////////////////////////////////////////////////////////////////////////

constexpr std::array<EField, FieldCount> AllFields = {
    EField::TabletId,
    EField::Generation,
    EField::Step,
    EField::Cookie,
    EField::BlobSize,
    EField::Ingress,
    EField::Channel,
};

ui64 GetField(const TLogoBlobRec& rec, EField field)
{
    switch (field) {
        case EField::TabletId:
            return rec.TabletId();
        case EField::Generation:
            return rec.Generation();
        case EField::Step:
            return rec.Step();
        case EField::Cookie:
            return rec.Cookie();
        case EField::BlobSize:
            return rec.BlobSize();
        case EField::Ingress:
            return rec.Ingress;
        case EField::Channel:
            return rec.Channel();
    }
    return 0;
}

ui32 BitWidth(ui64 value)
{
    return value ? GetValueBitCount(value) : 0;
}

ui64 DivCeil(ui64 a, ui64 b)
{
    return (a + b - 1) / b;
}

template <typename T>
void WritePod(IOutputStream& out, T value)
{
    out.Write(&value, sizeof(value));
}

void WriteBlob(IOutputStream& out, TStringBuf data)
{
    WritePod<ui64>(out, data.size());
    out.Write(data.data(), data.size());
}

template <typename T>
bool ReadPod(TStringBuf& in, T* value)
{
    if (in.size() < sizeof(T)) {
        return false;
    }
    *value = ReadUnaligned<T>(in.data());
    in.Skip(sizeof(T));
    return true;
}

bool ReadBlob(TStringBuf& in, TStringBuf* data)
{
    ui64 size = 0;
    if (!ReadPod(in, &size) || size > in.size()) {
        return false;
    }
    *data = in.Head(size);
    in.Skip(size);
    return true;
}

////////////////////////////////////////////////////////////////////////////////

class TRecordEncoder
{
private:
    const TCodeSet& Codes;
    TCompressStats& Stats;

public:
    TRecordEncoder(
            const TCodeSet& codes,
            TCompressStats& stats)
        : Codes(codes)
        , Stats(stats)
    {}

    void Encode(const TLogoBlobRec& rec, TBitWriter& writer)
    {
        EncodeField(rec, EField::TabletId, writer);
        EncodeField(rec, EField::Channel, writer);
        EncodeField(rec, EField::Generation, writer);
        EncodeField(rec, EField::Step, writer);
        EncodeField(rec, EField::Cookie, writer);
        WriteRaw(rec.CrcMode(), CrcModeBits, writer);
        EncodeField(rec, EField::BlobSize, writer);
        WriteRaw(rec.PartId(), PartIdBits, writer);
        EncodeField(rec, EField::Ingress, writer);
    }

private:
    void EncodeField(const TLogoBlobRec& rec, EField field, TBitWriter& writer)
    {
        const auto i = static_cast<ui32>(field);
        const ui64 before = writer.GetBitCount();
        Codes[i].Encode(GetField(rec, field), writer);
        Stats.CodedBits[i] += writer.GetBitCount() - before;
    }

    static void WriteRaw(ui64 value, ui32 bits, TBitWriter& writer)
    {
        writer.Write(value, bits);
    }
};

////////////////////////////////////////////////////////////////////////////////

/**
 * Builds one Huffman code per coded field over the records of several
 * segments, appends the saved tables and adds their sizes to the stats.
 */
TCodeSet BuildCodeSet(
    const TVector<const TVector<TLogoBlobRec>*>& members,
    TString* saved,
    TCompressStats& stats)
{
    TCodeSet codes;
    TStringOutput out(*saved);
    for (const auto field: AllFields) {
        const auto i = static_cast<ui32>(field);
        THashMap<ui64, ui64> frequencies;
        for (const auto* records: members) {
            for (const auto& rec: *records) {
                ++frequencies[GetField(rec, field)];
            }
        }
        codes[i] = THuffmanCode::Build(frequencies);
        const auto table = codes[i].Save();
        WriteBlob(out, table);
        stats.SymbolCount[i] += codes[i].GetSymbolCount();
        stats.MaxCodeLength[i] =
            Max(stats.MaxCodeLength[i], codes[i].GetMaxLength());
        stats.TableBytes[i] += table.size();
    }
    return codes;
}

/**
 * Encodes one segment with a code set and adds its sizes to the stats.
 */
void EncodeSegment(
    TArrayRef<const TLogoBlobRec> records,
    const TCodeSet& codes,
    TString* segment,
    TCompressStats& stats)
{
    //
    // Encode the records and remember the bit offset of each one.
    //

    TBitWriter stream;
    TVector<ui64> offsets;
    offsets.reserve(records.size() + 1);
    TRecordEncoder encoder(codes, stats);
    for (const auto& rec: records) {
        offsets.push_back(stream.GetBitCount());
        encoder.Encode(rec, stream);
    }
    offsets.push_back(stream.GetBitCount());

    //
    // Pack the offsets: an absolute anchor per block of IndexBlockSize
    // records, and for the other records the difference with the previous
    // offset minus the smallest such difference.
    //

    ui64 minDelta = Max<ui64>();
    ui64 maxDelta = 0;
    for (size_t i = 1; i < records.size(); ++i) {
        if (i % IndexBlockSize) {
            const ui64 delta = offsets[i] - offsets[i - 1];
            minDelta = Min(minDelta, delta);
            maxDelta = Max(maxDelta, delta);
        }
    }
    if (minDelta > maxDelta) {
        minDelta = 0;
    }
    const ui32 deltaWidth = BitWidth(maxDelta - minDelta);

    TVector<ui64> anchors;
    TBitWriter deltas;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i % IndexBlockSize == 0) {
            anchors.push_back(offsets[i]);
        } else {
            deltas.Write(offsets[i] - offsets[i - 1] - minDelta, deltaWidth);
        }
    }

    stats.RecordCount += records.size();
    stats.StreamBytes += stream.GetData().size();
    stats.AnchorBytes += anchors.size() * sizeof(ui64);
    stats.DeltaBytes += deltas.GetData().size();
    stats.MaxDeltaWidth = Max(stats.MaxDeltaWidth, deltaWidth);

    TStringOutput out(*segment);
    WritePod<ui64>(out, records.size());
    WritePod<ui32>(out, IndexBlockSize);
    WritePod<ui32>(out, deltaWidth);
    WritePod<ui64>(out, minDelta);
    WritePod<ui64>(out, stream.GetBitCount());
    WritePod<ui64>(out, anchors.size());
    for (const ui64 anchor: anchors) {
        WritePod<ui64>(out, anchor);
    }
    WriteBlob(out, deltas.GetData());
    WriteBlob(out, stream.GetData());
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

TStringBuf GetFieldName(EField field)
{
    switch (field) {
        case EField::TabletId:
            return "TabletId";
        case EField::Generation:
            return "Generation";
        case EField::Step:
            return "Step";
        case EField::Cookie:
            return "Cookie";
        case EField::BlobSize:
            return "BlobSize";
        case EField::Ingress:
            return "Ingress";
        case EField::Channel:
            return "Channel";
    }
    return "Unknown";
}

bool ParseScope(TStringBuf name, EScope* scope)
{
    if (name == "global") {
        *scope = EScope::Global;
    } else if (name == "vdisk") {
        *scope = EScope::VDisk;
    } else if (name == "sst") {
        *scope = EScope::Sst;
    } else {
        return false;
    }
    return true;
}

TStringBuf GetScopeName(EScope scope)
{
    switch (scope) {
        case EScope::Global:
            return "global";
        case EScope::VDisk:
            return "vdisk";
        case EScope::Sst:
            return "sst";
    }
    return "unknown";
}

TError WriteCompressedIndex(
    const TString& path,
    EScope scope,
    const TSegments& segments,
    TCompressStats* stats)
{
    *stats = {};
    stats->SegmentCount = segments.size();

    //
    // Group the segments by scope unit: all of them, one VDisk or one
    // segment. Every unit gets one code set.
    //

    auto getUnit = [scope](const TSegmentId& id)
    {
        TSegmentId unit;
        switch (scope) {
            case EScope::Global:
                break;
            case EScope::VDisk:
                unit.GroupId = id.GroupId;
                unit.VDiskIndex = id.VDiskIndex;
                break;
            case EScope::Sst:
                unit = id;
                break;
        }
        return unit;
    };

    TMap<TSegmentId, ui32> unitNumbers;
    TVector<TVector<const TVector<TLogoBlobRec>*>> units;
    TVector<ui32> segmentCodeSets;
    for (const auto& [id, records]: segments) {
        const auto [it, inserted] =
            unitNumbers.emplace(getUnit(id), units.size());
        if (inserted) {
            units.emplace_back();
        }
        units[it->second].push_back(&records);
        segmentCodeSets.push_back(it->second);
    }

    TString codeSetData;
    TVector<TCodeSet> codeSets;
    codeSets.reserve(units.size());
    for (const auto& members: units) {
        codeSets.push_back(BuildCodeSet(members, &codeSetData, *stats));
    }
    stats->CodeSetCount = codeSets.size();

    TVector<TString> encoded;
    encoded.reserve(segments.size());
    size_t n = 0;
    for (const auto& [id, records]: segments) {
        EncodeSegment(
            records,
            codeSets[segmentCodeSets[n]],
            &encoded.emplace_back(),
            *stats);
        ++n;
    }

    //
    // Segments follow the directory in the same order.
    //

    const ui64 headerBytes = IndexMagic.size() + 2 * sizeof(ui32)
        + sizeof(ui64) + codeSetData.size() + sizeof(ui64);
    stats->DirectoryBytes = segments.size() * DirectoryEntrySize;
    ui64 segmentOffset = headerBytes + stats->DirectoryBytes;

    //
    // TFileOutput reports failures by exceptions. They are converted to an
    // error here.
    //

    try {
        TFileOutput out(path);
        out.Write(IndexMagic.data(), IndexMagic.size());
        WritePod<ui32>(out, static_cast<ui32>(scope));
        WritePod<ui32>(out, 0 /* reserved */);
        WritePod<ui64>(out, codeSets.size());
        out.Write(codeSetData.data(), codeSetData.size());
        WritePod<ui64>(out, segments.size());

        size_t i = 0;
        for (const auto& [id, records]: segments) {
            WritePod<ui32>(out, id.GroupId);
            WritePod<ui32>(out, id.VDiskIndex);
            WritePod<ui32>(out, id.TableType);
            WritePod<ui32>(out, id.Level);
            WritePod<ui64>(out, id.SstId);
            WritePod<ui32>(out, segmentCodeSets[i]);
            WritePod<ui32>(out, 0 /* reserved */);
            WritePod<ui64>(out, segmentOffset);
            WritePod<ui64>(out, encoded[i].size());
            segmentOffset += encoded[i].size();
            ++i;
        }

        for (const auto& segment: encoded) {
            out.Write(segment.data(), segment.size());
        }

        out.Finish();
        stats->FileBytes = GetFileLength(path);
    } catch (...) {
        return MakeErrorFromCurrentException();
    }

    return {};
}

////////////////////////////////////////////////////////////////////////////////

TCompressedIndex::TCompressedIndex(const TString& path)
    : File(path)
{}

TResultOrError<std::unique_ptr<TCompressedIndex>> TCompressedIndex::Open(
    const TString& path)
{
    std::unique_ptr<TCompressedIndex> index;

    //
    // TFileMap reports failures by exceptions. They are converted to an
    // error here.
    //

    try {
        index = std::make_unique<TCompressedIndex>(path);
        index->File.Map(0 /* offset */, index->File.Length());
    } catch (...) {
        return MakeErrorFromCurrentException();
    }

    auto error = index->Parse();
    if (HasError(error)) {
        return error;
    }
    return index;
}

TError TCompressedIndex::Parse()
{
    const TStringBuf file(
        static_cast<const char*>(File.Ptr()),
        File.MappedSize());
    TStringBuf data = file;
    auto bad = [](TStringBuf what)
    {
        return MakeError(
            EBADMSG,
            TStringBuilder() << "malformed index file: " << what);
    };

    ui32 scope = 0;
    ui32 reserved = 0;
    ui64 codeSetCount = 0;
    if (!data.SkipPrefix(IndexMagic)) {
        return bad("bad magic");
    }
    if (!ReadPod(data, &scope) || !ReadPod(data, &reserved)
        || !ReadPod(data, &codeSetCount))
    {
        return bad("truncated header");
    }
    if (scope > static_cast<ui32>(EScope::Sst) || reserved != 0) {
        return bad("bad header values");
    }
    if (codeSetCount > data.size() / (FieldCount * sizeof(ui64))) {
        return bad("bad code set count");
    }
    Scope = static_cast<EScope>(scope);

    //
    // Code sets are loaded before the segments, which keep pointers to
    // them; the vector is not resized afterwards.
    //

    CodeSets.resize(codeSetCount);
    for (auto& codes: CodeSets) {
        for (const auto field: AllFields) {
            TStringBuf table;
            if (!ReadBlob(data, &table)) {
                return bad("truncated huffman table");
            }
            auto [code, error] = THuffmanCode::Load(table);
            if (HasError(error)) {
                return error;
            }
            codes[static_cast<ui32>(field)] = std::move(code);
        }
    }

    ui64 segmentCount = 0;
    if (!ReadPod(data, &segmentCount)
        || segmentCount > data.size() / DirectoryEntrySize)
    {
        return bad("bad segment count");
    }

    SegmentIds.resize(segmentCount);
    Segments.resize(segmentCount);
    for (ui64 i = 0; i < segmentCount; ++i) {
        auto& id = SegmentIds[i];
        ui32 codeSet = 0;
        ui32 entryReserved = 0;
        ui64 offset = 0;
        ui64 size = 0;
        if (!ReadPod(data, &id.GroupId) || !ReadPod(data, &id.VDiskIndex)
            || !ReadPod(data, &id.TableType) || !ReadPod(data, &id.Level)
            || !ReadPod(data, &id.SstId) || !ReadPod(data, &codeSet)
            || !ReadPod(data, &entryReserved) || !ReadPod(data, &offset)
            || !ReadPod(data, &size))
        {
            return bad("truncated directory");
        }

        if (i && !(SegmentIds[i - 1] < id)) {
            return bad("segments are not sorted");
        }
        if (codeSet >= CodeSets.size() || entryReserved != 0) {
            return bad("bad directory entry");
        }
        if (offset > file.size() || size > file.size() - offset) {
            return bad("segment out of file");
        }

        auto error =
            Segments[i].Parse(file.SubStr(offset, size), CodeSets[codeSet]);
        if (HasError(error)) {
            return error;
        }
    }

    return {};
}

TError TIndexSegment::Parse(TStringBuf data, const TCodeSet& codes)
{
    auto bad = [](TStringBuf what)
    {
        return MakeError(
            EBADMSG,
            TStringBuilder() << "malformed index segment: " << what);
    };

    if (!ReadPod(data, &RecordCount) || !ReadPod(data, &BlockSize)
        || !ReadPod(data, &DeltaWidth) || !ReadPod(data, &DeltaBase)
        || !ReadPod(data, &StreamBits))
    {
        return bad("truncated header");
    }
    if (BlockSize != IndexBlockSize || DeltaWidth > 64) {
        return bad("bad header values");
    }

    Codes = &codes;

    if (!ReadPod(data, &AnchorCount)
        || AnchorCount != DivCeil(RecordCount, BlockSize)
        || AnchorCount > data.size() / sizeof(ui64))
    {
        return bad("bad anchors");
    }
    Anchors = data.data();
    data.Skip(AnchorCount * sizeof(ui64));

    TStringBuf deltas;
    DeltaBits = (RecordCount - AnchorCount) * DeltaWidth;
    if (!ReadBlob(data, &deltas) || deltas.size() < DivCeil(DeltaBits, 8)) {
        return bad("bad deltas");
    }
    Deltas = reinterpret_cast<const ui8*>(deltas.data());

    TStringBuf stream;
    if (!ReadBlob(data, &stream) || stream.size() < DivCeil(StreamBits, 8)) {
        return bad("bad record stream");
    }
    Stream = reinterpret_cast<const ui8*>(stream.data());

    if (!data.empty()) {
        return bad("trailing bytes");
    }
    if (RecordCount && GetAnchor(0) != 0) {
        return bad("first anchor is not zero");
    }

    return {};
}

ui64 TIndexSegment::GetAnchor(ui64 block) const
{
    return ReadUnaligned<ui64>(Anchors + block * sizeof(ui64));
}

ui64 TIndexSegment::GetRecordOffset(ui64 index) const
{
    if (index >= RecordCount) {
        return StreamBits;
    }

    //
    // Record b * BlockSize + k has its delta at position
    // b * (BlockSize - 1) + k - 1 of the packed array.
    //

    const ui64 block = index / BlockSize;
    const ui64 first = block * BlockSize;
    ui64 offset = GetAnchor(block);

    TBitReader reader(
        Deltas,
        DeltaBits,
        block * (BlockSize - 1) * static_cast<ui64>(DeltaWidth));
    for (ui64 i = first + 1; i <= index; ++i) {
        offset += reader.Read(DeltaWidth) + DeltaBase;
    }
    return offset;
}

bool TIndexSegment::ReadRecordAt(
    ui64 offset,
    TLogoBlobRec* rec,
    ui64* nextOffset) const
{
    TBitReader reader(Stream, StreamBits, offset);
    ui64 tabletId = 0;
    ui64 channel = 0;
    ui64 generation = 0;
    ui64 step = 0;
    ui64 cookie = 0;
    ui64 blobSize = 0;
    ui64 ingress = 0;

    if (!GetCode(EField::TabletId).Decode(reader, &tabletId)
        || !GetCode(EField::Channel).Decode(reader, &channel)
        || !GetCode(EField::Generation).Decode(reader, &generation)
        || !GetCode(EField::Step).Decode(reader, &step)
        || !GetCode(EField::Cookie).Decode(reader, &cookie))
    {
        return false;
    }
    const ui64 crcMode = reader.Read(CrcModeBits);
    if (!GetCode(EField::BlobSize).Decode(reader, &blobSize)) {
        return false;
    }
    const ui64 partId = reader.Read(PartIdBits);
    if (!GetCode(EField::Ingress).Decode(reader, &ingress)) {
        return false;
    }

    if (reader.Overrun()) {
        return false;
    }

    *rec = TLogoBlobRec::Make(
        tabletId,
        generation,
        step,
        channel,
        cookie,
        blobSize,
        crcMode,
        partId,
        ingress);
    *nextOffset = reader.GetPosition();
    return true;
}

bool TIndexSegment::ReadRecord(ui64 index, TLogoBlobRec* rec) const
{
    ui64 nextOffset = 0;
    return ReadRecordAt(GetRecordOffset(index), rec, &nextOffset);
}

bool TIndexSegment::CompareKeyAt(
    ui64 offset,
    const TLogoBlobRec& key,
    std::strong_ordering* result) const
{
    TBitReader reader(Stream, StreamBits, offset);

    //
    // Fields are stored in key order, so the first differing field decides
    // the result and the rest of the record is not decoded.
    //

    auto differs = [&](ui64 value, ui64 expected)
    {
        *result = value <=> expected;
        return *result != std::strong_ordering::equal;
    };

    ui64 value = 0;
    if (!GetCode(EField::TabletId).Decode(reader, &value)) {
        return false;
    }
    if (differs(value, key.TabletId())) {
        return true;
    }
    if (!GetCode(EField::Channel).Decode(reader, &value)) {
        return false;
    }
    if (differs(value, key.Channel())) {
        return true;
    }
    if (!GetCode(EField::Generation).Decode(reader, &value)) {
        return false;
    }
    if (differs(value, key.Generation())) {
        return true;
    }
    if (!GetCode(EField::Step).Decode(reader, &value)) {
        return false;
    }
    if (differs(value, key.Step())) {
        return true;
    }
    if (!GetCode(EField::Cookie).Decode(reader, &value)) {
        return false;
    }
    if (differs(value, key.Cookie())) {
        return true;
    }
    if (differs(reader.Read(CrcModeBits), key.CrcMode())) {
        return true;
    }
    if (!GetCode(EField::BlobSize).Decode(reader, &value)) {
        return false;
    }
    if (differs(value, key.BlobSize())) {
        return true;
    }
    differs(reader.Read(PartIdBits), key.PartId());

    return !reader.Overrun();
}

ui64 TIndexSegment::GetBlockOffsets(ui64 block, TBlockOffsets* offsets) const
{
    const ui64 first = block * BlockSize;
    const ui64 count = Min<ui64>(BlockSize, RecordCount - first);

    TBitReader reader(
        Deltas,
        DeltaBits,
        block * (BlockSize - 1) * static_cast<ui64>(DeltaWidth));
    (*offsets)[0] = GetAnchor(block);
    for (ui64 i = 1; i < count; ++i) {
        (*offsets)[i] = (*offsets)[i - 1] + reader.Read(DeltaWidth) + DeltaBase;
    }
    return count;
}

TResultOrError<std::pair<ui64, ui64>> TIndexSegment::FindKey(
    const TLogoBlobRec& key,
    ui64* probes) const
{
    *probes = 0;
    auto compare = [&](ui64 offset, std::strong_ordering* cmp)
    {
        ++*probes;
        return CompareKeyAt(offset, key, cmp);
    };
    auto corrupted = [](ui64 offset)
    {
        return MakeError(
            EBADMSG,
            TStringBuilder() << "corrupted record at bit " << offset);
    };

    //
    // Find the first block whose first record is not less than the key.
    // First records are reached directly through the anchors.
    //

    ui64 lo = 0;
    ui64 hi = AnchorCount;
    while (lo < hi) {
        const ui64 mid = lo + (hi - lo) / 2;
        auto cmp = std::strong_ordering::equal;
        if (!compare(GetAnchor(mid), &cmp)) {
            return corrupted(GetAnchor(mid));
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    //
    // The lower bound is either the first record of that block or one of
    // the records after the first one of the previous block. Offsets of
    // the previous block are computed once and searched by binary search.
    //

    TBlockOffsets offsets;
    ui64 offsetsFirst = 0;
    ui64 offsetsCount = 0;
    ui64 begin = Min(lo * BlockSize, RecordCount);
    if (lo > 0) {
        const ui64 block = lo - 1;
        offsetsFirst = block * BlockSize;
        offsetsCount = GetBlockOffsets(block, &offsets);

        ui64 l = 1;
        ui64 h = offsetsCount;
        while (l < h) {
            const ui64 mid = l + (h - l) / 2;
            auto cmp = std::strong_ordering::equal;
            if (!compare(offsets[mid], &cmp)) {
                return corrupted(offsets[mid]);
            }
            if (cmp < 0) {
                l = mid + 1;
            } else {
                h = mid;
            }
        }
        begin = offsetsFirst + l;
    }

    //
    // Records with the same key follow the lower bound; scan them.
    //

    ui64 end = begin;
    while (end < RecordCount) {
        const ui64 offset = end - offsetsFirst < offsetsCount
            ? offsets[end - offsetsFirst]
            : GetRecordOffset(end);
        auto cmp = std::strong_ordering::equal;
        if (!compare(offset, &cmp)) {
            return corrupted(offset);
        }
        if (cmp != 0) {
            break;
        }
        ++end;
    }

    return std::make_pair(begin, end);
}

}   // namespace NCloud::NStorage::NLsmCompress
