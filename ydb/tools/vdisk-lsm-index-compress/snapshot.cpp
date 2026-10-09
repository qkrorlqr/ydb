#include "snapshot.h"

#include <library/cpp/streams/bzip2/bzip2.h>

#include <util/generic/hash.h>
#include <util/stream/file.h>
#include <util/stream/mem.h>
#include <util/string/builder.h>
#include <util/system/unaligned_mem.h>

namespace NCloud::NStorage::NLsmCompress {

namespace {

////////////////////////////////////////////////////////////////////////////////

constexpr size_t OuterHeaderSize = 12;
constexpr size_t PageHeaderSize = 32;
constexpr size_t BlockHeaderSize = 16;

constexpr size_t LogoBlobRecordSize = 32;
constexpr size_t BlockRecordSize = 12;
constexpr size_t BarrierRecordSize = 32;

////////////////////////////////////////////////////////////////////////////////

struct TStreamState
{
    ui64 ActorX1 = 0;
    ui64 ActorX2 = 0;
    ui64 NextSequenceId = 0;
    ui64 BlocksRemaining = 0;
};

using TStreamId = std::pair<ui32, ui32>;

////////////////////////////////////////////////////////////////////////////////

class TSnapshotReader
{
private:
    const TString Path;
    const bool ValidateStreams;
    const TLogoBlobHandler& OnLogoBlob;
    TSnapshotStats& Stats;

    THashMap<TStreamId, TStreamState> States;

public:
    TSnapshotReader(
            TString path,
            bool validateStreams,
            const TLogoBlobHandler& onLogoBlob,
            TSnapshotStats& stats)
        : Path(std::move(path))
        , ValidateStreams(validateStreams)
        , OnLogoBlob(onLogoBlob)
        , Stats(stats)
    {}

    TError Read()
    {
        TFileInput input(Path);
        ui64 fileOffset = 0;
        TString compressed;

        while (true) {
            char header[OuterHeaderSize];
            const size_t headerBytes = input.Load(header, OuterHeaderSize);
            if (headerBytes == 0) {
                break;
            }
            if (headerBytes != OuterHeaderSize) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "truncated outer header at file offset "
                                     << fileOffset);
            }

            const auto groupId = ReadUnaligned<ui32>(header);
            const auto vdiskIndex = ReadUnaligned<ui32>(header + 4);
            const auto compressedSize = ReadUnaligned<ui32>(header + 8);

            compressed.resize(compressedSize);
            if (input.Load(compressed.begin(), compressedSize)
                != compressedSize)
            {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "truncated compressed page at file "
                                     << "offset "
                                     << fileOffset + OuterHeaderSize);
            }
            if (!compressed.StartsWith("BZh")) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "frame at file offset " << fileOffset
                                     << " does not contain a bzip2 stream");
            }

            TMemoryInput compressedInput(compressed.data(), compressed.size());
            TBZipDecompress decompress(&compressedInput);
            const TString page = decompress.ReadAll();

            auto error = ParsePage(fileOffset, groupId, vdiskIndex, page);
            if (HasError(error)) {
                return error;
            }

            ++Stats.Pages;
            Stats.CompressedPageBytes += compressedSize;
            Stats.UncompressedPageBytes += page.size();
            fileOffset += OuterHeaderSize + compressedSize;
        }

        Stats.Streams = States.size();

        if (ValidateStreams) {
            for (const auto& [streamId, state]: States) {
                if (state.BlocksRemaining) {
                    return MakeError(
                        EBADMSG,
                        TStringBuilder()
                            << "incomplete VDisk stream (" << streamId.first
                            << ", " << streamId.second << ") at EOF: "
                            << state.BlocksRemaining << " blocks");
                }
            }
        }

        return {};
    }

private:
    TError ParsePage(
        ui64 fileOffset,
        ui32 groupId,
        ui32 vdiskIndex,
        TStringBuf page)
    {
        if (page.size() < PageHeaderSize) {
            return MakeError(
                EBADMSG,
                TStringBuilder() << "short decompressed page at file offset "
                                 << fileOffset);
        }

        const auto actorX1 = ReadUnaligned<ui64>(page.data());
        const auto actorX2 = ReadUnaligned<ui64>(page.data() + 8);
        const auto sequenceId = ReadUnaligned<ui64>(page.data() + 16);
        const auto blocksRemaining = ReadUnaligned<ui64>(page.data() + 24);

        //
        // Parse block headers and records. Payload is validated before any
        // record is passed to the handler.
        //

        size_t position = PageHeaderSize;
        ui64 blockCount = 0;
        while (position < page.size()) {
            if (page.size() - position < BlockHeaderSize) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "truncated block header at file offset "
                                     << fileOffset << ", decompressed offset "
                                     << position);
            }

            const char* blockHeader = page.data() + position;
            const auto numRecords = ReadUnaligned<ui32>(blockHeader);
            const auto flags = ReadUnaligned<ui32>(blockHeader + 4);
            const auto sstId = ReadUnaligned<ui64>(blockHeader + 8);
            position += BlockHeaderSize;

            const ui32 database = flags & 0x3U;
            const ui32 tableType = (flags >> 2) & 0x3U;
            const ui32 level = (flags >> 4) & 0x3FU;
            const ui32 reserved = flags >> 10;

            if (database > static_cast<ui32>(EDatabase::Barriers)) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "unknown database " << database
                                     << " at file offset " << fileOffset);
            }
            if (reserved) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "non-zero reserved block-header bits "
                                     << "at file offset " << fileOffset);
            }

            const auto db = static_cast<EDatabase>(database);
            const size_t recordSize = GetRecordSize(db);
            const ui64 payloadSize = static_cast<ui64>(numRecords) * recordSize;
            if (payloadSize > page.size() - position) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "truncated block at file offset "
                                     << fileOffset << ": expected "
                                     << payloadSize << " record bytes, got "
                                     << page.size() - position);
            }

            TBlockInfo block{
                .GroupId = groupId,
                .VDiskIndex = vdiskIndex,
                .SequenceId = sequenceId,
                .Database = db,
                .TableType = static_cast<ETableType>(tableType),
                .Level = level,
                .SstId = sstId,
                .NumRecords = numRecords,
            };
            HandleBlock(block, page.data() + position);

            position += payloadSize;
            ++blockCount;
        }

        if (blocksRemaining < blockCount) {
            return MakeError(
                EBADMSG,
                TStringBuilder() << "page at file offset " << fileOffset
                                 << " contains " << blockCount
                                 << " blocks, but its remaining-block counter "
                                 << "is " << blocksRemaining);
        }

        Stats.Blocks += blockCount;

        auto& state = States[TStreamId(groupId, vdiskIndex)];
        const bool newStream = state.NextSequenceId == 0;
        if (ValidateStreams) {
            auto error = ValidatePage(
                state,
                newStream,
                groupId,
                vdiskIndex,
                actorX1,
                actorX2,
                sequenceId,
                blocksRemaining);
            if (HasError(error)) {
                return error;
            }
        }

        state.ActorX1 = actorX1;
        state.ActorX2 = actorX2;
        state.NextSequenceId = sequenceId + 1;
        state.BlocksRemaining = blocksRemaining - blockCount;

        return {};
    }

    static TError ValidatePage(
        const TStreamState& state,
        bool newStream,
        ui32 groupId,
        ui32 vdiskIndex,
        ui64 actorX1,
        ui64 actorX2,
        ui64 sequenceId,
        ui64 blocksRemaining)
    {
        auto streamName = [&]
        {
            return TStringBuilder()
                   << "(" << groupId << ", " << vdiskIndex << ")";
        };

        if (newStream) {
            if (sequenceId != 1) {
                return MakeError(
                    EBADMSG,
                    TStringBuilder() << "stream " << streamName()
                                     << " starts at sequence " << sequenceId
                                     << ", not 1");
            }
            return {};
        }

        if (actorX1 != state.ActorX1 || actorX2 != state.ActorX2) {
            return MakeError(
                EBADMSG,
                TStringBuilder() << "actor id changed in stream "
                                 << streamName());
        }
        if (sequenceId != state.NextSequenceId) {
            return MakeError(
                EBADMSG,
                TStringBuilder() << "sequence gap in stream " << streamName()
                                 << ": expected " << state.NextSequenceId
                                 << ", got " << sequenceId);
        }
        if (blocksRemaining != state.BlocksRemaining) {
            return MakeError(
                EBADMSG,
                TStringBuilder() << "remaining-block mismatch in stream "
                                 << streamName() << ": expected "
                                 << state.BlocksRemaining << ", got "
                                 << blocksRemaining);
        }
        return {};
    }

    void HandleBlock(const TBlockInfo& block, const char* data)
    {
        switch (block.Database) {
            case EDatabase::LogoBlobs:
                break;
            case EDatabase::Blocks:
                Stats.BlockRecords += block.NumRecords;
                return;
            case EDatabase::Barriers:
                Stats.BarrierRecords += block.NumRecords;
                return;
        }

        Stats.LogoBlobRecords += block.NumRecords;
        for (ui32 i = 0; i < block.NumRecords; ++i) {
            const char* p = data + i * LogoBlobRecordSize;
            TLogoBlobRec rec;
            rec.Raw[0] = ReadUnaligned<ui64>(p);
            rec.Raw[1] = ReadUnaligned<ui64>(p + 8);
            rec.Raw[2] = ReadUnaligned<ui64>(p + 16);
            rec.Ingress = ReadUnaligned<ui64>(p + 24);
            OnLogoBlob(block, rec);
        }
    }

    static size_t GetRecordSize(EDatabase database)
    {
        switch (database) {
            case EDatabase::LogoBlobs:
                return LogoBlobRecordSize;
            case EDatabase::Blocks:
                return BlockRecordSize;
            case EDatabase::Barriers:
                return BarrierRecordSize;
        }
        return 0;
    }
};

}   // namespace

////////////////////////////////////////////////////////////////////////////////

bool ParseTableType(TStringBuf name, ETableType* tableType)
{
    if (name == "fcur") {
        *tableType = ETableType::FCur;
    } else if (name == "fdreg") {
        *tableType = ETableType::FDreg;
    } else if (name == "fold") {
        *tableType = ETableType::FOld;
    } else if (name == "level") {
        *tableType = ETableType::Level;
    } else {
        return false;
    }
    return true;
}

TStringBuf GetTableTypeName(ETableType tableType)
{
    switch (tableType) {
        case ETableType::FCur:
            return "FCur";
        case ETableType::FDreg:
            return "FDreg";
        case ETableType::FOld:
            return "FOld";
        case ETableType::Level:
            return "Level";
    }
    return "Unknown";
}

TError ReadSnapshot(
    const TString& path,
    bool validateStreams,
    const TLogoBlobHandler& onLogoBlob,
    TSnapshotStats* stats)
{
    *stats = {};
    TSnapshotReader reader(path, validateStreams, onLogoBlob, *stats);

    //
    // TFileInput and TBZipDecompress report failures by exceptions. They
    // are converted to an error here.
    //

    try {
        return reader.Read();
    } catch (const TBZipDecompressError& e) {
        return MakeError(EBADMSG, e.what());
    } catch (...) {
        return MakeErrorFromCurrentException();
    }
}

}   // namespace NCloud::NStorage::NLsmCompress
