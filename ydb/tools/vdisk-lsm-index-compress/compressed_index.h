#pragma once

#include "error.h"
#include "huffman.h"
#include "record.h"

#include <util/generic/map.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/system/filemap.h>

#include <array>
#include <compare>
#include <memory>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

/**
 * Fields coded with a Huffman code. The other fields are stored as is:
 * CrcMode 2 bits, PartId 4 bits.
 *
 * Record layout in the bit stream, in key order:
 * TabletId, Channel, Generation, Step, Cookie, CrcMode, BlobSize, PartId,
 * Ingress.
 */
enum class EField: ui32
{
    TabletId = 0,
    Generation = 1,
    Step = 2,
    Cookie = 3,
    BlobSize = 4,
    Ingress = 5,
    Channel = 6,
};

constexpr ui32 FieldCount = 7;

/**
 * @param field - Field.
 * @return - Field name.
 */
TStringBuf GetFieldName(EField field);

////////////////////////////////////////////////////////////////////////////////

/**
 * Which segments share one set of Huffman codes. Segments themselves are
 * always one per SST and one per fresh table of a VDisk.
 */
enum class EScope: ui32
{
    Global = 0,   // one code set for all segments
    VDisk = 1,    // one code set per VDisk
    Sst = 2,      // one code set per segment
};

/**
 * Parses a scope name: global, vdisk or sst.
 *
 * @param name - Scope name.
 * @param scope - (out) Parsed scope.
 * @return - False if the name is unknown.
 */
bool ParseScope(TStringBuf name, EScope* scope);

/**
 * @param scope - Scope.
 * @return - Scope name.
 */
TStringBuf GetScopeName(EScope scope);

/**
 * Segment identity: one SST, or one fresh table of a VDisk. For fresh
 * tables Level and SstId are zero.
 */
struct TSegmentId
{
    ui32 GroupId = 0;
    ui32 VDiskIndex = 0;
    ui32 TableType = 0;
    ui32 Level = 0;
    ui64 SstId = 0;

    friend auto operator<=>(const TSegmentId&, const TSegmentId&) = default;
};

using TSegments = TMap<TSegmentId, TVector<TLogoBlobRec>>;

/**
 * Huffman codes of all coded fields, in EField order.
 */
using TCodeSet = std::array<THuffmanCode, FieldCount>;

////////////////////////////////////////////////////////////////////////////////

/**
 * File layout, all integers are little-endian:
 *
 *  char[8]  Magic "VLSMHC05"
 *  ui32     Scope
 *  ui32     Reserved, zero
 *  ui64     CodeSetCount
 *  CodeSetCount x code set:
 *           7 x { ui64 size, bytes }, Huffman tables in EField order
 *  ui64     SegmentCount
 *  SegmentCount x directory entry, 48 bytes, ascending by segment id:
 *           ui32 GroupId, ui32 VDiskIndex, ui32 TableType, ui32 Level,
 *           ui64 SstId, ui32 code set number, ui32 reserved,
 *           ui64 file offset of the segment, ui64 its size
 *  segments
 *
 * Segment layout:
 *
 *  ui64     RecordCount
 *  ui32     BlockSize        records per offset anchor
 *  ui32     DeltaWidth       bits per packed delta
 *  ui64     DeltaBase        subtracted from every delta
 *  ui64     StreamBits       valid bits in the record stream
 *  ui64     AnchorCount, ui64[AnchorCount]
 *           bit offset of the first record of every block
 *  ui64     DeltaBytes, bytes
 *           for every record that does not start a block:
 *           offset(i) - offset(i - 1) - DeltaBase, DeltaWidth bits each
 *  ui64     StreamBytes, bytes
 *           records, sorted by key and ingress
 */
constexpr TStringBuf IndexMagic = "VLSMHC05";
constexpr ui32 IndexBlockSize = 64;
constexpr ui64 DirectoryEntrySize = 48;

/**
 * Sizes summed over all segments.
 */
struct TCompressStats
{
    ui64 RecordCount = 0;
    ui64 SegmentCount = 0;
    ui64 CodeSetCount = 0;
    std::array<ui64, FieldCount> SymbolCount = {};
    std::array<ui32, FieldCount> MaxCodeLength = {};
    std::array<ui64, FieldCount> TableBytes = {};
    std::array<ui64, FieldCount> CodedBits = {};
    ui64 StreamBytes = 0;
    ui64 AnchorBytes = 0;
    ui64 DeltaBytes = 0;
    ui32 MaxDeltaWidth = 0;
    ui64 DirectoryBytes = 0;
    ui64 FileBytes = 0;
};

/**
 * Builds one Huffman code set per scope unit and writes the index file.
 *
 * @param path - Output file path.
 * @param scope - Which segments share a code set.
 * @param segments - Records of every segment, sorted by key and ingress.
 * @param stats - (out) Sizes of the parts of the file.
 * @return - Error if the file cannot be written.
 */
TError WriteCompressedIndex(
    const TString& path,
    EScope scope,
    const TSegments& segments,
    TCompressStats* stats);

////////////////////////////////////////////////////////////////////////////////

/**
 * Read-only view of one segment of a mapped index file.
 */
class TIndexSegment
{
private:
    ui64 RecordCount = 0;
    ui32 BlockSize = 0;
    ui32 DeltaWidth = 0;
    ui64 DeltaBase = 0;
    const TCodeSet* Codes = nullptr;
    const char* Anchors = nullptr;
    ui64 AnchorCount = 0;
    const ui8* Deltas = nullptr;
    ui64 DeltaBits = 0;
    const ui8* Stream = nullptr;
    ui64 StreamBits = 0;

public:
    /**
     * Checks the segment layout and sets up the view. The data and the
     * codes must stay alive while the view is used.
     *
     * @param data - Segment bytes.
     * @param codes - Code set of the segment.
     * @return - Error if the segment is malformed.
     */
    TError Parse(TStringBuf data, const TCodeSet& codes);

    /**
     * @return - Number of records.
     */
    ui64 GetRecordCount() const
    {
        return RecordCount;
    }

    /**
     * Computes the bit offset of a record from its block anchor and the
     * packed deltas.
     *
     * @param index - Record index, at most GetRecordCount(); the value
     *  GetRecordCount() gives the end of the stream.
     * @return - Bit offset of the record in the stream.
     */
    ui64 GetRecordOffset(ui64 index) const;

    /**
     * Decodes the record at a bit offset.
     *
     * @param offset - Bit offset of the record.
     * @param rec - (out) Decoded record.
     * @param nextOffset - (out) Bit offset right after the record.
     * @return - False if the stream is corrupted.
     */
    bool ReadRecordAt(ui64 offset, TLogoBlobRec* rec, ui64* nextOffset) const;

    /**
     * Decodes a record by index.
     *
     * @param index - Record index.
     * @param rec - (out) Decoded record.
     * @return - False if the stream is corrupted.
     */
    bool ReadRecord(ui64 index, TLogoBlobRec* rec) const;

    /**
     * Finds the records with a given key: binary search over the first
     * records of the blocks, then over the records of one block, then a
     * scan over the records with the same key. Records with the same key
     * differ in ingress and are adjacent.
     *
     * @param key - Key to find; ingress is ignored.
     * @param probes - (out) Number of records compared with the key.
     * @return - Range [begin, end) of matching record indexes; empty if the
     *  key is absent, begin is then the insertion position.
     */
    TResultOrError<std::pair<ui64, ui64>> FindKey(
        const TLogoBlobRec& key,
        ui64* probes) const;

private:
    using TBlockOffsets = std::array<ui64, IndexBlockSize>;

    const THuffmanCode& GetCode(EField field) const
    {
        return (*Codes)[static_cast<ui32>(field)];
    }

    /**
     * Compares the key of the record at a bit offset with a given key field
     * by field, stops decoding at the first differing field.
     */
    bool CompareKeyAt(
        ui64 offset,
        const TLogoBlobRec& key,
        std::strong_ordering* result) const;

    ui64 GetAnchor(ui64 block) const;

    /**
     * Computes the bit offsets of all records of a block.
     *
     * @return - Number of records in the block.
     */
    ui64 GetBlockOffsets(ui64 block, TBlockOffsets* offsets) const;
};

////////////////////////////////////////////////////////////////////////////////

/**
 * Read-only view of an index file mapped into memory.
 */
class TCompressedIndex
{
private:
    TFileMap File;
    EScope Scope = EScope::Global;
    TVector<TCodeSet> CodeSets;
    TVector<TSegmentId> SegmentIds;
    TVector<TIndexSegment> Segments;

public:
    explicit TCompressedIndex(const TString& path);

    /**
     * Maps the file and checks its layout.
     *
     * @param path - Index file path.
     * @return - The index or an error if the file is unreadable or malformed.
     */
    static TResultOrError<std::unique_ptr<TCompressedIndex>> Open(
        const TString& path);

    /**
     * @return - Scope the file was written with.
     */
    EScope GetScope() const
    {
        return Scope;
    }

    /**
     * @return - Number of Huffman code sets.
     */
    size_t GetCodeSetCount() const
    {
        return CodeSets.size();
    }

    /**
     * @return - Number of segments.
     */
    size_t GetSegmentCount() const
    {
        return Segments.size();
    }

    /**
     * @param i - Segment number, in the order of the directory.
     * @return - Segment identity.
     */
    const TSegmentId& GetSegmentId(size_t i) const
    {
        return SegmentIds[i];
    }

    /**
     * @param i - Segment number, in the order of the directory.
     * @return - Segment view.
     */
    const TIndexSegment& GetSegment(size_t i) const
    {
        return Segments[i];
    }

private:
    TError Parse();
};

}   // namespace NCloud::NStorage::NLsmCompress
