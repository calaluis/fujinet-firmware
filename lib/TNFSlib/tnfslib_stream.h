#ifndef _TNFSLIB_STREAM_H
#define _TNFSLIB_STREAM_H

// TNFS over a byte-stream transport (a WSS tunnel to a TCP server, later perhaps plain TCP).
// tnfsStreamTransport is the port the TNFS domain needs from such a transport; the
// infrastructure implements it. tnfsStreamLink adapts that stream to whole TNFS packets:
// it writes a request completely and cuts replies out of the byte stream with
// tnfsReplyAssembler, so transport segmentation never decides where a reply ends.

#include <cstdint>
#include <memory>

#include "global_types.h"
#include "tnfslib_framing.h"

// Contract every implementation honours:
// - No call blocks longer than the timeout it is given.
// - After read() or write() returns -1, is_connected() is false until connect() succeeds.
// - close() is idempotent and valid in any state, including before connect().
// - connect() is only called while !is_connected().
class tnfsStreamTransport
{
public:
    virtual ~tnfsStreamTransport() = default;

    virtual success_is_true connect(const char *host, uint16_t port, int timeout_ms) = 0;
    virtual bool is_connected() const = 0;

    // Bytes accepted (1..len); 0 if none could be sent before the timeout; -1 if lost.
    virtual int write(const uint8_t *buf, int len, int timeout_ms) = 0;

    // Bytes received (1..max); 0 if none arrived before the timeout; -1 if lost.
    virtual int read(uint8_t *buf, int max, int timeout_ms) = 0;

    virtual void close() = 0;
};

enum class tnfsStreamStatus
{
    REPLY,        // one whole reply of `length` bytes was copied out
    PENDING,      // no whole reply yet; poll again until the transaction times out
    DISCONNECTED, // the connection is gone; the next send reconnects
    MALFORMED,    // the stream lost alignment; the connection was closed to resynchronise
};

struct tnfsStreamReply
{
    tnfsStreamStatus status;
    int length; // reply length when REPLY, otherwise 0
};

// One TNFS mount's stream connection and its reply reassembly. Owns the transport;
// fixed memory (one tnfsReplyAssembler), no allocation, task or lock of its own.
class tnfsStreamLink
{
public:
    // Largest request: header plus payload, the size of tnfsPacket::rawData.
    static constexpr int MAX_REQUEST_SIZE = TNFS_HEADER_SIZE + TNFS_PAYLOAD_SIZE;

    explicit tnfsStreamLink(std::unique_ptr<tnfsStreamTransport> transport);
    ~tnfsStreamLink();

    tnfsStreamLink(const tnfsStreamLink &) = delete;
    tnfsStreamLink &operator=(const tnfsStreamLink &) = delete;

    // Connect first if needed (starting with an empty assembler), then write the whole
    // request. A failed or partial write closes the connection, since the server would
    // otherwise see a truncated request followed by the next one.
    success_is_true send(const char *host, uint16_t port, const uint8_t *request, int len,
                         int connect_timeout_ms, int write_timeout_ms);

    // Deliver one buffered reply if there is one; otherwise read at most once, waiting at
    // most `poll_timeout_ms`. `out` is also the read buffer, so no scratch memory is used.
    tnfsStreamReply recv(uint8_t (&out)[TNFS_MAX_REPLY_SIZE], int poll_timeout_ms);

    // Close the connection and drop buffered bytes. Idempotent.
    void close();

    bool is_connected() const;
    int buffered() const { return _rx.buffered(); }

private:
    tnfsStreamReply take_reply(uint8_t (&out)[TNFS_MAX_REPLY_SIZE]);

    std::unique_ptr<tnfsStreamTransport> _transport;
    tnfsReplyAssembler _rx;
};

#endif // _TNFSLIB_STREAM_H
