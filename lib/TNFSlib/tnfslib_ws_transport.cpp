#include "tnfslib_ws_transport.h"

#include <cstring>
#include <utility>

#include "../../include/debug.h"

tnfsWsTransport::tnfsWsTransport(std::unique_ptr<tnfsWsIo> io) : _io(std::move(io)) {}

tnfsWsTransport::~tnfsWsTransport() { close(); }

success_is_true tnfsWsTransport::connect(const char *host, uint16_t port, int timeout_ms)
{
    close();
    if (!_io || host == nullptr || host[0] == '\0')
        RETURN_ERROR_AS_FALSE();

    // TLS handshake, then the HTTP upgrade. Redirects are not followed: anything but
    // 101 Switching Protocols with a valid accept key fails the connection.
    if (!_io->open(host, port, timeout_ms))
    {
        Debug_printf("TNFS WSS: connect to %s:%u failed\r\n", host, (unsigned)port);
        _io->shutdown();
        RETURN_ERROR_AS_FALSE();
    }

    _frames.reset();
    _ctrl_len = 0;
    _connected = true;
    RETURN_SUCCESS_AS_TRUE();
}

int tnfsWsTransport::drop()
{
    // After an error the stream may sit mid-frame, where a CLOSE frame would be garbage.
    _connected = false;
    close();
    return -1;
}

int tnfsWsTransport::write(const uint8_t *buf, int len, int timeout_ms)
{
    if (!_connected || buf == nullptr || len <= 0 || len > (int)sizeof(_tx))
        return drop(); // a zero-length write would be sent as a PING by ESP-IDF

    // One request, one binary frame. The frame header goes out before the payload, so a
    // short or timed-out payload write leaves a broken frame: all or nothing.
    memcpy(_tx, buf, len);
    int step_ms = timeout_ms / WRITE_STEPS > 0 ? timeout_ms / WRITE_STEPS : 1;
    int n = _io->send(tnfsWsFrameTracker::OP_BINARY, _tx, len, step_ms);
    if (n != len)
    {
        Debug_printf("TNFS WSS: write failed (%d of %d)\r\n", n, len);
        return drop();
    }
    return len;
}

int tnfsWsTransport::read(uint8_t *buf, int max, int timeout_ms)
{
    if (!_connected || buf == nullptr || max <= 0)
        return drop();

    int n = _io->read(buf, max, timeout_ms);
    tnfsWsAction action = _frames.on_read(n, _io->opcode(), _io->payload_len(), _io->fin());
    switch (action)
    {
    case tnfsWsAction::DATA:
        return n;
    case tnfsWsAction::IDLE:
        _ctrl_len = 0;
        return 0;
    case tnfsWsAction::LOST:
        Debug_printf("TNFS WSS: connection lost (read %d, opcode %d)\r\n", n, _io->opcode());
        return drop();
    default:
        return control(action, buf, n);
    }
}

int tnfsWsTransport::control(tnfsWsAction action, const uint8_t *buf, int n)
{
    // The tracker bounds a control payload to 125 bytes, the size of _ctrl.
    if (n > (int)sizeof(_ctrl) - _ctrl_len)
        return drop();
    memcpy(_ctrl + _ctrl_len, buf, n);
    _ctrl_len += n;

    switch (action)
    {
    case tnfsWsAction::CONTROL_PART:
        return 0;
    case tnfsWsAction::PONG: {
        int len = _ctrl_len;
        _ctrl_len = 0;
        // Masks _ctrl in place, which is fine: it is consumed.
        int sent =
            _io->send(tnfsWsFrameTracker::OP_PONG, _ctrl, len, CONTROL_WRITE_TIMEOUT_MS);
        if (sent != len)
        {
            Debug_println("TNFS WSS: PONG could not be sent");
            return drop();
        }
        return 0;
    }
    case tnfsWsAction::CLOSE:
    default:
        Debug_println("TNFS WSS: server closed the connection");
        close(); // still connected, so this answers with our own CLOSE frame
        return -1;
    }
}

void tnfsWsTransport::close()
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
    _ctrl_len = 0;
    if (_io)
        _io->shutdown();
}
