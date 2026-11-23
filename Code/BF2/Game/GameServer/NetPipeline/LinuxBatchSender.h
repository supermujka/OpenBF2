// [CHOSEN, not recovered] Phase 2A batched UDP packet sender for 32v32 dedicated servers.
// Acquires NetServer::m_mutex once per chunk of up to 64 packets across SWConnection buffer
// insertion, live sequence/ack header stamping, and Linux sendmmsg() dispatch (with non-Linux
// fallback), eliminating sequence-number inversion with NetServerThread and per-client lock/syscall
// overhead. Zero ABI, vtable, or class-layout changes.

#ifndef GAME_GAMESERVER_NETPIPELINE_LINUXBATCHSENDER_H
#define GAME_GAMESERVER_NETPIPELINE_LINUXBATCHSENDER_H

#include <cstddef>

namespace dice {
namespace hfe {

namespace io {
class BitStream;
class NetServer;
} // namespace io

// Per-packet result reported by LinuxBatchSender::sendBatch.
struct BatchResult
{
    int          bufferStatus;  // Matches INetServer::bufferPacket: 0 = queued, 15 = send ring full, 5 = missing conn, 16 = too big
    unsigned int sendStatus;    // Matches SWConnection::send: 0 = drained, 15 = ack window full, 6 = socket error, 5 = no socket, 9 = missing conn
    unsigned int datagramsSent; // Number of UDP datagrams actually sent to the OS socket for this connection
};

// Descriptor for one connection's outbound snapshot packet in a batch.
struct BatchPacket
{
    int                  connectionId;
    io::BitStream*       stream;
    const unsigned char* data;
    unsigned int         sizeBytes;
    void*                userContext;

    // Populated in-place by LinuxBatchSender::sendBatch (even when passed as const BatchPacket*).
    mutable int          bufferStatus;
    mutable unsigned int sendStatus;
    mutable unsigned int datagramsSent;

    // Optional caller-provided output pointers (updated if non-null).
    int*                 outBufferStatus;
    unsigned int*        outSendStatus;
    unsigned int*        outDatagramsSent;

    BatchPacket()
        : connectionId(-1)
        , stream(0)
        , data(0)
        , sizeBytes(0)
        , userContext(0)
        , bufferStatus(5)
        , sendStatus(9u)
        , datagramsSent(0u)
        , outBufferStatus(0)
        , outSendStatus(0)
        , outDatagramsSent(0)
    {
    }

    BatchPacket(int connId, io::BitStream* bitStream, unsigned int bytes = 0u, void* ctx = 0,
                int* outBufStat = 0, unsigned int* outSndStat = 0, unsigned int* outDgrams = 0)
        : connectionId(connId)
        , stream(bitStream)
        , data(0)
        , sizeBytes(bytes)
        , userContext(ctx)
        , bufferStatus(5)
        , sendStatus(9u)
        , datagramsSent(0u)
        , outBufferStatus(outBufStat)
        , outSendStatus(outSndStat)
        , outDatagramsSent(outDgrams)
    {
    }

    BatchPacket(int connId, const unsigned char* payload, unsigned int bytes, void* ctx = 0,
                int* outBufStat = 0, unsigned int* outSndStat = 0, unsigned int* outDgrams = 0)
        : connectionId(connId)
        , stream(0)
        , data(payload)
        , sizeBytes(bytes)
        , userContext(ctx)
        , bufferStatus(5)
        , sendStatus(9u)
        , datagramsSent(0u)
        , outBufferStatus(outBufStat)
        , outSendStatus(outSndStat)
        , outDatagramsSent(outDgrams)
    {
    }
};

class LinuxBatchSender
{
public:
    enum
    {
        kMaxChunkSize = 64
    };

    // Lazy function-local static lookup of CVar "net-batch-send" (default 0 = vanilla path).
    static bool isEnabled();

    // Buffers, stamps 6-bit sequence/ack headers, and transmits up to count packets in chunks of 64
    // under a single NetServer::m_mutex lock per chunk.
    // Populates each BatchPacket's bufferStatus, sendStatus, and datagramsSent fields (plus any
    // non-null out* pointers and optional outResults array).
    // Returns total UDP datagrams transmitted to the OS socket across the batch.
    static std::size_t sendBatch(io::NetServer* netServer,
                                 const BatchPacket* packets,
                                 std::size_t count,
                                 BatchResult* outResults = 0);

    // Convenience overload writing per-packet bufferStatus values into outBufferStatuses[0..count-1].
    static std::size_t sendBatch(io::NetServer* netServer,
                                 const BatchPacket* packets,
                                 std::size_t count,
                                 int* outBufferStatuses);
};

} // namespace hfe
} // namespace dice

#endif // GAME_GAMESERVER_NETPIPELINE_LINUXBATCHSENDER_H
