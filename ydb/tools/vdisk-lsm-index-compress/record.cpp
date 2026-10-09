#include "record.h"

#include <util/string/builder.h>
#include <util/string/printf.h>

#include <cinttypes>
#include <tuple>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

TLogoBlobRec TLogoBlobRec::Make(
    ui64 tabletId,
    ui32 generation,
    ui32 step,
    ui32 channel,
    ui32 cookie,
    ui32 blobSize,
    ui32 crcMode,
    ui32 partId,
    ui64 ingress)
{
    TLogoBlobRec rec;
    rec.Raw[0] = tabletId;
    rec.Raw[1] = (static_cast<ui64>(step >> 8) & 0xFFFFFFULL)
        | (static_cast<ui64>(generation) << 24)
        | (static_cast<ui64>(channel & 0xFFU) << 56);
    rec.Raw[2] = (static_cast<ui64>(partId) & 0xFULL)
        | ((static_cast<ui64>(blobSize) & 0x3FFFFFFULL) << 4)
        | ((static_cast<ui64>(crcMode) & 0x3ULL) << 30)
        | ((static_cast<ui64>(cookie) & 0xFFFFFFULL) << 32)
        | (static_cast<ui64>(step & 0xFFU) << 56);
    rec.Ingress = ingress;
    return rec;
}

std::strong_ordering TLogoBlobRec::CompareKey(const TLogoBlobRec& other) const
{
    return std::tie(Raw[0], Raw[1], Raw[2])
        <=> std::tie(other.Raw[0], other.Raw[1], other.Raw[2]);
}

bool operator<(const TLogoBlobRec& l, const TLogoBlobRec& r)
{
    return std::tie(l.Raw[0], l.Raw[1], l.Raw[2], l.Ingress)
        < std::tie(r.Raw[0], r.Raw[1], r.Raw[2], r.Ingress);
}

bool operator==(const TLogoBlobRec& l, const TLogoBlobRec& r)
{
    return l.Raw[0] == r.Raw[0] && l.Raw[1] == r.Raw[1]
        && l.Raw[2] == r.Raw[2] && l.Ingress == r.Ingress;
}

TString TLogoBlobRec::ToTsv() const
{
    return TStringBuilder()
        << TabletId() << '\t' << Generation() << '\t' << Step() << '\t'
        << Channel() << '\t' << BlobSize() << '\t' << Cookie() << '\t'
        << PartId() << '\t' << CrcMode() << '\t'
        << Sprintf("0x%016" PRIx64, Ingress) << '\t'
        << Sprintf("0x%016" PRIx64, Raw[0]) << '\t'
        << Sprintf("0x%016" PRIx64, Raw[1]) << '\t'
        << Sprintf("0x%016" PRIx64, Raw[2]);
}

TString TsvHeader()
{
    return "tablet_id\tgeneration\tstep\tchannel\tblob_size\tcookie\t"
           "part_id\tcrc_mode\tingress\traw0\traw1\traw2";
}

}   // namespace NCloud::NStorage::NLsmCompress
