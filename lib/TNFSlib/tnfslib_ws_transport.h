#ifndef _TNFSLIB_WS_TRANSPORT_H
#define _TNFSLIB_WS_TRANSPORT_H

// TNFS stream transport over an RFC 6455 WebSocket, independent of the WebSocket library.
// tnfsWsIo is the port to that library (on ESP32: esp_transport_ws over esp_transport_ssl,
// in tnfslib_ws.cpp); tnfsWsTransport holds the policy: one binary frame per request,
// written all or nothing; replies read as a byte stream; PING answered, CLOSE echoed,
// anything else unexpected drops the connection. Pure code, so it is tested on PC.

#include <cstdint>
#include <memory>

#include "tnfslib_stream.h"
#include "tnfslib_ws_frames.h"

// The WebSocket library as tnfsWsTransport needs it, with esp_transport_ws semantics.
class tnfsWsIo
{
public:
    virtual ~tnfsWsIo() = default;

    // TLS (verified) plus the HTTP upgrade; true only for 101 with a valid accept key.
    // Starts from a clean state: no frame state or buffered bytes from an earlier
    // connection survive.
    virtual bool open(const char *host, uint16_t port, int timeout_ms) = 0;

    // <0 error, 0 nothing (or a frame header only), >0 payload bytes. Afterwards opcode(),
    // payload_len() and fin() describe the frame being read (opcode() is
    // tnfsWsFrameTracker::NO_FRAME when no frame header was seen).
    virtual int read(uint8_t *buf, int max, int timeout_ms) = 0;
    virtual int opcode() const = 0;
    virtual int payload_len() const = 0;
    virtual bool fin() const = 0;

    // Send one complete, masked frame with FIN set. The payload may be masked in place.
    // Returns the payload bytes written, or <= 0 on failure or timeout (the frame header
    // may already be on the wire then).
    virtual int send(int opcode, uint8_t *payload, int len, int timeout_ms) = 0;

    // Close the connection and free per-connection resources. Idempotent.
    virtual void shutdown() = 0;
};

class tnfsWsTransport : public tnfsStreamTransport
{
public:
    // A frame write spends its timeout up to three times on ESP-IDF (writability, frame
    // header, payload), so each step gets a third of the caller's budget.
    static constexpr int WRITE_STEPS = 3;
    // Answering a PING or a CLOSE is a few bytes: allow for a busy socket, but bound it
    // independently of the 20 ms read polls that carry it.
    static constexpr int CONTROL_WRITE_TIMEOUT_MS = 500;
    // Our own CLOSE on a clean shutdown is best effort and never delays it for long.
    static constexpr int CLOSE_WRITE_TIMEOUT_MS = 200;

    explicit tnfsWsTransport(std::unique_ptr<tnfsWsIo> io);
    ~tnfsWsTransport() override;

    tnfsWsTransport(const tnfsWsTransport &) = delete;
    tnfsWsTransport &operator=(const tnfsWsTransport &) = delete;

    success_is_true connect(const char *host, uint16_t port, int timeout_ms) override;
    bool is_connected() const override { return _connected; }
    int write(const uint8_t *buf, int len, int timeout_ms) override;
    int read(uint8_t *buf, int max, int timeout_ms) override;
    void close() override;

private:
    int drop(); // abort without a closing handshake and report -1
    int control(tnfsWsAction action, const uint8_t *buf, int n);

    std::unique_ptr<tnfsWsIo> _io;
    bool _connected = false;
    tnfsWsFrameTracker _frames;
    // Payload of the control frame being read; a PING is answered with all of it.
    uint8_t _ctrl[tnfsWsFrameTracker::MAX_CONTROL_PAYLOAD];
    int _ctrl_len = 0;
    // The library masks payloads in place, so requests are sent from this copy and the
    // caller's request (retried as is on failure) is never modified.
    uint8_t _tx[tnfsStreamLink::MAX_REQUEST_SIZE];
};

#endif // _TNFSLIB_WS_TRANSPORT_H
