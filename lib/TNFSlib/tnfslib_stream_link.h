#ifndef _TNFSLIB_STREAM_LINK_H
#define _TNFSLIB_STREAM_LINK_H

// TNFS requests and replies over a byte stream, for any tnfsStreamTransport. The link owns
// the rules, not the state: the transport, the reply buffer and the reply format belong to
// the mount.
// - A new connection starts with an empty reply buffer, allocated only while connected.
// - A request is marked as sent before it is written: bytes received before it cannot be its
//   answer.
// - Replies are cut out of the stream for the pending request; only its answer is returned,
//   and replies to earlier requests or to no request of ours are skipped.
// - An optional field whose use the mount's format leaves unknown is settled before anything
//   else is sent (tnfsFormatPolicy), so it can never be read as the next request's answer.
// - A malformed reply or a connection closed by the server drops the connection, as does a
//   timeout that may hold part of the answer; the next send reconnects.

#include <cstddef>
#include <cstdint>

#include "tnfslib.h"
#include "tnfslib_framing.h"
#include "tnfslib_stream_transport.h"

enum class tnfsStreamStatus
{
    REPLY,   // the answer, `length` bytes, is in `out`
    PENDING, // nothing complete yet; poll again until the transaction times out
    LOST,    // the connection was dropped; the next send reconnects
};

struct tnfsStreamReply
{
    tnfsStreamStatus status;
    int length; // reply length when REPLY, otherwise 0
};

class tnfsStreamLink
{
public:
    tnfsStreamLink(tnfsStreamTransport &transport, tnfsMountInfo &mount);

    // Connect first if needed, with a new empty reply buffer, then write the request's first
    // `len` bytes (header and payload).
    success_is_true send(const tnfsPacket &request, size_t len);

    // The answer to `request`. Reads at most the reply buffer's free space; the rest stays
    // queued in the transport. `out` is also the read buffer.
    tnfsStreamReply recv(uint8_t (&out)[TNFS_MAX_REPLY_SIZE], const tnfsPacket &request);

    // The caller stopped waiting for the answer to `request`. If what is buffered could be
    // completed into a wrong answer by the retry's reply, it is dropped with the connection.
    void expire(const tnfsPacket &request);

    // Close the connection and free the reply buffer.
    void drop();

private:
    tnfsReplyContext context(const tnfsPacket &request) const;
    bool infers() const;
    void settle();

    tnfsStreamTransport &_transport;
    tnfsMountInfo &_mount;
};

#endif // _TNFSLIB_STREAM_LINK_H
