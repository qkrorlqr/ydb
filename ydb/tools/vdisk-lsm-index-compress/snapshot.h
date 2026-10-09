#pragma once

#include "error.h"
#include "record.h"

#include <util/generic/string.h>

#include <functional>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

enum class EDatabase: ui32
{
    LogoBlobs = 0,
    Blocks = 1,
    Barriers = 2,
};

enum class ETableType: ui32
{
    FCur = 0,
    FDreg = 1,
    FOld = 2,
    Level = 3,
};

/**
 * Parses a table type name: fcur, fdreg, fold or level.
 *
 * @param name - Table type name.
 * @param tableType - (out) Parsed table type.
 * @return - False if the name is unknown.
 */
bool ParseTableType(TStringBuf name, ETableType* tableType);

/**
 * @param tableType - Table type.
 * @return - Table type name as in YDB: FCur, FDreg, FOld or Level.
 */
TStringBuf GetTableTypeName(ETableType tableType);

////////////////////////////////////////////////////////////////////////////////

struct TBlockInfo
{
    ui32 GroupId = 0;
    ui32 VDiskIndex = 0;
    ui64 SequenceId = 0;
    EDatabase Database = EDatabase::LogoBlobs;
    ETableType TableType = ETableType::FCur;
    ui32 Level = 0;
    ui64 SstId = 0;
    ui32 NumRecords = 0;
};

struct TSnapshotStats
{
    ui64 Streams = 0;
    ui64 Pages = 0;
    ui64 Blocks = 0;
    ui64 CompressedPageBytes = 0;
    ui64 UncompressedPageBytes = 0;
    ui64 LogoBlobRecords = 0;
    ui64 BlockRecords = 0;
    ui64 BarrierRecords = 0;
};

using TLogoBlobHandler =
    std::function<void(const TBlockInfo& block, const TLogoBlobRec& rec)>;

/**
 * Reads a ydb-dstool group take-snapshot file.
 * The format: a sequence of <III> outer headers, each followed by a bzip2 page
 * with a <QQQQ> page header and <IIQ> block headers.
 *
 * @param path - Snapshot file path.
 * @param validateStreams - Check sequence ids, actor ids and remaining-block
 *  counters of every VDisk stream.
 * @param onLogoBlob - Called for every LogoBlob record in file order.
 * @param stats - (out) Counters over the whole file.
 * @return - Error if the file is unreadable or malformed.
 */
TError ReadSnapshot(
    const TString& path,
    bool validateStreams,
    const TLogoBlobHandler& onLogoBlob,
    TSnapshotStats* stats);

}   // namespace NCloud::NStorage::NLsmCompress
