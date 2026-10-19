#include "tnfslib_ws_stream.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "../../include/debug.h"

tnfsWsStream::tnfsWsStream(std::unique_ptr<tnfsWsIo> io) : _io(std::move(io)) {}

tnfsWsStream::~tnfsWsStream() { close(); }

success_is_true tnfsWsStream::open(const char *hostname, in_addr_t &, uint16_t port,
                                   int timeout_ms)
{
    close();
    if (!_allow_connect)
    {
        Debug_println("TNFS WSS: not connecting (keep-alive)");
        RETURN_ERROR_AS_FALSE();
    }
    if (!_io || hostname == nullptr || hostname[0] == '\0')
        RETURN_ERROR_AS_FALSE();

    // TLS handshake, then the HTTP upgrade. Redirects are not followed: anything but
    // 101 Switching Protocols fails the connection.
    if (_io->open(hostname, port, std::max(timeout_ms, CONNECT_TIMEOUT_MS)).is_error())
    {
        Debug_printf("TNFS WSS: connect to %s:%u failed\r\n", hostname, (unsigned)port);
        _io->shutdown();
        RETURN_ERROR_AS_FALSE();
    }
    _connected = true;
    RETURN_SUCCESS_AS_TRUE();
}

size_t tnfsWsStream::available()
{
    if (_rx_pos < _rx_len)
        return _rx_len - _rx_pos;
    _rx_pos = _rx_len = 0;
    if (!_connected)
        return 0;
    int n = pull();
    return n > 0 ? n : 0;
}

int tnfsWsStream::read(uint8_t *buf, size_t size)
{
    int n = static_cast<int>(std::min(size, static_cast<size_t>(_rx_len - _rx_pos)));
    memcpy(buf, _rx + _rx_pos, n);
    _rx_pos += n;
    return n;
}

size_t tnfsWsStream::write(const uint8_t *buf, size_t size)
{
    if (!_connected || buf == nullptr || size == 0 || size > sizeof(_tx))
    {
        drop(); // a zero-length write would be sent as a PING by ESP-IDF
        return 0;
    }

    // One request, one binary frame. The frame header goes out before the payload, so a
    // short or timed-out payload write leaves a broken frame: all or nothing.
    memcpy(_tx, buf, size);
    int len = static_cast<int>(size);
    int n = _io->send(tnfsWsFrameTracker::OP_BINARY, _tx, len, WRITE_TIMEOUT_MS / WRITE_STEPS);
    if (n != len)
    {
        Debug_printf("TNFS WSS: write failed (%d of %d)\r\n", n, len);
        drop();
        return 0;
    }
    return size;
}

// One read from the connection: >0 payload bytes now in _rx, 0 nothing for the stream,
// -1 the connection was dropped.
int tnfsWsStream::pull()
{
    int n = _io->read(_rx, sizeof(_rx), POLL_TIMEOUT_MS);
    tnfsWsAction action = _frames.on_read(n, _io->opcode(), _io->payload_len(), _io->fin());
    if (action == tnfsWsAction::DATA)
    {
        _rx_len = n;
        return n;
    }
    if (action == tnfsWsAction::IDLE)
    {
        _ctrl_len = 0;
        return 0;
    }
    if (action == tnfsWsAction::LOST)
    {
        Debug_printf("TNFS WSS: connection lost (read %d, opcode %d)\r\n", n, _io->opcode());
        drop();
        return -1;
    }
    return control(action, n);
}

// `n` bytes of a control frame payload are in _rx.
int tnfsWsStream::control(tnfsWsAction action, int n)
{
    // The tracker bounds a control payload to 125 bytes, the size of _ctrl.
    if (n > static_cast<int>(sizeof(_ctrl)) - _ctrl_len)
    {
        drop();
        return -1;
    }
    memcpy(_ctrl + _ctrl_len, _rx, n);
    _ctrl_len += n;

    if (action == tnfsWsAction::CONTROL_PART)
        return 0;
    if (action == tnfsWsAction::PONG)
    {
        int len = _ctrl_len;
        _ctrl_len = 0;
        // Masks _ctrl in place, which is fine: it is consumed.
        if (_io->send(tnfsWsFrameTracker::OP_PONG, _ctrl, len, CONTROL_WRITE_TIMEOUT_MS) != len)
        {
            Debug_println("TNFS WSS: PONG could not be sent");
            drop();
            return -1;
        }
        return 0;
    }
    Debug_println("TNFS WSS: server closed the connection");
    close(); // still connected, so this answers with our own CLOSE frame
    return -1;
}

void tnfsWsStream::drop()
{
    // After an error the stream may sit mid-frame, where a CLOSE frame would be garbage.
    _connected = false;
    close();
}

void tnfsWsStream::close()
{
    if (_connected)
    {
        // Best-effort closing handshake: status 1000 (normal closure).
        uint8_t normal_closure[2] = {0x03, 0xE8};
        _io->send(tnfsWsFrameTracker::OP_CLOSE, normal_closure, sizeof(normal_closure),
                  CLOSE_WRITE_TIMEOUT_MS);
    }
    _connected = false;
    _frames.reset();
    _rx_pos = _rx_len = 0;
    _ctrl_len = 0;
    if (_io)
        _io->shutdown();
}
