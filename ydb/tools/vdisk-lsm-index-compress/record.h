#pragma once

#include <util/generic/string.h>
#include <util/system/types.h>

#include <compare>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

constexpr ui32 CrcModeBits = 2;
constexpr ui32 PartIdBits = 4;

////////////////////////////////////////////////////////////////////////////////

/**
 * TKeyLogoBlob as three raw words plus the ingress word of TMemRecLogoBlob.
 * Ordering of the raw words matches the ordering of TLogoBlobID: TabletId,
 * Channel, Generation, Step, Cookie, CrcMode, BlobSize, PartId.
 */
struct TLogoBlobRec
{
    ui64 Raw[3] = {};
    ui64 Ingress = 0;

    /**
     * Builds a record from unpacked key fields.
     *
     * @param tabletId - TabletId.
     * @param generation - Generation.
     * @param step - Step.
     * @param channel - Channel.
     * @param cookie - Cookie.
     * @param blobSize - BlobSize.
     * @param crcMode - CrcMode.
     * @param partId - PartId.
     * @param ingress - Ingress.
     * @return - The record.
     */
    static TLogoBlobRec Make(
        ui64 tabletId,
        ui32 generation,
        ui32 step,
        ui32 channel,
        ui32 cookie,
        ui32 blobSize,
        ui32 crcMode,
        ui32 partId,
        ui64 ingress);

    ui64 TabletId() const
    {
        return Raw[0];
    }

    ui32 Generation() const
    {
        return (Raw[1] >> 24) & 0xFFFFFFFFULL;
    }

    ui32 Step() const
    {
        return ((Raw[1] & 0xFFFFFFULL) << 8) | ((Raw[2] >> 56) & 0xFFULL);
    }

    ui32 Channel() const
    {
        return (Raw[1] >> 56) & 0xFFULL;
    }

    ui32 BlobSize() const
    {
        return (Raw[2] >> 4) & 0x3FFFFFFULL;
    }

    ui32 Cookie() const
    {
        return (Raw[2] >> 32) & 0xFFFFFFULL;
    }

    ui32 PartId() const
    {
        return Raw[2] & 0xFULL;
    }

    ui32 CrcMode() const
    {
        return (Raw[2] >> 30) & 0x3ULL;
    }

    /**
     * Compares the key words only, ingress is ignored.
     *
     * @param other - Record to compare with.
     * @return - Ordering of the keys.
     */
    std::strong_ordering CompareKey(const TLogoBlobRec& other) const;

    /**
     * Orders by key, then by ingress.
     */
    friend bool operator<(const TLogoBlobRec& l, const TLogoBlobRec& r);

    friend bool operator==(const TLogoBlobRec& l, const TLogoBlobRec& r);

    /**
     * Formats the record as one tab-separated line: tablet_id, generation,
     * step, channel, blob_size, cookie, part_id, crc_mode, ingress, raw words.
     *
     * @return - The line without a trailing newline.
     */
    TString ToTsv() const;
};

/**
 * Header line matching TLogoBlobRec::ToTsv.
 *
 * @return - The header line without a trailing newline.
 */
TString TsvHeader();

}   // namespace NCloud::NStorage::NLsmCompress
