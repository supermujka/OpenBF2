#include "WireBitStream.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace bf2bot {

uint32_t bitsToHold(uint32_t span) {
    if (span == 0) {
        return 0;
    }
    uint32_t bits = 0;
    do {
        ++bits;
    } while (bits < 32 && span > ((1u << bits) - 1u));
    return bits;
}

BitStream::BitStream(uint32_t sizeBytes)
    : m_ownedBuf(sizeBytes, 0),
      m_writeBuf(sizeBytes > 0 ? &m_ownedBuf[0] : nullptr),
      m_readBuf(sizeBytes > 0 ? &m_ownedBuf[0] : nullptr),
      m_capacityBits(sizeBytes * 8u),
      m_bitLimit(0),
      m_bitOffset(0),
      m_highWaterBits(0),
      m_error(false),
      m_errorCode(0),
      m_compressionVector(),
      m_hasCompressionVector(false) {}

BitStream::BitStream(uint8_t* buf, uint32_t sizeBytes)
    : m_ownedBuf(),
      m_writeBuf(buf),
      m_readBuf(buf),
      m_capacityBits(buf ? sizeBytes * 8u : 0u),
      m_bitLimit(0),
      m_bitOffset(0),
      m_highWaterBits(0),
      m_error(false),
      m_errorCode(0),
      m_compressionVector(),
      m_hasCompressionVector(false) {}

BitStream::BitStream(const uint8_t* buf, uint32_t sizeBytes)
    : m_ownedBuf(),
      m_writeBuf(nullptr),
      m_readBuf(buf),
      m_capacityBits(buf ? sizeBytes * 8u : 0u),
      m_bitLimit(0),
      m_bitOffset(0),
      m_highWaterBits(buf ? sizeBytes * 8u : 0u),
      m_error(false),
      m_errorCode(0),
      m_compressionVector(),
      m_hasCompressionVector(false) {}

BitStream::BitStream(const BitStream& other)
    : m_ownedBuf(other.m_ownedBuf),
      m_writeBuf(other.m_writeBuf),
      m_readBuf(other.m_readBuf),
      m_capacityBits(other.m_capacityBits),
      m_bitLimit(other.m_bitLimit),
      m_bitOffset(other.m_bitOffset),
      m_highWaterBits(other.m_highWaterBits),
      m_error(other.m_error),
      m_errorCode(other.m_errorCode),
      m_compressionVector(other.m_compressionVector),
      m_hasCompressionVector(other.m_hasCompressionVector) {
    if (!m_ownedBuf.empty()) {
        m_writeBuf = &m_ownedBuf[0];
        m_readBuf = &m_ownedBuf[0];
    }
}

BitStream& BitStream::operator=(const BitStream& other) {
    if (this == &other) {
        return *this;
    }
    m_ownedBuf = other.m_ownedBuf;
    m_writeBuf = other.m_writeBuf;
    m_readBuf = other.m_readBuf;
    m_capacityBits = other.m_capacityBits;
    m_bitLimit = other.m_bitLimit;
    m_bitOffset = other.m_bitOffset;
    m_highWaterBits = other.m_highWaterBits;
    m_error = other.m_error;
    m_errorCode = other.m_errorCode;
    m_compressionVector = other.m_compressionVector;
    m_hasCompressionVector = other.m_hasCompressionVector;
    if (!m_ownedBuf.empty()) {
        m_writeBuf = &m_ownedBuf[0];
        m_readBuf = &m_ownedBuf[0];
    }
    return *this;
}

BitStream::BitStream(BitStream&& other)
    : m_ownedBuf(std::move(other.m_ownedBuf)),
      m_writeBuf(other.m_writeBuf),
      m_readBuf(other.m_readBuf),
      m_capacityBits(other.m_capacityBits),
      m_bitLimit(other.m_bitLimit),
      m_bitOffset(other.m_bitOffset),
      m_highWaterBits(other.m_highWaterBits),
      m_error(other.m_error),
      m_errorCode(other.m_errorCode),
      m_compressionVector(other.m_compressionVector),
      m_hasCompressionVector(other.m_hasCompressionVector) {
    if (!m_ownedBuf.empty()) {
        m_writeBuf = &m_ownedBuf[0];
        m_readBuf = &m_ownedBuf[0];
    }
    other.m_writeBuf = nullptr;
    other.m_readBuf = nullptr;
    other.m_capacityBits = 0;
    other.m_highWaterBits = 0;
}

BitStream& BitStream::operator=(BitStream&& other) {
    if (this == &other) {
        return *this;
    }
    m_ownedBuf = std::move(other.m_ownedBuf);
    m_writeBuf = other.m_writeBuf;
    m_readBuf = other.m_readBuf;
    m_capacityBits = other.m_capacityBits;
    m_bitLimit = other.m_bitLimit;
    m_bitOffset = other.m_bitOffset;
    m_highWaterBits = other.m_highWaterBits;
    m_error = other.m_error;
    m_errorCode = other.m_errorCode;
    m_compressionVector = other.m_compressionVector;
    m_hasCompressionVector = other.m_hasCompressionVector;
    if (!m_ownedBuf.empty()) {
        m_writeBuf = &m_ownedBuf[0];
        m_readBuf = &m_ownedBuf[0];
    }
    other.m_writeBuf = nullptr;
    other.m_readBuf = nullptr;
    other.m_capacityBits = 0;
    other.m_highWaterBits = 0;
    return *this;
}

void BitStream::clear() {
    if (m_writeBuf && m_capacityBits > 0) {
        std::memset(m_writeBuf, 0, m_capacityBits >> 3);
    }
    m_bitLimit = 0;
    m_bitOffset = 0;
    m_highWaterBits = 0;
    m_error = false;
    m_errorCode = 0;
    m_compressionVector = Vec3();
    m_hasCompressionVector = false;
}

bool BitStream::setData(const uint8_t* src, uint32_t sizeBytes) {
    if (!src && sizeBytes > 0) {
        return false;
    }
    if (!m_ownedBuf.empty()) {
        if (sizeBytes > m_ownedBuf.size()) {
            m_ownedBuf.resize(sizeBytes);
        }
        m_writeBuf = sizeBytes > 0 ? &m_ownedBuf[0] : nullptr;
        m_readBuf = m_writeBuf;
        m_capacityBits = static_cast<uint32_t>(m_ownedBuf.size()) * 8u;
    } else if (!m_writeBuf || sizeBytes * 8u > m_capacityBits) {
        return false;
    }

    m_bitLimit = 0;
    m_bitOffset = 0;
    m_error = false;
    m_errorCode = 0;
    m_compressionVector = Vec3();
    m_hasCompressionVector = false;

    if (sizeBytes > 0) {
        std::memcpy(m_writeBuf, src, sizeBytes);
    }
    m_highWaterBits = sizeBytes * 8u;
    return true;
}

void BitStream::setBitLimit(uint32_t bitLimit) {
    m_bitLimit = bitLimit;
}

bool BitStream::writeBits(const void* src, uint32_t nbits) {
    if (!src || nbits == 0 || !m_writeBuf || m_error) {
        return false;
    }

    const uint32_t end = m_bitOffset + nbits;
    if (end < m_bitOffset || end > m_capacityBits || (m_bitLimit != 0 && end > m_bitLimit)) {
        m_error = true;
        m_errorCode = 1;
        return false;
    }

    const uint8_t* srcBytes = static_cast<const uint8_t*>(src);
    uint32_t i = 0;
    while (i < nbits) {
        const uint32_t dstPos = m_bitOffset + i;
        const uint32_t dstByte = dstPos >> 3;
        const uint32_t dstBit = dstPos & 7u;
        const uint32_t srcByte = i >> 3;
        const uint32_t srcBit = i & 7u;

        uint32_t chunk = 8u - (dstBit > srcBit ? dstBit : srcBit);
        if (chunk > nbits - i) {
            chunk = nbits - i;
        }

        const uint8_t mask = static_cast<uint8_t>((1u << chunk) - 1u);
        const uint8_t bits = static_cast<uint8_t>((srcBytes[srcByte] >> srcBit) & mask);
        m_writeBuf[dstByte] = static_cast<uint8_t>(
            (m_writeBuf[dstByte] & static_cast<uint8_t>(~(mask << dstBit))) |
            static_cast<uint8_t>(bits << dstBit));
        i += chunk;
    }

    m_bitOffset = end;
    if (end > m_highWaterBits) {
        m_highWaterBits = end;
    }
    return true;
}

bool BitStream::readBits(void* dst, uint32_t nbits) {
    if (!dst || nbits == 0 || !m_readBuf || m_error) {
        return false;
    }

    const uint32_t end = m_bitOffset + nbits;
    if (end < m_bitOffset || end > m_highWaterBits) {
        m_error = true;
        m_errorCode = 2;
        return false;
    }

    uint8_t* dstBytes = static_cast<uint8_t*>(dst);
    const uint32_t outBytes = (nbits + 7u) >> 3;
    std::memset(dstBytes, 0, outBytes);

    uint32_t i = 0;
    while (i < nbits) {
        const uint32_t srcPos = m_bitOffset + i;
        const uint32_t srcByte = srcPos >> 3;
        const uint32_t srcBit = srcPos & 7u;
        const uint32_t dstByte = i >> 3;
        const uint32_t dstBit = i & 7u;

        uint32_t chunk = 8u - (srcBit > dstBit ? srcBit : dstBit);
        if (chunk > nbits - i) {
            chunk = nbits - i;
        }

        const uint8_t mask = static_cast<uint8_t>((1u << chunk) - 1u);
        const uint8_t bits = static_cast<uint8_t>((m_readBuf[srcByte] >> srcBit) & mask);
        dstBytes[dstByte] = static_cast<uint8_t>(dstBytes[dstByte] | static_cast<uint8_t>(bits << dstBit));
        i += chunk;
    }

    m_bitOffset = end;
    return true;
}

bool BitStream::writeBool(bool v) {
    uint32_t b = v ? 1u : 0u;
    return writeBits(&b, 1);
}

bool BitStream::readBool() {
    uint32_t b = 0;
    readBits(&b, 1);
    return b != 0;
}

bool BitStream::writeUInt(uint32_t v, uint32_t nbits) {
    return writeBits(&v, nbits);
}

uint32_t BitStream::readUInt(uint32_t nbits) {
    uint32_t v = 0;
    readBits(&v, nbits);
    return v;
}

bool BitStream::writeFloat(float v) {
    return writeBits(&v, 32);
}

float BitStream::readFloat() {
    float v = 0.0f;
    readBits(&v, 32);
    return v;
}

bool BitStream::writeSignedMag(int32_t v, uint32_t totalBits) {
    if (totalBits < 2 || totalBits > 32) {
        m_error = true;
        m_errorCode = 4;
        return false;
    }
    uint32_t sign = (v < 0) ? 1u : 0u;
    uint32_t mag = (v < 0) ? (0u - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
    if (!writeBits(&sign, 1)) {
        return false;
    }
    return writeBits(&mag, totalBits - 1u);
}

int32_t BitStream::readSignedMag(uint32_t totalBits) {
    if (totalBits < 2 || totalBits > 32) {
        m_error = true;
        m_errorCode = 4;
        return 0;
    }
    uint32_t sign = 0;
    uint32_t mag = 0;
    if (!readBits(&sign, 1) || !readBits(&mag, totalBits - 1u)) {
        return 0;
    }
    return (sign == 1u) ? -static_cast<int32_t>(mag) : static_cast<int32_t>(mag);
}

bool BitStream::writeRanged(uint32_t v, uint32_t upper) {
    if (v > upper) {
        m_error = true;
        m_errorCode = 4;
        return false;
    }
    const uint32_t bits = bitsToHold(upper + 1u);
    if (bits == 0) {
        return true;
    }
    return writeBits(&v, bits);
}

uint32_t BitStream::readRanged(uint32_t upper) {
    const uint32_t bits = bitsToHold(upper + 1u);
    if (bits == 0) {
        return 0;
    }
    uint32_t v = 0;
    readBits(&v, bits);
    return v;
}

bool BitStream::writeStringU16(const std::string& s) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    if (len > 0xFFFFu) {
        m_error = true;
        m_errorCode = 4;
        return false;
    }
    if (!writeUInt(len, 16)) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    return writeBits(s.data(), len * 8u);
}

std::string BitStream::readStringU16(uint32_t maxLen) {
    const uint32_t len = readUInt(16);
    if (m_error) {
        return std::string();
    }
    if (len == 0) {
        return std::string();
    }
    if (len > maxLen) {
        m_error = true;
        m_errorCode = 4;
        return std::string();
    }
    std::string out(len, '\0');
    if (!readBits(&out[0], len * 8u)) {
        return std::string();
    }
    return out;
}

bool BitStream::writeFixedBytes(const void* src, uint32_t len) {
    if (len == 0) {
        return true;
    }
    return writeBits(src, len * 8u);
}

bool BitStream::readFixedBytes(void* dst, uint32_t len) {
    if (len == 0) {
        return true;
    }
    return readBits(dst, len * 8u);
}

void BitStream::writeCompressedVector(const Vec3& v, const Vec3& ref, float scale) {
    static const uint32_t kTable[4] = { 32u, 20u, 16u, 12u };
    const float invScale = 1.0f / scale;
    const float dx = v.x - ref.x;
    const float dy = v.y - ref.y;
    const float dz = v.z - ref.z;
    const float scaledLen = std::sqrt(dx * dx + dy * dy + dz * dz) * invScale;

    uint32_t selector = 0;
    if (static_cast<float>(1u << (kTable[3] - 1u)) > scaledLen) {
        selector = 3;
    } else if (static_cast<float>(1u << (kTable[2] - 1u)) > scaledLen) {
        selector = 2;
    } else if (static_cast<float>(1u << (kTable[1] - 1u)) > scaledLen) {
        selector = 1;
    } else {
        selector = 0;
    }

    writeUInt(selector, 2);
    if (selector == 0) {
        writeFloat(v.x);
        writeFloat(v.y);
        writeFloat(v.z);
        return;
    }

    const uint32_t width = kTable[selector];
    const float d[3] = { dx, dy, dz };
    for (int i = 0; i < 3; ++i) {
        int32_t q = static_cast<int32_t>(d[i] * invScale);
        writeSignedMag(q, width);
    }
}

Vec3 BitStream::readCompressedVector(const Vec3& ref, float scale) {
    static const uint32_t kTable[4] = { 32u, 20u, 16u, 12u };
    Vec3 out;
    const uint32_t selector = readUInt(2);
    if (m_error) {
        return out;
    }
    if (selector == 0) {
        out.x = readFloat();
        out.y = readFloat();
        out.z = readFloat();
        return out;
    }

    const uint32_t width = kTable[selector];
    float q[3];
    for (int i = 0; i < 3; ++i) {
        q[i] = static_cast<float>(readSignedMag(width));
    }
    out.x = q[0] * scale + ref.x;
    out.y = q[1] * scale + ref.y;
    out.z = q[2] * scale + ref.z;
    return out;
}

void BitStream::setCompressionVector(const Vec3& ref) {
    m_compressionVector = ref;
    m_hasCompressionVector = true;
}

void BitStream::clearCompressionVector() {
    m_compressionVector = Vec3();
    m_hasCompressionVector = false;
}

BitPos BitStream::getBitPos() const {
    BitPos pos;
    pos.bytePos = m_bitOffset >> 3;
    pos.bitPos = m_bitOffset & 7u;
    return pos;
}

bool BitStream::setBitOffset(uint32_t bitOffset) {
    const uint32_t limit = m_writeBuf ? m_capacityBits : m_highWaterBits;
    if (bitOffset > limit) {
        return false;
    }
    m_bitOffset = bitOffset;
    return true;
}

bool BitStream::skipBits(uint32_t nbits) {
    if (m_error) {
        return false;
    }
    const uint32_t end = m_bitOffset + nbits;
    const uint32_t limit = m_writeBuf ? m_capacityBits : m_highWaterBits;
    if (end < m_bitOffset || end > limit || (m_writeBuf && m_bitLimit != 0 && end > m_bitLimit)) {
        m_error = true;
        m_errorCode = m_writeBuf ? 1u : 2u;
        return false;
    }
    m_bitOffset = end;
    if (m_writeBuf && end > m_highWaterBits) {
        m_highWaterBits = end;
    }
    return true;
}

void BitStream::setBitSize(uint32_t nbits) {
    if (nbits <= m_capacityBits) {
        m_highWaterBits = nbits;
    }
}

} // namespace bf2bot
