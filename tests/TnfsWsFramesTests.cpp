#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <deque>
#include <random>
#include <vector>

#include "tnfslib_stream.h"
#include "tnfslib_ws_frames.h"
#include "tnfslib_ws_transport.h"

using Bytes = std::vector<uint8_t>;
using T = tnfsWsFrameTracker;
using A = tnfsWsAction;

// ---------------------------------------------------------------------------------------
// tnfsWsFrameTracker on its own

TEST_CASE("frame tracker: timeouts and bytes outside any frame")
{
    T t;
    CHECK(t.on_read(0, T::NO_FRAME, 0, false) == A::IDLE);
    CHECK(t.on_read(3, T::NO_FRAME, 0, false) == A::LOST);
    CHECK(t.on_read(-1, T::NO_FRAME, 0, false) == A::LOST);
}

TEST_CASE("frame tracker: a binary frame read whole or in pieces")
{
    T t;
    CHECK(t.on_read(10, T::OP_BINARY, 10, true) == A::DATA);
    CHECK(t.frame_left() == 0);

    CHECK(t.on_read(4, T::OP_BINARY, 10, true) == A::DATA);
    CHECK(t.frame_left() == 6);
    CHECK(t.on_read(5, T::OP_BINARY, 10, true) == A::DATA);
    CHECK(t.on_read(1, T::OP_BINARY, 10, true) == A::DATA);
    CHECK(t.frame_left() == 0);
    CHECK(t.on_read(0, T::NO_FRAME, 0, false) == A::IDLE);
}

TEST_CASE("frame tracker: a timeout inside a payload means the transport lost the frame")
{
    T t;
    REQUIRE(t.on_read(4, T::OP_BINARY, 10, true) == A::DATA);
    CHECK(t.on_read(0, T::OP_BINARY, 10, true) == A::LOST);
    CHECK(t.frame_left() == 0); // reset, ready for a new connection

    REQUIRE(t.on_read(4, T::OP_BINARY, 10, true) == A::DATA);
    CHECK(t.on_read(7, T::OP_BINARY, 10, true) == A::LOST); // more than the frame holds

    // The header arrived but the payload read timed out at once.
    CHECK(t.on_read(0, T::OP_BINARY, 10, true) == A::LOST);
    // A count larger than the frame.
    CHECK(t.on_read(11, T::OP_BINARY, 10, true) == A::LOST);
    CHECK(t.on_read(1, T::OP_BINARY, -1, true) == A::LOST);
}

TEST_CASE("frame tracker: fragmented binary messages")
{
    T t;
    CHECK(t.on_read(3, T::OP_BINARY, 3, false) == A::DATA);
    CHECK(t.message_open());
    CHECK(t.on_read(2, T::OP_CONTINUATION, 2, false) == A::DATA);
    CHECK(t.on_read(0, T::OP_CONTINUATION, 0, true) == A::IDLE); // empty final fragment
    CHECK_FALSE(t.message_open());

    CHECK(t.on_read(1, T::OP_CONTINUATION, 1, true) == A::LOST); // nothing to continue
    CHECK(t.on_read(1, T::OP_BINARY, 1, false) == A::DATA);
    CHECK(t.on_read(1, T::OP_BINARY, 1, true) == A::LOST); // new message mid-message
}

TEST_CASE("frame tracker: control frames")
{
    T t;
    CHECK(t.on_read(0, T::OP_PING, 0, true) == A::PONG);
    CHECK(t.on_read(5, T::OP_PING, 5, true) == A::PONG);
    CHECK(t.on_read(6, T::OP_PING, 5, true) == A::LOST);     // more than the payload
    CHECK(t.on_read(0, T::OP_PING, 5, true) == A::LOST);     // payload read timed out
    CHECK(t.on_read(5, T::OP_PING, 5, false) == A::LOST);    // fragmented control frame
    CHECK(t.on_read(126, T::OP_PING, 126, true) == A::LOST); // over 125 bytes
    CHECK(t.on_read(125, T::OP_PING, 125, true) == A::PONG);
    CHECK(t.on_read(3, T::OP_PONG, 3, true) == A::IDLE); // unsolicited PONG
    CHECK(t.on_read(2, T::OP_CLOSE, 2, true) == A::CLOSE);
    CHECK(t.on_read(0, T::OP_CLOSE, 0, true) == A::CLOSE);
}

TEST_CASE("frame tracker: a control payload may arrive over several reads")
{
    T t;
    CHECK(t.on_read(2, T::OP_PING, 9, true) == A::CONTROL_PART);
    CHECK(t.frame_left() == 7);
    CHECK(t.on_read(4, T::OP_PING, 9, true) == A::CONTROL_PART);
    CHECK(t.on_read(3, T::OP_PING, 9, true) == A::PONG);
    CHECK(t.frame_left() == 0);

    CHECK(t.on_read(1, T::OP_CLOSE, 2, true) == A::CONTROL_PART);
    CHECK(t.on_read(1, T::OP_CLOSE, 2, true) == A::CLOSE);

    CHECK(t.on_read(1, T::OP_PONG, 3, true) == A::CONTROL_PART);
    CHECK(t.on_read(2, T::OP_PONG, 3, true) == A::IDLE);

    // A timeout in the middle means the transport dropped the frame.
    CHECK(t.on_read(1, T::OP_PING, 3, true) == A::CONTROL_PART);
    CHECK(t.on_read(0, T::OP_PING, 3, true) == A::LOST);
}

TEST_CASE("frame tracker: control frames between fragments keep the message open")
{
    T t;
    REQUIRE(t.on_read(3, T::OP_BINARY, 3, false) == A::DATA);
    CHECK(t.on_read(1, T::OP_PING, 1, true) == A::PONG);
    CHECK(t.message_open());
    CHECK(t.on_read(2, T::OP_CONTINUATION, 2, true) == A::DATA);
    CHECK_FALSE(t.message_open());
}

TEST_CASE("frame tracker: text and reserved opcodes are refused")
{
    for (int op : {T::OP_TEXT, 0x3, 0x4, 0x5, 0x6, 0x7, 0xB, 0xC, 0xD, 0xE, 0xF})
    {
        CAPTURE(op);
        T t;
        CHECK(t.on_read(1, op, 1, true) == A::LOST);
    }
}

TEST_CASE("frame tracker: an error resets every state")
{
    T t;
    REQUIRE(t.on_read(3, T::OP_BINARY, 10, false) == A::DATA);
    CHECK(t.on_read(-1, T::OP_BINARY, 10, false) == A::LOST);
    CHECK(t.frame_left() == 0);
    CHECK_FALSE(t.message_open());
    CHECK(t.on_read(1, T::OP_BINARY, 1, true) == A::DATA);
}

// ---------------------------------------------------------------------------------------
// End to end on PC: tnfsStreamLink -> tnfsWsTransport (the real adapter) -> SimIo, a
// tnfsWsIo simulating ESP-IDF 5.4.0 esp_transport_ws with control frames propagated
// (transport_ws.c: ws_read, ws_read_header, ws_read_payload, _ws_write).

// Server-to-client frame (unmasked).
static Bytes frame(int opcode, const Bytes &payload, bool fin = true)
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
    // What reaches the socket: byte runs separated by stalls (a stall makes the next
    // read that needs more bytes time out).
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
        int n = std::min<int>(max, r.size());
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
    // Behaviour of the next send() of each kind; true makes it fail.
    bool fail_open = false;
    bool fail_binary = false;
    bool pong_backpressure = false; // send returns 0: the socket never became writable
    bool fail_pong = false;         // send returns -1
};

// tnfsWsIo over a simulation of ESP-IDF 5.4.0 esp_transport_ws with control frames
// propagated (transport_ws.c: ws_read, ws_read_header, ws_read_payload, _ws_write).
class SimIo : public tnfsWsIo
{
public:
    SimIo(Net &net, WsLog &log) : _net(net), _log(log) {}

    bool open(const char *, uint16_t, int) override
    {
        _log.connects++;
        _bytes_remaining = 0;
        _header_received = false;
        return !_log.fail_open;
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
        switch (opcode)
        {
        case T::OP_BINARY:
            if (_log.fail_binary)
                return -1;
            _log.frames_sent++;
            _log.requests.push_back(plain);
            return len;
        case T::OP_PONG:
            if (_log.pong_backpressure)
                return 0;
            if (_log.fail_pong)
                return -1;
            _log.pongs.push_back(plain);
            return len;
        case T::OP_CLOSE:
            _log.close_frames++;
            return len;
        default:
            return -1;
        }
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

// TNFS fixtures (session 0xBEEF).
static Bytes read_reply(int count, uint8_t seq)
{
    Bytes r = {0xEF,
               0xBE,
               seq,
               0x21,
               0x00,
               static_cast<uint8_t>(count & 0xFF),
               static_cast<uint8_t>(count >> 8)};
    for (int i = 0; i < count; i++)
        r.push_back(static_cast<uint8_t>(i * 13 + seq));
    return r;
}
static const Bytes STAT_OK = {0x7d, 0xed, 0x01, 0x24, 0x00, 0xed, 0x41, 0xe9, 0x03,
                              0xe9, 0x03, 0x00, 0x10, 0x00, 0x00, 0x1f, 0x9a, 0x4f,
                              0x6a, 0xbe, 0x72, 0x66, 0x6a, 0xbe, 0x72, 0x66, 0x6a};
static const Bytes REQUEST = {0xEF, 0xBE, 0x01, 0x24, '/', 0x00};

static Bytes slice(const Bytes &b, size_t from, size_t to)
{ return Bytes(b.begin() + from, b.begin() + std::min(to, b.size())); }

static Bytes cat_bytes(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

struct WsRig
{
    Net net;
    WsLog log;
    tnfsStreamLink link{std::unique_ptr<tnfsStreamTransport>(
        new tnfsWsTransport(std::unique_ptr<tnfsWsIo>(new SimIo(net, log))))};
    uint8_t out[TNFS_MAX_REPLY_SIZE];

    success_is_true send()
    {
        return link.send("h", 443, REQUEST.data(), static_cast<int>(REQUEST.size()), 5000,
                         2000);
    }
    tnfsStreamReply poll(int polls = 5000) // one WS frame per poll at most
    {
        tnfsStreamReply r{tnfsStreamStatus::PENDING, 0};
        for (int i = 0; i < polls && r.status == tnfsStreamStatus::PENDING; i++)
            r = link.recv(out, 20);
        return r;
    }
    void expect(const Bytes &want)
    {
        tnfsStreamReply r = poll();
        REQUIRE(r.status == tnfsStreamStatus::REPLY);
        CHECK(Bytes(out, out + r.length) == want);
    }
};

TEST_CASE("ws end to end: one reply per frame, one request per frame")
{
    WsRig r;
    REQUIRE(r.send());
    REQUIRE(r.send());
    CHECK(r.log.frames_sent == 2);
    r.net.push(frame(T::OP_BINARY, STAT_OK));
    r.net.push(frame(T::OP_BINARY, read_reply(525, 2)));
    r.expect(STAT_OK);
    r.expect(read_reply(525, 2));
    CHECK(r.link.buffered() == 0);
}

TEST_CASE("ws end to end: TNFS boundaries are independent of frame boundaries")
{
    Bytes stream;
    for (uint8_t s = 1; s <= 4; s++)
    {
        Bytes rep = read_reply(100 * s, s);
        stream.insert(stream.end(), rep.begin(), rep.end());
    }
    for (int frame_size : {1, 7, 64, 125, 126, 500, 4096})
    {
        CAPTURE(frame_size);
        WsRig r;
        REQUIRE(r.send());
        for (size_t pos = 0; pos < stream.size(); pos += frame_size)
            r.net.push(frame(T::OP_BINARY, slice(stream, pos, pos + frame_size)));
        for (uint8_t s = 1; s <= 4; s++)
            r.expect(read_reply(100 * s, s));
    }
}

TEST_CASE("ws end to end: fragmented messages and pings between frames")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes rep = read_reply(300, 9);
    r.net.push(frame(T::OP_BINARY, slice(rep, 0, 100), false));
    r.net.push(frame(T::OP_PING, {1, 2, 3}));
    r.net.push(frame(T::OP_CONTINUATION, slice(rep, 100, 250), false));
    r.net.push(frame(T::OP_PING, {}));
    r.net.push(frame(T::OP_PONG, {7}));
    r.net.push(frame(T::OP_CONTINUATION, slice(rep, 250, 400), true));
    r.expect(rep);
    REQUIRE(r.log.pongs.size() == 2);
    CHECK(r.log.pongs[0] == Bytes{1, 2, 3});
    CHECK(r.log.pongs[1].empty());
    CHECK(r.link.is_connected());
}

TEST_CASE("ws end to end: a payload split across TCP segments is reassembled")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes f = frame(T::OP_BINARY, STAT_OK);
    // Arrives in pieces, with no stall: each read takes what is there.
    for (size_t i = 0; i < f.size(); i += 5)
        r.net.push(slice(f, i, i + 5));
    r.expect(STAT_OK);
}

TEST_CASE("ws end to end: a PING split across TCP segments is answered whole")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes ping = frame(T::OP_PING, Bytes{10, 20, 30, 40, 50, 60});
    Bytes data = frame(T::OP_BINARY, STAT_OK);
    r.net.push(slice(ping, 0, 4));
    r.net.push(slice(ping, 4, 6));
    r.net.push(cat_bytes(slice(ping, 6, ping.size()), data));
    r.expect(STAT_OK);
    REQUIRE(r.log.pongs.size() == 1);
    CHECK(r.log.pongs[0] == (Bytes{10, 20, 30, 40, 50, 60}));
}

TEST_CASE("ws end to end: a stall inside a payload drops the connection, never misframes")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes f = frame(T::OP_BINARY, STAT_OK);
    r.net.push(slice(f, 0, 12)); // header + 10 payload bytes
    r.net.stall();               // ESP-IDF times out and forgets the frame
    r.net.push(slice(f, 12, f.size()));
    CHECK(r.poll().status == tnfsStreamStatus::DISCONNECTED);
    CHECK(r.link.buffered() == 0);
    CHECK_FALSE(r.link.is_connected());

    // The retry reconnects; the server's answer arrives on the new connection.
    r.net.runs.clear();
    REQUIRE(r.send());
    CHECK(r.log.connects == 2);
    r.net.push(frame(T::OP_BINARY, STAT_OK));
    r.expect(STAT_OK);
}

TEST_CASE("ws end to end: a stall inside a frame header is a lost connection")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes f = frame(T::OP_BINARY, read_reply(200, 1)); // 126: 4-byte header
    r.net.push(slice(f, 0, 2));
    r.net.stall();
    r.net.push(slice(f, 2, f.size()));
    CHECK(r.poll().status == tnfsStreamStatus::DISCONNECTED);
}

TEST_CASE("ws end to end: server CLOSE ends the connection; nothing after it is read")
{
    WsRig r;
    REQUIRE(r.send());
    r.net.push(frame(T::OP_BINARY, slice(STAT_OK, 0, 10)));
    r.net.push(frame(T::OP_CLOSE, {0x03, 0xE8}));
    r.net.push(frame(T::OP_BINARY, slice(STAT_OK, 10, 27)));
    CHECK(r.poll().status == tnfsStreamStatus::DISCONNECTED);
    CHECK(r.log.close_frames == 1); // our CLOSE answers the server's
    CHECK(r.log.shutdowns >= 1);
    CHECK(r.link.buffered() == 0); // the 10-byte fragment is dropped
}

TEST_CASE("ws end to end: text frames are refused")
{
    WsRig r;
    REQUIRE(r.send());
    r.net.push(frame(T::OP_TEXT, STAT_OK));
    CHECK(r.poll().status == tnfsStreamStatus::DISCONNECTED);
}

TEST_CASE("ws end to end: random frames, fragments, pings and segmentation")
{
    std::mt19937 rng(0x5753);
    for (int round = 0; round < 200; round++)
    {
        CAPTURE(round);
        std::vector<Bytes> replies;
        Bytes stream;
        for (int i = 0; i < 5; i++)
        {
            Bytes rep = rng() % 3 ? read_reply(rng() % 526, static_cast<uint8_t>(i)) : STAT_OK;
            replies.push_back(rep);
            stream.insert(stream.end(), rep.begin(), rep.end());
        }
        // Cut the stream into messages of 1..3 frames, with pings between frames.
        Bytes wire;
        for (size_t pos = 0; pos < stream.size();)
        {
            int parts = 1 + rng() % 3;
            for (int p = 0; p < parts && pos < stream.size(); p++)
            {
                size_t n = 1 + rng() % 600;
                bool last = p == parts - 1 || pos + n >= stream.size();
                Bytes f = frame(p == 0 ? T::OP_BINARY : T::OP_CONTINUATION,
                                slice(stream, pos, pos + n), last);
                wire.insert(wire.end(), f.begin(), f.end());
                pos += n;
                if (rng() % 4 == 0)
                {
                    Bytes ping = frame(T::OP_PING, Bytes(rng() % 126, 0x5A));
                    wire.insert(wire.end(), ping.begin(), ping.end());
                }
                if (last)
                    break;
            }
        }
        WsRig r;
        REQUIRE(r.send());
        // Deliver in random TCP segments, no stalls.
        for (size_t pos = 0; pos < wire.size();)
        {
            size_t n = 1 + rng() % 900;
            r.net.push(slice(wire, pos, pos + n));
            pos += n;
        }
        for (const Bytes &want : replies)
            r.expect(want);
        CHECK(r.link.buffered() == 0);
        CHECK(r.link.is_connected());
    }
}

// ---------------------------------------------------------------------------------------
// tnfsWsTransport itself (P4.1): control-frame write budget, masking, cleanup

TEST_CASE("PONG gets its own write budget, not the 20 ms read poll that carried the PING")
{
    WsRig r;
    REQUIRE(r.send());
    r.log.send_timeouts.clear();
    r.net.push(frame(T::OP_PING, {7, 8, 9}));
    r.net.push(frame(T::OP_BINARY, STAT_OK));
    r.expect(STAT_OK);
    REQUIRE(r.log.pongs.size() == 1);
    CHECK(r.log.pongs[0] == (Bytes{7, 8, 9}));
    REQUIRE(r.log.send_timeouts.size() == 1);
    CHECK(r.log.send_timeouts[0] == tnfsWsTransport::CONTROL_WRITE_TIMEOUT_MS);
    CHECK(r.link.is_connected());
}

TEST_CASE("a fragmented PING is answered once, whole, within the control budget")
{
    WsRig r;
    REQUIRE(r.send());
    r.log.send_timeouts.clear();
    Bytes payload(125, 0x33);
    Bytes ping = frame(T::OP_PING, payload);
    r.net.push(slice(ping, 0, 3));
    r.net.push(slice(ping, 3, 60));
    r.net.push(cat_bytes(slice(ping, 60, ping.size()), frame(T::OP_BINARY, STAT_OK)));
    r.expect(STAT_OK);
    REQUIRE(r.log.pongs.size() == 1);
    CHECK(r.log.pongs[0] == payload);
    CHECK(r.log.send_timeouts == std::vector<int>{tnfsWsTransport::CONTROL_WRITE_TIMEOUT_MS});
}

TEST_CASE("PONG under TCP back-pressure or failing: dropped cleanly, no CLOSE mid-stream")
{
    for (int mode = 0; mode < 2; mode++)
    {
        CAPTURE(mode);
        WsRig r;
        REQUIRE(r.send());
        r.log.pong_backpressure = mode == 0; // send returns 0
        r.log.fail_pong = mode == 1;         // send returns -1
        int shutdowns = r.log.shutdowns;
        r.net.push(frame(T::OP_PING, {1, 2}));
        r.net.push(frame(T::OP_BINARY, slice(STAT_OK, 0, 10)));
        CHECK(r.poll().status == tnfsStreamStatus::DISCONNECTED);
        CHECK_FALSE(r.link.is_connected());
        CHECK(r.link.buffered() == 0);
        CHECK(r.log.close_frames == 0);     // aborted, not a closing handshake
        CHECK(r.log.shutdowns > shutdowns); // shut down (idempotently, by adapter and link)

        // The next request reconnects and works.
        r.log.pong_backpressure = r.log.fail_pong = false;
        r.net.runs.clear();
        REQUIRE(r.send());
        CHECK(r.log.connects == 2);
        r.net.push(frame(T::OP_BINARY, STAT_OK));
        r.expect(STAT_OK);
    }
}

TEST_CASE("masking never touches the caller's request, also across a failed send and retry")
{
    WsRig r;
    const Bytes original = REQUEST;
    Bytes request = REQUEST;
    r.log.fail_binary = true;
    CHECK_FALSE(
        r.link.send("h", 443, request.data(), static_cast<int>(request.size()), 5000, 2000));
    CHECK(request == original);
    CHECK_FALSE(r.link.is_connected());

    r.log.fail_binary = false;
    REQUIRE(
        r.link.send("h", 443, request.data(), static_cast<int>(request.size()), 5000, 2000));
    CHECK(request == original);
    REQUIRE(r.log.requests.size() == 1);
    CHECK(r.log.requests[0] == original); // the frame carried the unmasked bytes
    CHECK(r.log.connects == 2);
}

TEST_CASE("a request frame gets a third of the write budget per ESP-IDF step")
{
    WsRig r;
    REQUIRE(r.send());
    REQUIRE(r.log.send_timeouts.size() == 1);
    CHECK(r.log.send_timeouts[0] == 2000 / tnfsWsTransport::WRITE_STEPS);
}

TEST_CASE("a failed open is shut down and never sends a frame")
{
    WsRig r;
    r.log.fail_open = true;
    CHECK_FALSE(r.send());
    CHECK(r.log.frames_sent == 0);
    CHECK(r.log.close_frames == 0);
    CHECK(r.log.shutdowns >= 1);
    CHECK_FALSE(r.link.is_connected());
}

TEST_CASE("a clean close sends one CLOSE; closing again or destroying sends none")
{
    Net net;
    WsLog log;
    {
        tnfsStreamLink link(std::unique_ptr<tnfsStreamTransport>(
            new tnfsWsTransport(std::unique_ptr<tnfsWsIo>(new SimIo(net, log)))));
        REQUIRE(
            link.send("h", 443, REQUEST.data(), static_cast<int>(REQUEST.size()), 5000, 2000));
        link.close();
        CHECK(log.close_frames == 1);
        link.close();
        CHECK(log.close_frames == 1);
    }
    CHECK(log.close_frames == 1);
    CHECK(log.shutdowns >= 3); // explicit closes plus destruction, all idempotent
}

TEST_CASE("destroying an open link closes it cleanly")
{
    Net net;
    WsLog log;
    {
        tnfsStreamLink link(std::unique_ptr<tnfsStreamTransport>(
            new tnfsWsTransport(std::unique_ptr<tnfsWsIo>(new SimIo(net, log)))));
        REQUIRE(
            link.send("h", 443, REQUEST.data(), static_cast<int>(REQUEST.size()), 5000, 2000));
    }
    CHECK(log.close_frames == 1);
    CHECK(log.shutdowns >= 1);
}
