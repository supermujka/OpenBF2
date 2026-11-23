// [CHOSEN, not recovered] Phase 2A batched UDP packet sender for 32v32 dedicated servers.
// Holds NetServer::m_mutex across SWConnection::addPacketToSendBuffer, live header stamping, and
// Linux sendmmsg() (or non-Linux SWConnection::send fallback) in chunks of up to 64 packets.

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#endif

#include "LinuxBatchSender.h"

#include "../../Utility/Vars.h"
#include "../../../IO/BitStream.h"
#include "../../../IO/Network/NetworkLayer/Socket/UDPSocket.h"
#include "../../../IO/Network/ProtocolLayer/NetServer.h"
#include "../../../IO/Network/ProtocolLayer/Packet.h"
#include "../../../IO/Network/ProtocolLayer/PacketBuffer.h"
#include "../../../IO/Network/ProtocolLayer/SWConnection.h"

#include <cstring>
#include <map>

namespace dice {
namespace hfe {

namespace {

inline void recordPacketResult(const BatchPacket& pkt,
                               BatchResult* outRes,
                               int* outBufStatElem,
                               int bufferStatus,
                               unsigned int sendStatus,
                               unsigned int datagramsSent)
{
    pkt.bufferStatus  = bufferStatus;
    pkt.sendStatus    = sendStatus;
    pkt.datagramsSent = datagramsSent;

    if (pkt.outBufferStatus)
        *pkt.outBufferStatus = bufferStatus;
    if (pkt.outSendStatus)
        *pkt.outSendStatus = sendStatus;
    if (pkt.outDatagramsSent)
        *pkt.outDatagramsSent = datagramsSent;

    if (outRes)
    {
        outRes->bufferStatus  = bufferStatus;
        outRes->sendStatus    = sendStatus;
        outRes->datagramsSent = datagramsSent;
    }
    if (outBufStatElem)
        *outBufStatElem = bufferStatus;
}

inline void resolvePayload(const BatchPacket& pkt,
                           unsigned char*& outSrc,
                           unsigned int& outBytes)
{
    if (pkt.stream)
    {
        unsigned int n = pkt.stream->m_highWaterBits >> 3;
        if ((pkt.stream->m_highWaterBits & 7u) != 0u)
            ++n;
        outSrc   = pkt.stream->getBuffer();
        outBytes = n;
    }
    else
    {
        outSrc   = const_cast<unsigned char*>(pkt.data);
        outBytes = pkt.sizeBytes;
    }
}

#ifdef __linux__

struct StagedConnection
{
    io::SWConnection* conn;
    std::size_t       packetIndex;
    int               bufferStatus;
    unsigned int      firstMsgIdx;
    unsigned int      msgCount;
    bool              hitWindowLimit;
};

struct LinuxSubBatch
{
    StagedConnection   conns[LinuxBatchSender::kMaxChunkSize];
    unsigned int       connCount;

    struct mmsghdr     msgs[LinuxBatchSender::kMaxChunkSize];
    struct iovec       iovs[LinuxBatchSender::kMaxChunkSize];
    struct sockaddr_in addrs[LinuxBatchSender::kMaxChunkSize];
    unsigned int       msgOwnerConnIdx[LinuxBatchSender::kMaxChunkSize];
    bool               msgSent[LinuxBatchSender::kMaxChunkSize];
    unsigned int       msgCount;

    void reset()
    {
        connCount = 0u;
        msgCount  = 0u;
    }

    bool containsConnection(const io::SWConnection* c) const
    {
        for (unsigned int i = 0u; i < connCount; ++i)
        {
            if (conns[i].conn == c)
                return true;
        }
        return false;
    }
};

// Dispatches all staged mmsghdr entries via ::sendmmsg (with EINTR retry, partial-send loop, and
// per-message ::sendto fallback on non-transient errors) and commits exact SWConnection & UDPSocket
// state transitions ONLY for packets confirmed transmitted by the kernel.
std::size_t flushLinuxSubBatch(LinuxSubBatch& sub,
                               io::UDPSocket* socket,
                               const BatchPacket* packets,
                               BatchResult* outResults,
                               int* outBufferStatuses)
{
    if (sub.connCount == 0u)
        return 0u;

    const int fd = static_cast<int>(socket->m_socket);

    if (sub.msgCount > 0u)
    {
        if (fd == -1)
        {
            socket->showError();
        }
        else
        {
            unsigned int sent = 0u;
            bool needSendtoFallback = false;

            while (sent < sub.msgCount)
            {
                const int r = ::sendmmsg(fd, &sub.msgs[sent], sub.msgCount - sent, 0);
                if (r > 0)
                {
                    for (int k = 0; k < r; ++k)
                        sub.msgSent[sent + static_cast<unsigned int>(k)] = true;
                    sent += static_cast<unsigned int>(r);
                }
                else if (r == -1)
                {
                    if (errno == EINTR)
                        continue;

                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        socket->showError();
                        break;
                    }

                    // Non-EAGAIN error (e.g. ENOSYS or per-peer netfilter EPERM / ENETUNREACH):
                    // fall back to ::sendto for remaining messages while preserving per-connection
                    // FIFO ordering.
                    socket->showError();
                    needSendtoFallback = true;
                    break;
                }
                else
                {
                    break;
                }
            }

            if (needSendtoFallback && sent < sub.msgCount)
            {
                bool connFailed[LinuxBatchSender::kMaxChunkSize];
                for (unsigned int ci = 0u; ci < sub.connCount; ++ci)
                    connFailed[ci] = false;

                for (unsigned int m = sent; m < sub.msgCount; ++m)
                {
                    const unsigned int ownerIdx = sub.msgOwnerConnIdx[m];
                    if (connFailed[ownerIdx])
                        continue;

                    for (;;)
                    {
                        const ssize_t s = ::sendto(
                            fd,
                            sub.iovs[m].iov_base,
                            sub.iovs[m].iov_len,
                            0,
                            reinterpret_cast<const struct sockaddr*>(&sub.addrs[m]),
                            static_cast<socklen_t>(16));
                        if (s != -1)
                        {
                            sub.msgSent[m] = true;
                            break;
                        }
                        if (errno == EINTR)
                            continue;

                        const int err = errno;
                        socket->showError();
                        connFailed[ownerIdx] = true;
                        if (err == EAGAIN || err == EWOULDBLOCK)
                            m = sub.msgCount;
                        break;
                    }
                }
            }
        }
    }

    std::size_t batchSentTotal = 0u;

    for (unsigned int ci = 0u; ci < sub.connCount; ++ci)
    {
        StagedConnection& sc = sub.conns[ci];
        io::SWConnection* const c = sc.conn;

        // Match SWConnection::send(ISocket*): aim socket's cached peer at this connection.
        socket->setPeerAddress(c->m_address);

        unsigned int connDatagramsSent = 0u;
        bool socketRefused = false;

        for (unsigned int m = sc.firstMsgIdx; m < sc.firstMsgIdx + sc.msgCount; ++m)
        {
            if (!sub.msgSent[m])
            {
                socketRefused = true;
                break;
            }

            io::Packet* const front = c->m_sendBuffer->getFront();
            if (front)
            {
                ++socket->m_packetsSent;
                ++c->m_dword58;
                c->m_localPacketCounter = static_cast<int>(front->dword8);
                c->m_dword68 += front->size;
                c->m_sendBuffer->popFront();
                ++connDatagramsSent;
                ++batchSentTotal;
            }
        }

        unsigned int sendStatus = 0u;
        if (socketRefused)
        {
            // Matches SWConnection::send returning 6 when socket->send returns 0:
            // unsent front packet stays queued in m_sendBuffer and updateStats() is not called.
            sendStatus = 6u;
        }
        else if (sc.hitWindowLimit)
        {
            // Matches SWConnection::send returning 15 when front packet exceeds the 30-packet
            // acknowledgement window: increment m_dword78 and skip updateStats().
            ++c->m_dword78;
            sendStatus = 15u;
        }
        else
        {
            c->updateStats();
            sendStatus = 0u;
        }

        const std::size_t pktIdx = sc.packetIndex;
        recordPacketResult(
            packets[pktIdx],
            outResults ? &outResults[pktIdx] : 0,
            outBufferStatuses ? &outBufferStatuses[pktIdx] : 0,
            sc.bufferStatus,
            sendStatus,
            connDatagramsSent);
    }

    sub.reset();
    return batchSentTotal;
}

#endif // __linux__

std::size_t sendBatchImpl(io::NetServer* netServer,
                          const BatchPacket* packets,
                          std::size_t count,
                          BatchResult* outResults,
                          int* outBufferStatuses)
{
    if (!packets || count == 0u)
        return 0u;

    if (!netServer)
    {
        for (std::size_t i = 0u; i < count; ++i)
        {
            recordPacketResult(
                packets[i],
                outResults ? &outResults[i] : 0,
                outBufferStatuses ? &outBufferStatuses[i] : 0,
                5,
                5u,
                0u);
        }
        return 0u;
    }

    std::size_t totalDatagramsSent = 0u;

#ifdef __linux__
    LinuxSubBatch sub;
    sub.reset();
#endif

    for (std::size_t chunkStart = 0u; chunkStart < count; chunkStart += LinuxBatchSender::kMaxChunkSize)
    {
        const std::size_t chunkEnd =
            (chunkStart + LinuxBatchSender::kMaxChunkSize < count)
                ? (chunkStart + LinuxBatchSender::kMaxChunkSize)
                : count;

        // Hold NetServer::m_mutex once per chunk across buffer insertion, live sequence/ack
        // header stamping, and UDP socket dispatch so NetServerThread can never interleave a
        // ping/handshake send with an inverted sequence counter on the wire.
        netServer->m_mutex.lock();

        io::UDPSocket* const socket = netServer->m_listenSocket2;

#ifdef __linux__
        sub.reset();

        for (std::size_t i = chunkStart; i < chunkEnd; ++i)
        {
            const BatchPacket& pkt = packets[i];

            std::map<int, io::SWConnection*>::iterator it =
                netServer->m_connections.find(pkt.connectionId);
            io::SWConnection* const c =
                (it == netServer->m_connections.end()) ? 0 : it->second;

            if (!c)
            {
                recordPacketResult(
                    pkt,
                    outResults ? &outResults[i] : 0,
                    outBufferStatuses ? &outBufferStatuses[i] : 0,
                    5,
                    9u,
                    0u);
                continue;
            }

            // If the same connection appears twice in the same chunk, flush prior staged packets
            // first (still under m_mutex) so m_sendBuffer is drained in exact sequential order.
            if (sub.containsConnection(c) || sub.connCount >= LinuxBatchSender::kMaxChunkSize)
            {
                totalDatagramsSent += flushLinuxSubBatch(
                    sub, socket, packets, outResults, outBufferStatuses);
            }

            unsigned char* src = 0;
            unsigned int n = 0u;
            resolvePayload(pkt, src, n);

            const int bufStatus = c->addPacketToSendBuffer(src, n, pkt.sizeBytes);

            if (!socket)
            {
                recordPacketResult(
                    pkt,
                    outResults ? &outResults[i] : 0,
                    outBufferStatuses ? &outBufferStatuses[i] : 0,
                    bufStatus,
                    5u,
                    0u);
                continue;
            }

            // Inspect c->m_sendBuffer in FIFO order to count how many queued packets are within
            // the 30-packet acknowledgement window (((dword8 - m_dword50) & 0x3F) <= 0x1E).
            io::PacketBuffer* const sendBuf = c->m_sendBuffer;
            const unsigned int queuedCount = sendBuf ? sendBuf->m_count : 0u;
            unsigned int eligibleCount = 0u;
            bool hitWindowLimit = false;

            for (unsigned int k = 0u; k < queuedCount; ++k)
            {
                const unsigned int ringIdx = (sendBuf->m_head + k) & sendBuf->m_mask;
                const io::Packet* const slotPkt = &sendBuf->m_packets[ringIdx];
                if (((slotPkt->dword8 - c->m_dword50) & 0x3Fu) > 0x1Eu)
                {
                    hitWindowLimit = true;
                    break;
                }
                ++eligibleCount;
            }

            // A single connection's send ring holds at most 32 packets (<= 31 window-eligible),
            // which always fits inside kMaxChunkSize (64). Flush first if adding eligibleCount
            // would overflow the current 64-message mmsghdr array.
            if (sub.msgCount > 0u
                && (sub.msgCount + eligibleCount > LinuxBatchSender::kMaxChunkSize))
            {
                totalDatagramsSent += flushLinuxSubBatch(
                    sub, socket, packets, outResults, outBufferStatuses);
            }

            socket->setPeerAddress(c->m_address);
            const io::ISocket::Address& to =
                socket->m_useOwnAddress ? socket->m_address : socket->m_peerAddress;

            const unsigned int connSlotIdx = sub.connCount;
            StagedConnection& sc = sub.conns[connSlotIdx];
            sc.conn           = c;
            sc.packetIndex    = i;
            sc.bufferStatus   = bufStatus;
            sc.firstMsgIdx    = sub.msgCount;
            sc.msgCount       = eligibleCount;
            sc.hitWindowLimit = hitWindowLimit;
            ++sub.connCount;

            for (unsigned int k = 0u; k < eligibleCount; ++k)
            {
                const unsigned int ringIdx = (sendBuf->m_head + k) & sendBuf->m_mask;
                io::Packet* const front = &sendBuf->m_packets[ringIdx];
                unsigned char* const slot =
                    static_cast<unsigned char*>(const_cast<void*>(front->pvoid14));

                // Rewrite the 60-bit basic + extended header in place with live counters,
                // bit-for-bit identical to SWConnection::send().
                c->m_sendStream->setBuffer(slot, 0x5C1u);
                io::writeBasicHeader(
                    c->m_sendStream, static_cast<unsigned char>(c->m_connectionId), 15);
                io::writeExtendedHeader(
                    c->m_sendStream,
                    front->dword8,
                    c->m_remotePacketCounter,
                    c->m_delayScaleFactor);

                const unsigned int m = sub.msgCount;
                std::memcpy(&sub.addrs[m], to.sockaddr, 16);

                sub.iovs[m].iov_base = slot;
                sub.iovs[m].iov_len  = static_cast<std::size_t>(front->size);

                std::memset(&sub.msgs[m], 0, sizeof(sub.msgs[m]));
                sub.msgs[m].msg_hdr.msg_name    = &sub.addrs[m];
                sub.msgs[m].msg_hdr.msg_namelen = 16;
                sub.msgs[m].msg_hdr.msg_iov     = &sub.iovs[m];
                sub.msgs[m].msg_hdr.msg_iovlen  = 1;

                sub.msgOwnerConnIdx[m] = connSlotIdx;
                sub.msgSent[m]         = false;
                ++sub.msgCount;
            }
        }

        if (sub.connCount > 0u)
        {
            totalDatagramsSent += flushLinuxSubBatch(
                sub, socket, packets, outResults, outBufferStatuses);
        }
#else
        // Non-Linux fallback (Windows / macOS): under the same single per-chunk NetServer::m_mutex
        // lock, buffer and invoke SWConnection::send(m_listenSocket2) directly.
        for (std::size_t i = chunkStart; i < chunkEnd; ++i)
        {
            const BatchPacket& pkt = packets[i];

            std::map<int, io::SWConnection*>::iterator it =
                netServer->m_connections.find(pkt.connectionId);
            io::SWConnection* const c =
                (it == netServer->m_connections.end()) ? 0 : it->second;

            if (!c)
            {
                recordPacketResult(
                    pkt,
                    outResults ? &outResults[i] : 0,
                    outBufferStatuses ? &outBufferStatuses[i] : 0,
                    5,
                    9u,
                    0u);
                continue;
            }

            unsigned char* src = 0;
            unsigned int n = 0u;
            resolvePayload(pkt, src, n);

            const int bufStatus = c->addPacketToSendBuffer(src, n, pkt.sizeBytes);
            const unsigned int sentBefore = c->m_dword58;
            const unsigned int sendStatus = c->send(socket);
            const unsigned int connDatagramsSent = c->m_dword58 - sentBefore;

            totalDatagramsSent += static_cast<std::size_t>(connDatagramsSent);

            recordPacketResult(
                pkt,
                outResults ? &outResults[i] : 0,
                outBufferStatuses ? &outBufferStatuses[i] : 0,
                bufStatus,
                sendStatus,
                connDatagramsSent);
        }
#endif

        netServer->m_mutex.unlock();
    }

    return totalDatagramsSent;
}

} // namespace

bool LinuxBatchSender::isEnabled()
{
    static int* const s_netBatchSend = Vars::getInt("net-batch-send", 0);
    return s_netBatchSend != 0 && (*s_netBatchSend != 0);
}

std::size_t LinuxBatchSender::sendBatch(io::NetServer* netServer,
                                        const BatchPacket* packets,
                                        std::size_t count,
                                        BatchResult* outResults)
{
    return sendBatchImpl(netServer, packets, count, outResults, 0);
}

std::size_t LinuxBatchSender::sendBatch(io::NetServer* netServer,
                                        const BatchPacket* packets,
                                        std::size_t count,
                                        int* outBufferStatuses)
{
    return sendBatchImpl(netServer, packets, count, 0, outBufferStatuses);
}

} // namespace hfe
} // namespace dice
