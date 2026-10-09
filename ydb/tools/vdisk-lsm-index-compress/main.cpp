#include "compressed_index.h"
#include "record.h"
#include "snapshot.h"

#include <library/cpp/getopt/small/last_getopt.h>
#include <library/cpp/getopt/small/modchooser.h>

#include <util/generic/algorithm.h>
#include <util/generic/array_ref.h>
#include <util/generic/scope.h>
#include <util/random/fast.h>
#include <util/stream/file.h>
#include <util/stream/output.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/printf.h>
#include <util/system/filemap.h>
#include <util/system/fs.h>
#include <util/system/fstat.h>

#include <chrono>
#include <functional>
#include <optional>
#include <tuple>

namespace NCloud::NStorage::NLsmCompress {

namespace {

////////////////////////////////////////////////////////////////////////////////

constexpr int ExitFound = 0;
constexpr int ExitNotFound = 1;
constexpr int ExitError = 2;

////////////////////////////////////////////////////////////////////////////////

struct TSnapshotOptions
{
    TString SnapshotPath;
    TString Table = "all";
    bool NoValidate = false;

    void Register(NLastGetopt::TOpts& opts)
    {
        opts.AddLongOption("snapshot", "ydb-dstool take-snapshot file")
            .RequiredArgument("PATH")
            .Required()
            .StoreResult(&SnapshotPath);

        opts.AddLongOption(
                "table",
                "take records of one table type only: "
                "all, fcur, fdreg, fold, level")
            .RequiredArgument("STR")
            .StoreResult(&Table);

        opts.AddLongOption(
                "no-validate",
                "skip VDisk stream sequence checks")
            .NoArgument()
            .SetFlag(&NoValidate);
    }
};

double PerRecord(double value, ui64 recordCount)
{
    return recordCount ? value / recordCount : 0;
}

TSegmentId MakeSegmentId(const TBlockInfo& block)
{
    TSegmentId id{
        .GroupId = block.GroupId,
        .VDiskIndex = block.VDiskIndex,
        .TableType = static_cast<ui32>(block.TableType),
    };
    if (block.TableType == ETableType::Level) {
        id.Level = block.Level;
        id.SstId = block.SstId;
    }
    return id;
}

TString FormatSegment(const TSegmentId& id)
{
    TStringBuilder out;
    out << "vdisk " << id.GroupId << ":" << id.VDiskIndex;
    const auto tableType = static_cast<ETableType>(id.TableType);
    if (tableType == ETableType::Level) {
        out << " level " << id.Level << " sst " << id.SstId;
    } else {
        out << " " << GetTableTypeName(tableType);
    }
    return out;
}

/**
 * Reads LogoBlob records from a snapshot, splits them into segments, one
 * per SST and one per fresh table of a VDisk, and sorts every segment by
 * key and ingress.
 */
TError LoadSegments(
    const TSnapshotOptions& options,
    TSegments* segments,
    TSnapshotStats* stats)
{
    std::optional<ETableType> table;
    if (options.Table != "all") {
        ETableType tableType = ETableType::FCur;
        if (!ParseTableType(options.Table, &tableType)) {
            return MakeError(
                EINVAL,
                "unknown table type: " + options.Table);
        }
        table = tableType;
    }

    //
    // Records of one block go to one segment, so the segment found for the
    // previous record is reused while the id does not change.
    //

    TSegmentId lastId;
    TVector<TLogoBlobRec>* last = nullptr;
    auto error = ReadSnapshot(
        options.SnapshotPath,
        !options.NoValidate,
        [&](const TBlockInfo& block, const TLogoBlobRec& rec)
        {
            if (table && block.TableType != *table) {
                return;
            }
            const auto id = MakeSegmentId(block);
            if (!last || id != lastId) {
                last = &(*segments)[id];
                lastId = id;
            }
            last->push_back(rec);
        },
        stats);
    if (HasError(error)) {
        return error;
    }

    for (auto& [id, records]: *segments) {
        Sort(records);
    }
    return {};
}

int PrintError(const TError& error)
{
    Cerr << "error: " << FormatError(error) << Endl;
    return ExitError;
}

////////////////////////////////////////////////////////////////////////////////

/**
 * Text table: the first column is aligned left, the others right.
 */
class TReportTable
{
private:
    TVector<TVector<TString>> Rows;

public:
    void AddRow(TVector<TString> row)
    {
        Rows.push_back(std::move(row));
    }

    void AddSeparator()
    {
        Rows.emplace_back();
    }

    void Print(IOutputStream& out) const
    {
        TVector<size_t> widths;
        for (const auto& row: Rows) {
            widths.resize(Max(widths.size(), row.size()));
            for (size_t i = 0; i < row.size(); ++i) {
                widths[i] = Max(widths[i], row[i].size());
            }
        }

        size_t lineWidth = 0;
        for (const size_t width: widths) {
            lineWidth += width + 2;
        }

        for (const auto& row: Rows) {
            out << "  ";
            if (row.empty()) {
                out << TString(lineWidth - 2, '-') << '\n';
                continue;
            }
            for (size_t i = 0; i < row.size(); ++i) {
                if (i) {
                    out << "  ";
                }
                const TString pad(widths[i] - row[i].size(), ' ');
                if (i == 0) {
                    out << row[i] << pad;
                } else {
                    out << pad << row[i];
                }
            }
            out << '\n';
        }
    }
};

TString FormatCount(ui64 value)
{
    TString digits = ToString(value);
    TString result;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) {
            result.push_back(',');
        }
        result.push_back(digits[i]);
    }
    return result;
}

TString FormatMiB(double bytes)
{
    return Sprintf("%.2f MiB", bytes / (1024 * 1024));
}

TString FormatBitsPerRecord(double bits, ui64 recordCount)
{
    return Sprintf("%.2f", PerRecord(bits, recordCount));
}

TString FormatRatio(double from, double to)
{
    return to > 0 ? Sprintf("%.2f", from / to) : TString("-");
}

void PrintCompressReport(
    const TString& snapshotPath,
    const TSnapshotStats& snapshotStats,
    EScope scope,
    const TSegments& segments,
    const TString& outputPath,
    const TCompressStats& stats)
{
    const ui64 n = stats.RecordCount;
    const double rawBytes = static_cast<double>(n) * sizeof(TLogoBlobRec);
    const i64 snapshotBytes = GetFileLength(snapshotPath);
    const double fileBits = 8.0 * stats.FileBytes;

    //
    // Input.
    //

    Cout << "Input: " << snapshotPath << "\n\n";
    TReportTable input;
    input.AddRow({
        "snapshot file (bzip2 pages)",
        snapshotBytes < 0 ? TString("-") : FormatMiB(snapshotBytes)});
    input.AddRow({
        "unpacked pages",
        FormatMiB(snapshotStats.UncompressedPageBytes)});
    input.AddRow({
        "LogoBlob records in snapshot",
        FormatCount(snapshotStats.LogoBlobRecords)});
    input.AddRow({"records taken", FormatCount(n)});
    input.AddRow({
        Sprintf("records taken, %zu B each", sizeof(TLogoBlobRec)),
        FormatMiB(rawBytes)});
    input.Print(Cout);

    //
    // Segments.
    //

    TVector<ui64> segmentSizes;
    for (const auto& [id, records]: segments) {
        segmentSizes.push_back(records.size());
    }
    Sort(segmentSizes);

    Cout << "\nSegments: one per SST and per fresh table\n\n";
    TReportTable segmentTable;
    segmentTable.AddRow({
        "Huffman code sets",
        FormatCount(stats.CodeSetCount) + " (scope " + GetScopeName(scope)
            + ")"});
    segmentTable.AddRow({"segments", FormatCount(segmentSizes.size())});
    if (!segmentSizes.empty()) {
        segmentTable.AddRow({
            "records per segment, mean",
            FormatCount(n / segmentSizes.size())});
        segmentTable.AddRow({
            "records per segment, median",
            FormatCount(segmentSizes[segmentSizes.size() / 2])});
        segmentTable.AddRow({
            "records per segment, min",
            FormatCount(segmentSizes.front())});
        segmentTable.AddRow({
            "records per segment, max",
            FormatCount(segmentSizes.back())});
    }
    segmentTable.Print(Cout);

    //
    // Output, one row per part of the file. The header, the size fields
    // and the padding of the bit arrays go to the last row. Symbols and
    // table sizes are summed over segments, max code is the largest one.
    //

    Cout << "\nOutput: " << outputPath << "\n\n";
    TReportTable output;
    output.AddRow(
        {"part", "symbols", "max code", "size", "bits/rec", "share"});
    output.AddSeparator();

    double accountedBits = 0;
    auto addPart = [&](TString name, TString symbols, TString maxCode,
                       double bits)
    {
        accountedBits += bits;
        output.AddRow({
            std::move(name),
            std::move(symbols),
            std::move(maxCode),
            FormatMiB(bits / 8),
            FormatBitsPerRecord(bits, n),
            Sprintf("%.1f%%", fileBits > 0 ? 100 * bits / fileBits : 0)});
    };
    auto addCoded = [&](EField field)
    {
        const auto i = static_cast<ui32>(field);
        addPart(
            TString(GetFieldName(field)),
            FormatCount(stats.SymbolCount[i]),
            ToString(stats.MaxCodeLength[i]),
            stats.CodedBits[i]);
    };
    auto addRaw = [&](TString name, ui32 bits)
    {
        addPart(std::move(name), "-", "raw", static_cast<double>(n) * bits);
    };

    addCoded(EField::TabletId);
    addCoded(EField::Channel);
    addCoded(EField::Generation);
    addCoded(EField::Step);
    addCoded(EField::Cookie);
    addRaw("CrcMode", CrcModeBits);
    addCoded(EField::BlobSize);
    addRaw("PartId", PartIdBits);
    addCoded(EField::Ingress);

    double tableBits = 0;
    for (const ui64 bytes: stats.TableBytes) {
        tableBits += 8.0 * bytes;
    }
    addPart("Huffman tables", "", "", tableBits);
    addPart("offset anchors", "", "", 8.0 * stats.AnchorBytes);
    addPart(
        "offset deltas",
        "",
        Sprintf("max %u bits", stats.MaxDeltaWidth),
        8.0 * stats.DeltaBytes);
    addPart(
        "segment directory",
        FormatCount(stats.SegmentCount),
        "",
        8.0 * stats.DirectoryBytes);
    addPart("headers, padding", "", "", fileBits - accountedBits);

    output.AddSeparator();
    output.AddRow({
        "total",
        "",
        "",
        FormatMiB(stats.FileBytes),
        FormatBitsPerRecord(fileBits, n),
        "100.0%"});
    output.Print(Cout);

    //
    // Ratios: input size / output size, more than 1 means the output is
    // smaller.
    //

    Cout << "\nCompression ratio (input size / output size)\n\n";
    TReportTable ratio;
    ratio.AddRow({
        "vs records taken, raw",
        FormatRatio(rawBytes, stats.FileBytes)});
    ratio.AddRow({
        "vs unpacked pages",
        FormatRatio(snapshotStats.UncompressedPageBytes, stats.FileBytes)});
    ratio.AddRow({
        "vs snapshot file (bzip2)",
        snapshotBytes < 0
            ? TString("-")
            : FormatRatio(snapshotBytes, stats.FileBytes)});
    ratio.Print(Cout);
    Cout.Flush();
}

////////////////////////////////////////////////////////////////////////////////

int Compress(int argc, const char** argv)
{
    TSnapshotOptions snapshotOptions;
    TString outputPath;
    TString scopeName = "global";

    NLastGetopt::TOpts opts;
    opts.AddHelpOption();
    opts.SetFreeArgsNum(0);
    snapshotOptions.Register(opts);
    opts.AddLongOption("output", "compressed index file")
        .RequiredArgument("PATH")
        .Required()
        .StoreResult(&outputPath);
    opts.AddLongOption(
            "scope",
            "segments sharing one set of Huffman codes: "
            "global (all), vdisk, sst (none)")
        .RequiredArgument("STR")
        .StoreResult(&scopeName);
    NLastGetopt::TOptsParseResult res(&opts, argc, argv);

    auto scope = EScope::Global;
    if (!ParseScope(scopeName, &scope)) {
        return PrintError(
            MakeError(EINVAL, "unknown scope: " + scopeName));
    }

    TSegments segments;
    TSnapshotStats snapshotStats;
    auto error = LoadSegments(snapshotOptions, &segments, &snapshotStats);
    if (HasError(error)) {
        return PrintError(error);
    }

    TCompressStats stats;
    error = WriteCompressedIndex(outputPath, scope, segments, &stats);
    if (HasError(error)) {
        return PrintError(error);
    }

    PrintCompressReport(
        snapshotOptions.SnapshotPath,
        snapshotStats,
        scope,
        segments,
        outputPath,
        stats);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

struct TFindStats
{
    ui64 Probes = 0;
    ui64 Matches = 0;
};

using TMatchHandler = std::function<
    void(size_t segment, ui64 index, const TLogoBlobRec& rec)>;

/**
 * Searches a key in the segments [segmentBegin, segmentEnd), a key can be
 * in any of them. Decodes the matching records and passes them to the
 * handler.
 */
TError FindInSegments(
    const TCompressedIndex& index,
    size_t segmentBegin,
    size_t segmentEnd,
    const TLogoBlobRec& key,
    const TMatchHandler& onMatch,
    TFindStats& stats)
{
    for (size_t i = segmentBegin; i < segmentEnd; ++i) {
        const auto& segment = index.GetSegment(i);
        ui64 probes = 0;
        auto [range, error] = segment.FindKey(key, &probes);
        if (HasError(error)) {
            return error;
        }
        stats.Probes += probes;

        for (ui64 j = range.first; j < range.second; ++j) {
            TLogoBlobRec rec;
            if (!segment.ReadRecord(j, &rec)) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "corrupted record " << j);
            }
            onMatch(i, j, rec);
            ++stats.Matches;
        }
    }
    return {};
}

////////////////////////////////////////////////////////////////////////////////

int Find(int argc, const char** argv)
{
    TString indexPath;
    ui64 tabletId = 0;
    ui32 generation = 0;
    ui32 step = 0;
    ui32 channel = 0;
    ui32 cookie = 0;
    ui32 blobSize = 0;
    ui32 crcMode = 0;
    ui32 partId = 0;

    NLastGetopt::TOpts opts;
    opts.AddHelpOption();
    opts.SetFreeArgsNum(0);
    opts.AddLongOption("index", "compressed index file")
        .RequiredArgument("PATH")
        .Required()
        .StoreResult(&indexPath);
    opts.AddLongOption("tablet-id")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&tabletId);
    opts.AddLongOption("generation")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&generation);
    opts.AddLongOption("step")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&step);
    opts.AddLongOption("channel")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&channel);
    opts.AddLongOption("cookie")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&cookie);
    opts.AddLongOption("blob-size")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&blobSize);
    opts.AddLongOption("part-id")
        .RequiredArgument("NUM")
        .Required()
        .StoreResult(&partId);
    opts.AddLongOption("crc-mode")
        .RequiredArgument("NUM")
        .DefaultValue(0)
        .StoreResult(&crcMode);
    NLastGetopt::TOptsParseResult res(&opts, argc, argv);

    auto [index, openError] = TCompressedIndex::Open(indexPath);
    if (HasError(openError)) {
        return PrintError(openError);
    }

    const auto key = TLogoBlobRec::Make(
        tabletId,
        generation,
        step,
        channel,
        cookie,
        blobSize,
        crcMode,
        partId,
        0 /* ingress */);

    ui64 records = 0;
    for (size_t i = 0; i < index->GetSegmentCount(); ++i) {
        records += index->GetSegment(i).GetRecordCount();
    }

    Cout << "segment\tindex\t" << TsvHeader() << '\n';
    TFindStats stats;
    auto error = FindInSegments(
        *index,
        0 /* segmentBegin */,
        index->GetSegmentCount(),
        key,
        [&](size_t segment, ui64 i, const TLogoBlobRec& rec)
        {
            Cout << FormatSegment(index->GetSegmentId(segment)) << '\t' << i
                 << '\t' << rec.ToTsv() << '\n';
        },
        stats);
    Cout.Flush();
    if (HasError(error)) {
        return PrintError(error);
    }

    Cerr << "segments\t" << index->GetSegmentCount()
         << "\nrecords\t" << records
         << "\nprobes\t" << stats.Probes
         << "\nmatches\t" << stats.Matches << Endl;

    if (!stats.Matches) {
        Cerr << "not found" << Endl;
        return ExitNotFound;
    }

    return ExitFound;
}

////////////////////////////////////////////////////////////////////////////////

TString FormatMicroseconds(double ns)
{
    return Sprintf("%.2f us", ns / 1000);
}

//
// Plain index record as kept in VDisk memory: TKeyLogoBlob (24 bytes) and
// TMemRecLogoBlob packed to 4 bytes (20 bytes). The snapshot has no
// location fields, they are zero.
//

#pragma pack(push, 4)
struct TPlainRec
{
    ui64 Raw[3] = {};
    ui64 Ingress = 0;
    ui32 TypeAndId = 0;
    ui32 Offset = 0;
    ui32 Size = 0;
};
#pragma pack(pop)

static_assert(sizeof(TPlainRec) == 44);

using TPlainSegments = TVector<TArrayRef<const TPlainRec>>;

/**
 * Decodes one segment of the index into a plain sorted array.
 */
TError DecodePlainSegment(
    const TIndexSegment& segment,
    TVector<TPlainRec>* records)
{
    records->assign(segment.GetRecordCount(), TPlainRec());
    ui64 offset = 0;
    for (auto& plainRec: *records) {
        TLogoBlobRec rec;
        if (!segment.ReadRecordAt(offset, &rec, &offset)) {
            return MakeError(EBADMSG, "corrupted record");
        }
        Copy(std::begin(rec.Raw), std::end(rec.Raw), plainRec.Raw);
        plainRec.Ingress = rec.Ingress;
    }
    return {};
}

/**
 * Plain sorted arrays of all segments of an index, kept on the heap or in
 * a file mapped into memory.
 */
class TPlainIndex
{
private:
    TVector<TVector<TPlainRec>> Heap;
    std::unique_ptr<TFileMap> File;
    TPlainSegments Segments;

public:
    const TPlainSegments& GetSegments() const
    {
        return Segments;
    }

    TError LoadHeap(const TCompressedIndex& index)
    {
        Heap.resize(index.GetSegmentCount());
        for (size_t i = 0; i < index.GetSegmentCount(); ++i) {
            auto error = DecodePlainSegment(index.GetSegment(i), &Heap[i]);
            if (HasError(error)) {
                return error;
            }
            Segments.emplace_back(Heap[i]);
        }
        return {};
    }

    TError LoadMapped(
        const TCompressedIndex& index,
        const TString& path)
    {
        //
        // Segments are written one after another without a header, then
        // the file is mapped. TFileOutput and TFileMap report failures by
        // exceptions, they are converted to an error here.
        //

        TVector<ui64> counts;
        try {
            TFileOutput out(path);
            TVector<TPlainRec> records;
            for (size_t i = 0; i < index.GetSegmentCount(); ++i) {
                auto error = DecodePlainSegment(index.GetSegment(i), &records);
                if (HasError(error)) {
                    return error;
                }
                out.Write(records.data(), records.size() * sizeof(TPlainRec));
                counts.push_back(records.size());
            }
            out.Finish();

            File = std::make_unique<TFileMap>(path);
            File->Map(0 /* offset */, File->Length());
        } catch (...) {
            return MakeErrorFromCurrentException();
        }

        const auto* records = static_cast<const TPlainRec*>(File->Ptr());
        for (const ui64 count: counts) {
            Segments.emplace_back(records, count);
            records += count;
        }
        return {};
    }
};

/**
 * Searches a key in the plain segments [segmentBegin, segmentEnd) with
 * std::equal_range and copies the matching records, as FindInSegments
 * decodes them.
 */
void FindInPlainSegments(
    const TPlainSegments& plain,
    size_t segmentBegin,
    size_t segmentEnd,
    const TLogoBlobRec& key,
    const TMatchHandler& onMatch,
    TFindStats& stats)
{
    auto less = [&](const auto& l, const auto& r)
    {
        ++stats.Probes;
        return std::tie(l.Raw[0], l.Raw[1], l.Raw[2])
             < std::tie(r.Raw[0], r.Raw[1], r.Raw[2]);
    };

    for (size_t i = segmentBegin; i < segmentEnd; ++i) {
        const auto records = plain[i];
        TPlainRec plainKey;
        Copy(std::begin(key.Raw), std::end(key.Raw), plainKey.Raw);
        const auto [begin, end] =
            std::equal_range(records.begin(), records.end(), plainKey, less);

        for (auto it = begin; it != end; ++it) {
            TLogoBlobRec rec;
            Copy(std::begin(it->Raw), std::end(it->Raw), rec.Raw);
            rec.Ingress = it->Ingress;
            onMatch(i, it - records.begin(), rec);
            ++stats.Matches;
        }
    }
}

struct TBenchResult
{
    TFindStats Find;
    ui64 Checksum = 0;
    ui64 TotalNs = 0;
    TVector<ui64> Latencies;   // sorted, one per key

    void Add(const TBenchResult& other)
    {
        Find.Probes += other.Find.Probes;
        Find.Matches += other.Find.Matches;
        Checksum += other.Checksum;
        TotalNs += other.TotalNs;
        Latencies.insert(
            Latencies.end(),
            other.Latencies.begin(),
            other.Latencies.end());
        Sort(Latencies);
    }

    double Percentile(double p) const
    {
        const size_t i = Min<size_t>(
            Latencies.size() * p,
            Latencies.size() - 1);
        return static_cast<double>(Latencies[i]);
    }
};

/**
 * Runs a search for every key and times every search separately. The
 * checksum of the matched ingress words shows that both searches return
 * the same records.
 */
template <typename TSearch>
TError RunBench(
    const TVector<TLogoBlobRec>& keys,
    const TSearch& search,
    TBenchResult* result)
{
    using TClock = std::chrono::steady_clock;

    auto onMatch = [result](size_t, ui64, const TLogoBlobRec& rec)
    {
        result->Checksum += rec.Ingress;
    };

    result->Latencies.reserve(keys.size());
    const auto start = TClock::now();
    for (const auto& key: keys) {
        const auto t0 = TClock::now();
        auto error = search(key, onMatch, result->Find);
        const auto t1 = TClock::now();
        if (HasError(error)) {
            return error;
        }
        result->Latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
    }
    result->TotalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        TClock::now() - start).count();

    Sort(result->Latencies);
    return {};
}

/**
 * Segments of one VDisk: they are adjacent in the directory, which is
 * sorted by segment id.
 */
struct TVDiskSegments
{
    ui32 GroupId = 0;
    ui32 VDiskIndex = 0;
    size_t Begin = 0;
    size_t End = 0;
    ui64 Records = 0;
};

TVector<TVDiskSegments> GetVDiskSegments(const TCompressedIndex& index)
{
    TVector<TVDiskSegments> vdisks;
    for (size_t i = 0; i < index.GetSegmentCount(); ++i) {
        const auto& id = index.GetSegmentId(i);
        if (vdisks.empty() || vdisks.back().GroupId != id.GroupId
            || vdisks.back().VDiskIndex != id.VDiskIndex)
        {
            vdisks.push_back({
                .GroupId = id.GroupId,
                .VDiskIndex = id.VDiskIndex,
                .Begin = i,
                .End = i,
            });
        }
        vdisks.back().End = i + 1;
        vdisks.back().Records += index.GetSegment(i).GetRecordCount();
    }
    return vdisks;
}

/**
 * Picks keys of records taken uniformly over the records of one VDisk.
 */
TError PickKeys(
    const TCompressedIndex& index,
    const TVDiskSegments& vdisk,
    ui64 count,
    TFastRng64& rng,
    TVector<TLogoBlobRec>* keys)
{
    TVector<ui64> firstRecord;
    ui64 records = 0;
    for (size_t i = vdisk.Begin; i < vdisk.End; ++i) {
        firstRecord.push_back(records);
        records += index.GetSegment(i).GetRecordCount();
    }

    keys->reserve(count);
    for (ui64 k = 0; k < count; ++k) {
        const ui64 r = rng.Uniform(records);
        const size_t i =
            UpperBound(firstRecord.begin(), firstRecord.end(), r)
            - firstRecord.begin() - 1;
        TLogoBlobRec rec;
        if (!index.GetSegment(vdisk.Begin + i).ReadRecord(
                r - firstRecord[i],
                &rec))
        {
            return MakeError(EBADMSG, "corrupted record");
        }
        keys->push_back(rec);
    }
    return {};
}

int Bench(int argc, const char** argv)
{
    TString indexPath;
    ui64 count = 100000;
    ui64 seed = 1;
    TString plainMemory = "mmap";
    TString plainPath;

    NLastGetopt::TOpts opts;
    opts.AddHelpOption();
    opts.SetFreeArgsNum(0);
    opts.AddLongOption("index", "compressed index file")
        .RequiredArgument("PATH")
        .Required()
        .StoreResult(&indexPath);
    opts.AddLongOption("count", "number of keys to search per VDisk")
        .RequiredArgument("NUM")
        .DefaultValue(count)
        .StoreResult(&count);
    opts.AddLongOption("seed", "random seed for key selection")
        .RequiredArgument("NUM")
        .DefaultValue(seed)
        .StoreResult(&seed);
    opts.AddLongOption(
            "plain-memory",
            "where the plain arrays are kept: mmap (a file mapped into "
            "memory, like the index) or heap")
        .RequiredArgument("STR")
        .DefaultValue(plainMemory)
        .StoreResult(&plainMemory);
    opts.AddLongOption(
            "plain-file",
            "file for the plain arrays in mmap mode, deleted after the run; "
            "default: index path + .plain")
        .RequiredArgument("PATH")
        .StoreResult(&plainPath);
    NLastGetopt::TOptsParseResult res(&opts, argc, argv);

    if (plainMemory != "mmap" && plainMemory != "heap") {
        return PrintError(MakeError(
            EINVAL,
            "unknown plain memory: " + plainMemory));
    }
    const bool plainMapped = plainMemory == "mmap";
    if (!plainPath) {
        plainPath = indexPath + ".plain";
    }
    if (!count) {
        return PrintError(MakeError(EINVAL, "count must be positive"));
    }

    auto [index, openError] = TCompressedIndex::Open(indexPath);
    if (HasError(openError)) {
        return PrintError(openError);
    }

    const auto vdisks = GetVDiskSegments(*index);
    ui64 records = 0;
    for (const auto& vdisk: vdisks) {
        records += vdisk.Records;
    }
    if (!records) {
        return PrintError(MakeError(EINVAL, "index has no records"));
    }

    //
    // Decode the plain arrays. Not timed.
    //

    TPlainIndex plain;
    Y_DEFER {
        if (plainMapped && NFs::Exists(plainPath) && !NFs::Remove(plainPath)) {
            Cerr << "warning: cannot delete " << plainPath << Endl;
        }
    };
    auto error = plainMapped
        ? plain.LoadMapped(*index, plainPath)
        : plain.LoadHeap(*index);
    if (HasError(error)) {
        return PrintError(error);
    }

    //
    // For every VDisk: pick keys from its records, then search them in its
    // segments only, first in the compressed index, then in the plain
    // arrays. Key selection is not timed.
    //

    TFastRng64 rng(seed);
    TVector<TBenchResult> compressed(vdisks.size());
    TVector<TBenchResult> plainResults(vdisks.size());
    for (size_t v = 0; v < vdisks.size(); ++v) {
        const auto& vdisk = vdisks[v];
        if (!vdisk.Records) {
            continue;
        }

        TVector<TLogoBlobRec> keys;
        error = PickKeys(*index, vdisk, count, rng, &keys);
        if (HasError(error)) {
            return PrintError(error);
        }

        error = RunBench(
            keys,
            [&](const TLogoBlobRec& key, const TMatchHandler& onMatch,
                TFindStats& stats)
            {
                return FindInSegments(
                    *index,
                    vdisk.Begin,
                    vdisk.End,
                    key,
                    onMatch,
                    stats);
            },
            &compressed[v]);
        if (HasError(error)) {
            return PrintError(error);
        }

        error = RunBench(
            keys,
            [&](const TLogoBlobRec& key, const TMatchHandler& onMatch,
                TFindStats& stats)
            {
                FindInPlainSegments(
                    plain.GetSegments(),
                    vdisk.Begin,
                    vdisk.End,
                    key,
                    onMatch,
                    stats);
                return TError();
            },
            &plainResults[v]);
        if (HasError(error)) {
            return PrintError(error);
        }

        const auto& c = compressed[v];
        const auto& p = plainResults[v];
        if (c.Find.Matches < count || c.Find.Matches != p.Find.Matches
            || c.Checksum != p.Checksum)
        {
            return PrintError(MakeError(
                EBADMSG,
                TStringBuilder() << "searches disagree on vdisk "
                                 << vdisk.GroupId << ":" << vdisk.VDiskIndex
                                 << ": compressed " << c.Find.Matches
                                 << " matches, plain " << p.Find.Matches
                                 << " matches"));
        }
    }

    //
    // Report.
    //

    const i64 indexBytes = GetFileLength(indexPath);
    const double plainBytes =
        static_cast<double>(records) * sizeof(TPlainRec);

    Cout << "Index: " << indexPath << "\n\n";
    TReportTable setup;
    setup.AddRow({"scope", TString(GetScopeName(index->GetScope()))});
    setup.AddRow({"code sets", FormatCount(index->GetCodeSetCount())});
    setup.AddRow({"segments", FormatCount(index->GetSegmentCount())});
    setup.AddRow({"VDisks", FormatCount(vdisks.size())});
    setup.AddRow({"records", FormatCount(records)});
    setup.AddRow({"keys searched per VDisk", FormatCount(count)});
    setup.AddRow({"seed", ToString(seed)});
    setup.AddRow({
        "compressed size",
        indexBytes < 0 ? TString("-") : FormatMiB(indexBytes)});
    setup.AddRow({
        Sprintf("plain size, %zu B records", sizeof(TPlainRec)),
        FormatMiB(plainBytes)});
    setup.AddRow({
        "plain arrays",
        plainMapped ? "mmap of " + plainPath : TString("heap")});
    setup.Print(Cout);

    Cout << "\nFind time per VDisk: each key is searched in the segments of "
         << "its VDisk only\n\n";
    TReportTable table;
    table.AddRow({
        "vdisk",
        "segments",
        "records",
        "mean, compr.",
        "mean, plain",
        "compr. / plain",
        "p99, compr.",
        "p99, plain",
        "compared, compr.",
        "compared, plain"});
    table.AddSeparator();

    auto addRow = [&](TString name, ui64 segments, ui64 vdiskRecords,
                      const TBenchResult& c, const TBenchResult& p)
    {
        const ui64 keys = c.Latencies.size();
        const double cMean = PerRecord(c.TotalNs, keys);
        const double pMean = PerRecord(p.TotalNs, keys);
        table.AddRow({
            std::move(name),
            FormatCount(segments),
            FormatCount(vdiskRecords),
            FormatMicroseconds(cMean),
            FormatMicroseconds(pMean),
            FormatRatio(cMean, pMean),
            FormatMicroseconds(c.Percentile(0.99)),
            FormatMicroseconds(p.Percentile(0.99)),
            Sprintf("%.1f", PerRecord(c.Find.Probes, keys)),
            Sprintf("%.1f", PerRecord(p.Find.Probes, keys))});
    };

    TBenchResult compressedAll;
    TBenchResult plainAll;
    for (size_t v = 0; v < vdisks.size(); ++v) {
        const auto& vdisk = vdisks[v];
        if (!vdisk.Records) {
            continue;
        }
        addRow(
            TStringBuilder() << vdisk.GroupId << ":" << vdisk.VDiskIndex,
            vdisk.End - vdisk.Begin,
            vdisk.Records,
            compressed[v],
            plainResults[v]);
        compressedAll.Add(compressed[v]);
        plainAll.Add(plainResults[v]);
    }
    table.AddSeparator();
    addRow(
        "all keys",
        index->GetSegmentCount(),
        records,
        compressedAll,
        plainAll);
    table.Print(Cout);
    Cout.Flush();

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

struct TVerifyStats
{
    ui64 Records = 0;
    ui64 Keys = 0;
    ui64 TotalProbes = 0;
    ui64 MaxProbes = 0;
};

/**
 * Checks one segment against its records from the snapshot.
 */
TError VerifySegment(
    const TIndexSegment& segment,
    const TVector<TLogoBlobRec>& records,
    const TString& name,
    TVerifyStats& stats)
{
    auto fail = [&](TStringBuf what)
    {
        return MakeError(
            EBADMSG,
            TStringBuilder() << "segment " << name << ": " << what);
    };

    if (segment.GetRecordCount() != records.size()) {
        return fail(
            TStringBuilder() << "record count mismatch: index "
                             << segment.GetRecordCount() << ", snapshot "
                             << records.size());
    }

    //
    // Decode the stream sequentially and check every record and every
    // offset computed from the anchors and the deltas.
    //

    ui64 offset = 0;
    for (ui64 i = 0; i < records.size(); ++i) {
        if (segment.GetRecordOffset(i) != offset) {
            return fail(TStringBuilder() << "offset mismatch at record " << i);
        }
        TLogoBlobRec rec;
        if (!segment.ReadRecordAt(offset, &rec, &offset)) {
            return fail(TStringBuilder() << "corrupted record " << i);
        }
        if (!(rec == records[i])) {
            return fail(
                TStringBuilder() << "record mismatch at " << i << ": "
                                 << rec.ToTsv() << " vs "
                                 << records[i].ToTsv());
        }
    }
    if (segment.GetRecordOffset(records.size()) != offset) {
        return fail("stream end mismatch");
    }

    //
    // Search every distinct key and compare the range with the one in the
    // sorted snapshot records.
    //

    for (ui64 begin = 0; begin < records.size();) {
        ui64 end = begin + 1;
        while (end < records.size()
               && records[end].CompareKey(records[begin]) == 0)
        {
            ++end;
        }

        ui64 probes = 0;
        auto [range, findError] = segment.FindKey(records[begin], &probes);
        if (HasError(findError)) {
            return fail(FormatError(findError));
        }
        if (range != std::make_pair(begin, end)) {
            return fail(
                TStringBuilder() << "search mismatch for record " << begin
                                 << ": got [" << range.first << ", "
                                 << range.second << "), expected ["
                                 << begin << ", " << end << ")");
        }

        ++stats.Keys;
        stats.TotalProbes += probes;
        stats.MaxProbes = Max(stats.MaxProbes, probes);
        begin = end;
    }

    stats.Records += records.size();
    return {};
}

int Verify(int argc, const char** argv)
{
    TSnapshotOptions snapshotOptions;
    TString indexPath;

    NLastGetopt::TOpts opts;
    opts.AddHelpOption();
    opts.SetFreeArgsNum(0);
    snapshotOptions.Register(opts);
    opts.AddLongOption("index", "compressed index file")
        .RequiredArgument("PATH")
        .Required()
        .StoreResult(&indexPath);
    NLastGetopt::TOptsParseResult res(&opts, argc, argv);

    auto [index, openError] = TCompressedIndex::Open(indexPath);
    if (HasError(openError)) {
        return PrintError(openError);
    }

    TSegments segments;
    TSnapshotStats snapshotStats;
    auto error = LoadSegments(snapshotOptions, &segments, &snapshotStats);
    if (HasError(error)) {
        return PrintError(error);
    }

    if (index->GetSegmentCount() != segments.size()) {
        return PrintError(MakeError(
            EBADMSG,
            TStringBuilder() << "segment count mismatch: index "
                             << index->GetSegmentCount() << ", snapshot "
                             << segments.size()));
    }

    TVerifyStats stats;
    size_t i = 0;
    for (const auto& [id, records]: segments) {
        const auto name = FormatSegment(id);
        if (index->GetSegmentId(i) != id) {
            return PrintError(MakeError(
                EBADMSG,
                TStringBuilder() << "segment " << i << " is "
                                 << FormatSegment(index->GetSegmentId(i))
                                 << ", expected " << name));
        }
        error = VerifySegment(index->GetSegment(i), records, name, stats);
        if (HasError(error)) {
            return PrintError(error);
        }
        ++i;
    }

    Cout << "scope\t" << GetScopeName(index->GetScope())
         << "\ncode_sets\t" << index->GetCodeSetCount()
         << "\nsegments\t" << segments.size()
         << "\nrecords\t" << stats.Records
         << "\ndistinct_keys\t" << stats.Keys
         << "\navg_probes\t"
         << Sprintf("%.2f", PerRecord(stats.TotalProbes, stats.Keys))
         << "\nmax_probes\t" << stats.MaxProbes
         << "\nOK" << Endl;

    return 0;
}

}   // namespace

}   // namespace NCloud::NStorage::NLsmCompress

////////////////////////////////////////////////////////////////////////////////

int main(int argc, const char** argv)
{
    using namespace NCloud::NStorage::NLsmCompress;

    TModChooser modes;
    modes.AddMode(
        "compress",
        Compress,
        "read a snapshot, write sorted Huffman-coded records and offsets");
    modes.AddMode(
        "find",
        Find,
        "binary search for a TKeyLogoBlob in a compressed index file");
    modes.AddMode(
        "bench",
        Bench,
        "time find for random keys taken from a compressed index file");
    modes.AddMode(
        "verify",
        Verify,
        "check a compressed index file against its snapshot");

    return modes.Run(argc, argv);
}
