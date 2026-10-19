#ifndef TNFS_HARNESS_WS_H
#define TNFS_HARNESS_WS_H

// tnfsWsIo over a simulation of ESP-IDF 5.4.0 esp_transport_ws with control frames
// propagated (transport_ws.c: ws_read, ws_read_header, ws_read_payload, _ws_write), for the
// pure tnfsWsStream policy on PC.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "tnfslib_ws_stream.h"

namespace harness_ws
{
using Bytes = std::vector<uint8_t>;
using T = tnfsWsFrameTracker;

// Server-to-client frame (unmasked).
inline Bytes frame(int opcode, const Bytes &payload, bool fin = true)
{
    Bytes f = {static_cast<uint8_t>((fin ? 0x80 : 0) | opcode)};
    if (payload.size() <= 125)
        f.push_back(static_cast<uint8_t>(payload.size()));
    else
    {
        f.push_back(126);
        f.push_back(static_cast<uint8_t>(payload.size() >> 8));
        f.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

struct Net
{
    // What reaches the socket: byte runs separated by stalls (a stall makes the next read
    // that needs more bytes time out).
    std::deque<Bytes> runs;
    void push(const Bytes &b) { runs.push_back(b); }
    void stall() { runs.push_back({}); }

    // Bytes available without waiting.
    bool ready() const { return !runs.empty() && !runs.front().empty(); }
    // Read up to `max` available bytes; 0 means the read timed out (a stall is consumed).
    int take(uint8_t *buf, int max)
    {
        if (runs.empty())
            return 0;
        if (runs.front().empty())
        {
            runs.pop_front();
            return 0;
        }
        Bytes &r = runs.front();
        int n = std::min<int>(max, static_cast<int>(r.size()));
        std::copy(r.begin(), r.begin() + n, buf);
        r.erase(r.begin(), r.begin() + n);
        if (r.empty())
            runs.pop_front();
        return n;
    }
    bool take_exact(uint8_t *buf, int n)
    {
        while (n > 0)
        {
            int k = take(buf, n);
            if (k == 0)
                return false;
            buf += k;
            n -= k;
        }
        return true;
    }
};

struct WsLog
{
    int connects = 0, shutdowns = 0, frames_sent = 0, close_frames = 0;
    std::vector<Bytes> requests; // unmasked payloads of BINARY frames
    std::vector<Bytes> pongs;    // unmasked payloads of PONG frames
    std::vector<int> send_timeouts;
    std::vector<int> open_timeouts;
    // Behaviour of the next call of each kind; true makes it fail.
    bool fail_open = false;
    bool fail_binary = false;
    bool pong_backpressure = false; // send returns 0: the socket never became writable
    bool fail_pong = false;         // send returns -1
    // Called with each request the server receives, so it can answer through Net.
    std::function<void(const Bytes &)> on_request;
};

class SimIo : public tnfsWsIo
{
public:
    SimIo(Net &net, WsLog &log) : _net(net), _log(log) {}

    success_is_true open(const char *, uint16_t, int timeout_ms) override
    {
        _log.connects++;
        _log.open_timeouts.push_back(timeout_ms);
        _net.runs.clear(); // a new connection: nothing of the old socket's bytes
        _bytes_remaining = 0;
        _header_received = false;
        return success_is_true(!_log.fail_open);
    }

    int read(uint8_t *buf, int len, int) override
    {
        if (_bytes_remaining <= 0)
        {
            _header_received = false;
            if (!_net.ready()) // esp_transport_poll_read timed out
            {
                _net.take(buf, 0); // consume a stall, if any
                return 0;
            }
            uint8_t h[8];
            if (!_net.take_exact(h, 2))
                return -1; // esp_transport_read_exact_size: a timeout there is an error
            _header_received = true;
            _fin = (h[0] & 0x80) != 0;
            _opcode = h[0] & 0x0F;
            int plen = h[1] & 0x7F;
            if (plen == 126)
            {
                if (!_net.take_exact(h, 2))
                    return -1;
                plen = h[0] << 8 | h[1];
            }
            _payload_len = plen;
            _bytes_remaining = plen;
            if (plen == 0)
                return 0;
        }
        int want = std::min(len, _bytes_remaining);
        int n = _net.take(buf, want);
        if (n <= 0)
        {
            _bytes_remaining = 0; // what ESP-IDF does on a payload timeout
            return n;
        }
        _bytes_remaining -= n;
        return n;
    }

    int opcode() const override { return _header_received ? _opcode : T::NO_FRAME; }
    int payload_len() const override { return _payload_len; }
    bool fin() const override { return _fin; }

    int send(int opcode, uint8_t *payload, int len, int timeout_ms) override
    {
        _log.send_timeouts.push_back(timeout_ms);
        Bytes plain(payload, payload + len);
        // Like _ws_write: the payload is masked in place while it is sent.
        for (int i = 0; i < len; i++)
            payload[i] ^= 0x5A;
        if (opcode == T::OP_BINARY)
        {
            if (_log.fail_binary)
                return -1;
            _log.frames_sent++;
            _log.requests.push_back(plain);
            if (_log.on_request)
                _log.on_request(plain);
            return len;
        }
        if (opcode == T::OP_PONG)
        {
            if (_log.pong_backpressure)
                return 0;
            if (_log.fail_pong)
                return -1;
            _log.pongs.push_back(plain);
            return len;
        }
        if (opcode == T::OP_CLOSE)
        {
            _log.close_frames++;
            return len;
        }
        return -1;
    }

    void shutdown() override { _log.shutdowns++; }

private:
    Net &_net;
    WsLog &_log;
    int _bytes_remaining = 0;
    bool _header_received = false;
    int _opcode = 0;
    int _payload_len = 0;
    bool _fin = false;
};
} // namespace harness_ws

#endif
