#ifndef BF2BOT_WIRE_BIT_STREAM_H
#define BF2BOT_WIRE_BIT_STREAM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf2bot {

struct Vec3 {
    float x;
    float y;
    float z;

    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct BitPos {
    uint32_t bytePos;
    uint32_t bitPos;
};

// Computes the smallest bit count >= 1 such that ((1u << bits) - 1u) >= span.
// Matches BF2's BitStream.inl ranged-bit width loop (used with span = upper + 1).
uint32_t bitsToHold(uint32_t span);

// Standalone, bit-exact LSB-first BitStream matching OpenBF2's Code/BF2/IO/BitStream.cpp.
class BitStream {
public:
    explicit BitStream(uint32_t sizeBytes = 1500);
    BitStream(uint8_t* buf, uint32_t sizeBytes);
    BitStream(const uint8_t* buf, uint32_t sizeBytes);
    BitStream(const BitStream& other);
    BitStream& operator=(const BitStream& other);
    BitStream(BitStream&& other);
    BitStream& operator=(BitStream&& other);

    // Resets stream for writing into its current writable buffer.
    void clear();

    // Copies src[0..sizeBytes-1] into the buffer (resizing if owned) and prepares for reading.
    bool setData(const uint8_t* src, uint32_t sizeBytes);

    // Sets an optional write bit limit (0 = unlimited).
    void setBitLimit(uint32_t bitLimit);
    uint32_t getBitLimit() const { return m_bitLimit; }

    // Core bit-level primitives (LSB-first, little-endian memory layout).
    bool writeBits(const void* src, uint32_t nbits);
    bool readBits(void* dst, uint32_t nbits);

    // Scalar helpers matching BF2 wire conventions.
    bool writeBool(bool v);
    bool readBool();

    bool writeUInt(uint32_t v, uint32_t nbits);
    uint32_t readUInt(uint32_t nbits);

    bool writeFloat(float v);
    float readFloat();

    // 1-bit sign + (totalBits - 1)-bit magnitude (matches BF2 ServerInfo/ClientInfo/PlayerActionManager).
    bool writeSignedMag(int32_t v, uint32_t totalBits);
    int32_t readSignedMag(uint32_t totalBits);

    // Ranged unsigned integer in [0, upper] using bitsToHold(upper + 1) bits.
    bool writeRanged(uint32_t v, uint32_t upper);
    uint32_t readRanged(uint32_t upper);

    // Length-prefixed string: u16 byte length + len*8 raw bytes (no NUL terminator).
    bool writeStringU16(const std::string& s);
    std::string readStringU16(uint32_t maxLen = 4096);

    // Fixed byte arrays (nbits = len * 8).
    bool writeFixedBytes(const void* src, uint32_t len);
    bool readFixedBytes(void* dst, uint32_t len);

    // BF2 CompressedVector (2-bit selector, table {32, 20, 16, 12}).
    void writeCompressedVector(const Vec3& v, const Vec3& ref, float scale);
    Vec3 readCompressedVector(const Vec3& ref, float scale);

    // Relative compression reference vector state.
    void setCompressionVector(const Vec3& ref);
    void clearCompressionVector();
    bool hasCompressionVector() const { return m_hasCompressionVector; }
    const Vec3& getCompressionVector() const { return m_compressionVector; }

    // Cursor & state inspection.
    uint32_t getBitOffset() const { return m_bitOffset; }
    BitPos getBitPos() const;
    bool setBitOffset(uint32_t bitOffset);
    bool skipBits(uint32_t nbits);

    uint32_t getBitSize() const { return m_highWaterBits; }
    void setBitSize(uint32_t nbits);
    uint32_t getByteSize() const { return (m_highWaterBits + 7u) >> 3; }
    uint32_t getCapacityBits() const { return m_capacityBits; }

    bool hasError() const { return m_error; }
    uint32_t getErrorCode() const { return m_errorCode; }
    void clearError() { m_error = false; m_errorCode = 0; }

    const uint8_t* data() const { return m_readBuf; }
    uint8_t* mutableData() { return m_writeBuf; }

private:
    std::vector<uint8_t> m_ownedBuf;
    uint8_t* m_writeBuf;
    const uint8_t* m_readBuf;
    uint32_t m_capacityBits;
    uint32_t m_bitLimit;
    uint32_t m_bitOffset;
    uint32_t m_highWaterBits;
    bool m_error;
    uint32_t m_errorCode;
    Vec3 m_compressionVector;
    bool m_hasCompressionVector;
};

} // namespace bf2bot

#endif // BF2BOT_WIRE_BIT_STREAM_H
