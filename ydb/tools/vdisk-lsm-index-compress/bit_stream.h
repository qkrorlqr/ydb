#pragma once

#include <util/generic/strbuf.h>
#include <util/generic/vector.h>
#include <util/system/byteorder.h>
#include <util/system/types.h>
#include <util/system/unaligned_mem.h>
#include <util/system/yassert.h>

namespace NCloud::NStorage::NLsmCompress {

////////////////////////////////////////////////////////////////////////////////

/**
 * Appends bits to a byte buffer, most significant bit first.
 */
class TBitWriter
{
private:
    TVector<ui8> Data;
    ui64 BitCount = 0;

public:
    /**
     * Appends the low bits of a value, most significant of them first.
     *
     * @param value - Value to append.
     * @param bits - Number of low bits of value to append, at most 64.
     */
    void Write(ui64 value, ui32 bits)
    {
        Y_DEBUG_ABORT_UNLESS(bits <= 64);
        for (ui32 i = bits; i > 0; --i) {
            WriteBit((value >> (i - 1)) & 1ULL);
        }
    }

    /**
     * Appends one bit.
     *
     * @param bit - Bit value, 0 or 1.
     */
    void WriteBit(ui64 bit)
    {
        if (BitCount % 8 == 0) {
            Data.push_back(0);
        }
        if (bit) {
            Data.back() |= 0x80U >> (BitCount % 8);
        }
        ++BitCount;
    }

    /**
     * @return - Number of bits written.
     */
    ui64 GetBitCount() const
    {
        return BitCount;
    }

    /**
     * @return - Written bytes, the last byte is padded with zero bits.
     */
    TStringBuf GetData() const
    {
        return {reinterpret_cast<const char*>(Data.data()), Data.size()};
    }
};

////////////////////////////////////////////////////////////////////////////////

/**
 * Reads bits written by TBitWriter. Bytes past the end of the buffer read
 * as zero; the caller checks the position with Overrun.
 */
class TBitReader
{
public:
    // Largest value of Peek bits: one 8-byte load covers the bits at any
    // position inside a byte.
    static constexpr ui32 MaxPeekBits = 57;

private:
    const ui8* Data = nullptr;
    ui64 BitCount = 0;
    ui64 ByteCount = 0;
    ui64 Position = 0;

public:
    /**
     * @param data - Buffer start.
     * @param bitCount - Number of valid bits in the buffer.
     * @param position - Bit position to start reading from.
     */
    TBitReader(const ui8* data, ui64 bitCount, ui64 position)
        : Data(data)
        , BitCount(bitCount)
        , ByteCount((bitCount + 7) / 8)
        , Position(position)
    {}

    /**
     * Returns the next bits without moving the position.
     *
     * @param bits - Number of bits, from 1 to MaxPeekBits.
     * @return - The bits as an unsigned value, first bit most significant.
     */
    ui64 Peek(ui32 bits) const
    {
        Y_DEBUG_ABORT_UNLESS(bits >= 1 && bits <= MaxPeekBits);

        //
        // Load 8 bytes starting with the current byte, most significant
        // first. Near the end of the buffer only the bytes inside it are
        // loaded.
        //

        const ui64 byte = Position / 8;
        ui64 word = 0;
        if (byte + 8 <= ByteCount) {
            word = SwapBytes64(ReadUnaligned<ui64>(Data + byte));
        } else {
            for (ui64 i = 0; i < 8 && byte + i < ByteCount; ++i) {
                word |= static_cast<ui64>(Data[byte + i]) << (56 - 8 * i);
            }
        }
        return (word << (Position % 8)) >> (64 - bits);
    }

    /**
     * Moves the position forward.
     *
     * @param bits - Number of bits to skip.
     */
    void Skip(ui64 bits)
    {
        Position += bits;
    }

    /**
     * Reads a value written by TBitWriter::Write.
     *
     * @param bits - Number of bits to read, at most 64.
     * @return - The value.
     */
    ui64 Read(ui32 bits)
    {
        if (bits == 0) {
            return 0;
        }
        if (bits > MaxPeekBits) {
            const ui64 high = Read(bits - 32);
            return (high << 32) | Read(32);
        }
        const ui64 value = Peek(bits);
        Position += bits;
        return value;
    }

    /**
     * Reads one bit.
     *
     * @return - Bit value, 0 or 1.
     */
    ui64 ReadBit()
    {
        return Read(1);
    }

    /**
     * @return - Current bit position.
     */
    ui64 GetPosition() const
    {
        return Position;
    }

    /**
     * @return - True if the reader went past the last valid bit.
     */
    bool Overrun() const
    {
        return Position > BitCount;
    }
};

}   // namespace NCloud::NStorage::NLsmCompress
