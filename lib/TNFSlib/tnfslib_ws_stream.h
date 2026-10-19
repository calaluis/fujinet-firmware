#ifndef _TNFSLIB_WS_STREAM_H
#define _TNFSLIB_WS_STREAM_H

// TNFS over an RFC 6455 WebSocket: tnfsStreamTransport for tnfsStreamLink, independent of the
// WebSocket library. tnfsWsIo is the port to that library (on ESP32: esp_transport_ws over
// esp_transport_ssl, in tnfslib_wss.cpp); tnfsWsStream holds the policy: one binary frame per
// request, written all or nothing; replies read as a byte stream; PING answered, CLOSE echoed,
// anything else unexpected drops the connection. Pure code, so it is tested on PC.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "tnfslib.h"
#include "tnfslib_framing.h"
#include "tnfslib_stream_transport.h"
#include "tnfslib_ws_frames.h"

// The WebSocket library as tnfsWsStream needs it, with esp_transport_ws semantics.
class tnfsWsIo
{
public:
    virtual ~tnfsWsIo() = default;

    // TLS (verified) plus the HTTP upgrade; true only for 101 Switching Protocols. Starts
    // from a clean state: no frame state or buffered bytes from an earlier connection survive.
    virtual success_is_true open(const char *host, uint16_t port, int timeout_ms) = 0;

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

class tnfsWsStream : public tnfsStreamTransport
{
public:
    // Longest available() waits for the next frame; the transaction loop keeps polling.
    static constexpr int POLL_TIMEOUT_MS = 20;
    // A TLS handshake from an ESP32 takes seconds: connecting always gets at least this long,
    // even when the transaction's timeout (its reply wait) is shorter.
    static constexpr int CONNECT_TIMEOUT_MS = 5000;
    // A request frame write; ESP-IDF spends its timeout up to three times (writability,
    // frame header, payload), so each step gets a third.
    static constexpr int WRITE_TIMEOUT_MS = 2000;
    static constexpr int WRITE_STEPS = 3;
    // Answering a PING or a CLOSE is a few bytes: allow for a busy socket, but bound it
    // independently of the read poll that carried it.
    static constexpr int CONTROL_WRITE_TIMEOUT_MS = 500;
    // Our own CLOSE on a clean shutdown is best effort and never delays it for long.
    static constexpr int CLOSE_WRITE_TIMEOUT_MS = 200;

    explicit tnfsWsStream(std::unique_ptr<tnfsWsIo> io);
    ~tnfsWsStream() override;

    tnfsWsStream(const tnfsWsStream &) = delete;
    tnfsWsStream &operator=(const tnfsWsStream &) = delete;

    // `address` is not used: a WSS connection is made by name, which TLS needs for SNI and
    // for checking the certificate.
    success_is_true open(const char *hostname, in_addr_t &address, uint16_t port,
                         int timeout_ms) override;
    bool is_open() override { return _connected; }
    // While false, open() fails at once: no DNS lookup, TCP connect or TLS handshake.
    void allow_connect(bool allowed) { _allow_connect = allowed; }
    // Reads at most one frame from the connection, waiting at most POLL_TIMEOUT_MS.
    size_t available() override;
    int read(uint8_t *buf, size_t size) override;
    size_t write(const uint8_t *buf, size_t size) override;
    void close() override;

private:
    int pull();
    int control(tnfsWsAction action, int n);
    void drop(); // abort without a closing handshake

    std::unique_ptr<tnfsWsIo> _io;
    bool _connected = false;
    bool _allow_connect = true;
    tnfsWsFrameTracker _frames;
    // Payload of the last data frame read, not yet taken by read().
    uint8_t _rx[TNFS_MAX_REPLY_SIZE];
    int _rx_pos = 0;
    int _rx_len = 0;
    // Payload of the control frame being read; a PING is answered with all of it.
    uint8_t _ctrl[tnfsWsFrameTracker::MAX_CONTROL_PAYLOAD];
    int _ctrl_len = 0;
    // The library masks payloads in place, so requests are sent from this copy and the
    // caller's request (retried as is on failure) is never modified.
    uint8_t _tx[TNFS_HEADER_SIZE + TNFS_PAYLOAD_SIZE];
};

#endif // _TNFSLIB_WS_STREAM_H
