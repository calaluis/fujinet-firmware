#include "tnfslib_stream.h"

#include <utility>

tnfsStreamLink::tnfsStreamLink(std::unique_ptr<tnfsStreamTransport> transport)
    : _transport(std::move(transport))
{
}

tnfsStreamLink::~tnfsStreamLink() { close(); }

bool tnfsStreamLink::is_connected() const { return _transport && _transport->is_connected(); }

void tnfsStreamLink::close()
{
    if (_transport)
        _transport->close();
    _rx.reset();
}

success_is_true tnfsStreamLink::send(const char *host, uint16_t port, const uint8_t *request,
                                     int len, int connect_timeout_ms, int write_timeout_ms)
{
    if (!_transport || request == nullptr || len <= 0 || len > MAX_REQUEST_SIZE)
        RETURN_ERROR_AS_FALSE();

    if (!_transport->is_connected())
    {
        // Whatever is buffered belongs to the previous connection.
        close();
        if (!_transport->connect(host, port, connect_timeout_ms))
        {
            close();
            RETURN_ERROR_AS_FALSE();
        }
    }

    // Each call either makes progress or fails, so this loop runs at most `len` times.
    int sent = 0;
    while (sent < len)
    {
        int n = _transport->write(request + sent, len - sent, write_timeout_ms);
        if (n <= 0 || n > len - sent)
        {
            close();
            RETURN_ERROR_AS_FALSE();
        }
        sent += n;
    }
    RETURN_SUCCESS_AS_TRUE();
}

tnfsStreamReply tnfsStreamLink::take_reply(uint8_t (&out)[TNFS_MAX_REPLY_SIZE])
{
    tnfsFrame f = _rx.take(out);
    switch (f.status)
    {
    case tnfsFrameStatus::COMPLETE:
        return {tnfsStreamStatus::REPLY, f.length};
    case tnfsFrameStatus::MALFORMED:
        // No length field: once misaligned, the rest of this stream cannot be trusted.
        close();
        return {tnfsStreamStatus::MALFORMED, 0};
    case tnfsFrameStatus::NEED_MORE:
    default:
        return {tnfsStreamStatus::PENDING, 0};
    }
}

tnfsStreamReply tnfsStreamLink::recv(uint8_t (&out)[TNFS_MAX_REPLY_SIZE], int poll_timeout_ms)
{
    // A coalesced reply may already be complete; deliver it even if the peer has gone.
    tnfsStreamReply r = take_reply(out);
    if (r.status != tnfsStreamStatus::PENDING)
        return r;

    if (!is_connected())
    {
        close();
        return {tnfsStreamStatus::DISCONNECTED, 0};
    }

    int room = _rx.space() < TNFS_MAX_REPLY_SIZE ? _rx.space() : TNFS_MAX_REPLY_SIZE;
    if (room <= 0)
    {
        // Unreachable while the framer bounds replies; never spin on a full buffer.
        close();
        return {tnfsStreamStatus::MALFORMED, 0};
    }

    int n = _transport->read(out, room, poll_timeout_ms);
    if (n < 0 || n > room)
    {
        close();
        return {tnfsStreamStatus::DISCONNECTED, 0};
    }
    if (n == 0)
        return {tnfsStreamStatus::PENDING, 0};

    _rx.append(out, n);
    return take_reply(out);
}
