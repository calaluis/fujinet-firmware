#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "harness_server.h"
#include "harness_ws.h"
#include "tnfslib_cert_time.h"
#include "tnfslib_stream_link.h"
#include "tnfslib_wss.h"
#include "tnfslib_wss_url.h"

// tnfslibMountInfo.cpp (reply buffer and transport memory) links against the stand-ins in
// tests/tnfs_harness/; the WebSocket stream itself only sees harness_ws::SimIo.
HarnessSystem fnSystem;
HarnessServer harness_server;

using namespace harness_ws;
using A = tnfsWsAction;

// ---------------------------------------------------------------------------------------
// tnfsWsFrameTracker on its own

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
// tnfsw:// host slots

namespace
{
tnfsWssUrlResult parse(const char *text, tnfsWssUrl &url) { return tnfs_parse_wss_url(text, url); }
} // namespace

TEST_CASE("tnfsw URL: host, default port and path")
{
    tnfsWssUrl u{};
    REQUIRE(parse("tnfsw://tnfs.example.org", u) == tnfsWssUrlResult::VALID);
    CHECK(std::string(u.host) == "tnfs.example.org");
    CHECK(u.port == TNFS_WSS_DEFAULT_PORT);
    CHECK(std::string(u.path) == "/");

    REQUIRE(parse("TNFSW://Example.org/", u) == tnfsWssUrlResult::VALID); // scheme case
    CHECK(std::string(u.host) == "Example.org");
    CHECK(std::string(u.path) == "/");
}

TEST_CASE("tnfsw URL: explicit port and WebSocket path")
{
    tnfsWssUrl u{};
    REQUIRE(parse("tnfsw://h.example:8443/tnfs", u) == tnfsWssUrlResult::VALID);
    CHECK(std::string(u.host) == "h.example");
    CHECK(u.port == 8443);
    CHECK(std::string(u.path) == "/tnfs");

    REQUIRE(parse("tnfsw://h:1", u) == tnfsWssUrlResult::VALID);
    CHECK(u.port == 1);
    REQUIRE(parse("tnfsw://h:65535/", u) == tnfsWssUrlResult::VALID);
    CHECK(u.port == 65535);
}

TEST_CASE("tnfsw URL: anything else is an ordinary TNFS host")
{
    tnfsWssUrl u{};
    for (const char *text : {"tnfs.example", "192.168.1.10", "tcp://h", "_tcp.h", "tnfs://h",
                             "tnfsw:/h", "tnfsw:h", "wss://h", ""})
    {
        CAPTURE(text);
        CHECK(parse(text, u) == tnfsWssUrlResult::NOT_WSS);
    }
    CHECK(tnfs_parse_wss_url(nullptr, u) == tnfsWssUrlResult::NOT_WSS);
}

TEST_CASE("tnfsw URL: unusable URLs are invalid, never another transport")
{
    tnfsWssUrl u;
    std::memset(&u, 0x55, sizeof(u));
    tnfsWssUrl untouched;
    std::memcpy(&untouched, &u, sizeof(u));
    for (const char *text :
         {"tnfsw://", "tnfsw:///path", "tnfsw://:443", "tnfsw://h:", "tnfsw://h:/x",
          "tnfsw://h:0", "tnfsw://h:65536", "tnfsw://h:99999999999", "tnfsw://h:-1",
          "tnfsw://h:44a", "tnfsw://h: 443", "tnfsw://user@h", "tnfsw://u:p@h",
          "tnfsw://[::1]", "tnfsw://[::1]:443", "tnfsw://h/p?q=1", "tnfsw://h/#frag",
          "tnfsw://h:443:444"})
    {
        CAPTURE(text);
        CHECK(parse(text, u) == tnfsWssUrlResult::INVALID);
        CHECK(std::memcmp(&u, &untouched, sizeof(u)) == 0); // only written when valid
    }
}

TEST_CASE("tnfsw URL: host and path limits")
{
    tnfsWssUrl u{};
    std::string host63(TNFS_WSS_HOST_SIZE - 1, 'a');
    CHECK(parse(("tnfsw://" + host63).c_str(), u) == tnfsWssUrlResult::VALID);
    CHECK(std::string(u.host) == host63);
    CHECK(parse(("tnfsw://" + host63 + "a").c_str(), u) == tnfsWssUrlResult::INVALID);

    std::string path31 = "/" + std::string(TNFS_WSS_PATH_SIZE - 2, 'p');
    CHECK(parse(("tnfsw://h" + path31).c_str(), u) == tnfsWssUrlResult::VALID);
    CHECK(std::string(u.path) == path31);
    CHECK(parse(("tnfsw://h" + path31 + "p").c_str(), u) == tnfsWssUrlResult::INVALID);

    // The longest URL a host slot holds (MAX_HOSTNAME_LEN 32, with the terminator).
    std::string slot = "tnfsw://" + std::string(31 - 8, 'b');
    REQUIRE(slot.size() == 31);
    CHECK(parse(slot.c_str(), u) == tnfsWssUrlResult::VALID);
    CHECK(std::strlen("tnfsw://tnfs.example.org") < 32);
}

// ---------------------------------------------------------------------------------------
// Certificate validity dates

namespace
{
constexpr time_t T_2026_10_14 = 1791936000; // 2026-10-14T00:00:00Z
const tnfsCertTime FROM = {2026, 9, 1, 0, 0, 0};
const tnfsCertTime TO = {2026, 11, 30, 23, 59, 59};
} // namespace

TEST_CASE("certificate dates: inside the validity window")
{
    CHECK(tnfs_cert_validity(FROM, TO, true, T_2026_10_14) == tnfsCertValidity::VALID);
    // Both bounds are inclusive.
    CHECK(tnfs_cert_validity(FROM, TO, true, 1788220800) == tnfsCertValidity::VALID); // 09-01
    CHECK(tnfs_cert_validity(FROM, TO, true, 1796083199) == tnfsCertValidity::VALID); // 11-30
}

TEST_CASE("certificate dates: expired and not yet valid")
{
    CHECK(tnfs_cert_validity(FROM, TO, true, 1796083200) == tnfsCertValidity::EXPIRED);
    CHECK(tnfs_cert_validity(FROM, TO, true, 1788220799) == tnfsCertValidity::NOT_YET_VALID);
    CHECK(tnfs_cert_validity({2020, 1, 1, 0, 0, 0}, {2025, 1, 1, 0, 0, 0}, true,
                             T_2026_10_14) == tnfsCertValidity::EXPIRED);
    CHECK(tnfs_cert_validity({2027, 1, 1, 0, 0, 0}, {2028, 1, 1, 0, 0, 0}, true,
                             T_2026_10_14) == tnfsCertValidity::NOT_YET_VALID);
}

TEST_CASE("certificate dates: no trusted clock, no certificate")
{
    // Not synced by SNTP, whatever the clock says.
    CHECK(tnfs_cert_validity(FROM, TO, false, T_2026_10_14) == tnfsCertValidity::NO_CLOCK);
    // Synced, but plainly wrong: the epoch, or before 2024.
    CHECK(tnfs_cert_validity({1970, 1, 1, 0, 0, 0}, {2100, 1, 1, 0, 0, 0}, true, 0) ==
          tnfsCertValidity::NO_CLOCK);
    CHECK(tnfs_cert_validity(FROM, TO, true, TNFS_CERT_EARLIEST_CLOCK - 1) ==
          tnfsCertValidity::NO_CLOCK);
    CHECK(tnfs_cert_validity({2023, 1, 1, 0, 0, 0}, {2030, 1, 1, 0, 0, 0}, true,
                             TNFS_CERT_EARLIEST_CLOCK) == tnfsCertValidity::VALID);
}

TEST_CASE("certificate dates: calendar arithmetic")
{
    // Leap day and year boundaries, against known epoch seconds.
    const tnfsCertTime leap = {2028, 2, 29, 12, 0, 0}; // 1835438400
    CHECK(tnfs_cert_validity(leap, {2028, 12, 31, 0, 0, 0}, true, 1835438400) ==
          tnfsCertValidity::VALID);
    CHECK(tnfs_cert_validity(leap, {2028, 12, 31, 0, 0, 0}, true, 1835438399) ==
          tnfsCertValidity::NOT_YET_VALID);
    CHECK(tnfs_cert_validity({2024, 12, 31, 23, 59, 59}, {2025, 1, 1, 0, 0, 0}, true,
                             1735689600) == tnfsCertValidity::VALID); // 2025-01-01T00:00:00Z
    CHECK(tnfs_cert_validity({2024, 12, 31, 23, 59, 59}, {2025, 1, 1, 0, 0, 0}, true,
                             1735689601) == tnfsCertValidity::EXPIRED);
}

// ---------------------------------------------------------------------------------------
// End to end on PC: tnfsStreamLink -> tnfsWsStream -> SimIo

namespace
{
// TNFS fixtures (session 0xBEEF).
Bytes read_reply(int count, uint8_t seq)
{
    Bytes r = {0xEF, 0xBE, seq, 0x21, 0x00, static_cast<uint8_t>(count & 0xFF),
               static_cast<uint8_t>(count >> 8)};
    for (int i = 0; i < count; i++)
        r.push_back(static_cast<uint8_t>(i * 13 + seq));
    return r;
}
const Bytes STAT_OK = {0x7d, 0xed, 0x01, 0x24, 0x00, 0xed, 0x41, 0xe9, 0x03,
                       0xe9, 0x03, 0x00, 0x10, 0x00, 0x00, 0x1f, 0x9a, 0x4f,
                       0x6a, 0xbe, 0x72, 0x66, 0x6a, 0xbe, 0x72, 0x66, 0x6a};
const Bytes REQUEST = {0xEF, 0xBE, 0x01, 0x24, '/', 0x00};

Bytes slice(const Bytes &b, size_t from, size_t to)
{ return Bytes(b.begin() + from, b.begin() + std::min(to, b.size())); }

Bytes cat_bytes(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// What a mount holds: the transport, the reply buffer and the reply format; the link is
// rebuilt per call, as _tnfs_tcp_link() does. These cases are about the WebSocket transport,
// so the mount has TNFSD's format, confirmed, and no fence goes out on a stream with no TNFS
// server behind it (the fence itself is covered in TNFSStreamLinkTests.cpp). The answer
// awaited is the one whose header the test expects: several requests may be in flight here.
struct WsRig
{
    Net net;
    WsLog log;
    tnfsWsStream stream{std::unique_ptr<tnfsWsIo>(new SimIo(net, log))};
    tnfsMountInfo mount{"h"};
    tnfsPacket last_request;
    uint8_t out[TNFS_MAX_REPLY_SIZE];

    WsRig()
    {
        mount.host_ip = 0;
        mount.port = 443;
        mount.timeout_ms = 5000;
        mount.tcp_reply_format = tnfsReplyFormat::fixed();
        mount.tcp_format_verified = 0x7;
    }

    static tnfsPacket packet(const Bytes &b)
    {
        tnfsPacket p;
        std::copy(b.begin(), b.end(), p.rawData);
        return p;
    }
    tnfsReplyAssemblerPtr &replies() { return mount.tcp_replies; }
    tnfsStreamLink link() { return tnfsStreamLink(stream, mount); }
    bool send(const Bytes &request = REQUEST)
    {
        last_request = packet(request);
        return link().send(last_request, request.size()).is_success();
    }
    tnfsStreamReply poll(const tnfsPacket &awaited, int polls = 5000) // one WS frame per poll at most
    {
        tnfsStreamReply r{tnfsStreamStatus::PENDING, 0};
        for (int i = 0; i < polls && r.status == tnfsStreamStatus::PENDING; i++)
            r = link().recv(out, awaited);
        return r;
    }
    tnfsStreamReply poll() { return poll(last_request); }
    void expect(const Bytes &want)
    {
        tnfsStreamReply r = poll(packet(Bytes(want.begin(), want.begin() + TNFS_HEADER_SIZE)));
        REQUIRE(r.status == tnfsStreamStatus::REPLY);
        CHECK(Bytes(out, out + r.length) == want);
    }
    size_t buffered() const { return mount.tcp_replies ? mount.tcp_replies->buffered() : 0; }
};
} // namespace

TEST_CASE("ws end to end: one reply per frame, one request per frame")
{
    WsRig r;
    REQUIRE(r.send());
    REQUIRE(r.send());
    CHECK(r.log.connects == 1);
    CHECK(r.log.frames_sent == 2);
    CHECK(r.log.requests[0] == REQUEST);
    r.net.push(frame(T::OP_BINARY, STAT_OK));
    r.net.push(frame(T::OP_BINARY, read_reply(525, 2)));
    r.expect(STAT_OK);
    r.expect(read_reply(525, 2));
    CHECK(r.buffered() == 0);
}

TEST_CASE("ws end to end: the connection is made by name; the address is not used")
{
    WsRig r;
    r.mount.host_ip = 0x12345678;
    REQUIRE(r.send());
    CHECK(r.mount.host_ip == 0x12345678);
    REQUIRE(r.log.open_timeouts.size() == 1);
    CHECK(r.log.open_timeouts[0] == 5000); // the mount's timeout reaches the connect
}

TEST_CASE("ws end to end: TNFS boundaries are independent of frame boundaries")
{
    Bytes stream;
    for (uint8_t s = 1; s <= 4; s++)
        stream = cat_bytes(stream, read_reply(100 * s, s));
    for (int frame_size : {1, 7, 64, 125, 126, 500, 532, 4096})
    {
        CAPTURE(frame_size);
        WsRig r;
        REQUIRE(r.send());
        for (size_t pos = 0; pos < stream.size(); pos += frame_size)
            r.net.push(frame(T::OP_BINARY, slice(stream, pos, pos + frame_size)));
        for (uint8_t s = 1; s <= 4; s++)
            r.expect(read_reply(100 * s, s));
        CHECK(r.buffered() == 0);
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
    CHECK(r.stream.is_open());
}

TEST_CASE("ws end to end: a payload split across TCP segments is reassembled")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes f = frame(T::OP_BINARY, STAT_OK);
    for (size_t i = 0; i < f.size(); i += 5)
        r.net.push(slice(f, i, i + 5));
    r.expect(STAT_OK);
}

TEST_CASE("ws end to end: a PING split across TCP segments is answered whole")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes ping = frame(T::OP_PING, Bytes{10, 20, 30, 40, 50, 60});
    r.net.push(slice(ping, 0, 4));
    r.net.push(slice(ping, 4, 6));
    r.net.push(cat_bytes(slice(ping, 6, ping.size()), frame(T::OP_BINARY, STAT_OK)));
    r.expect(STAT_OK);
    REQUIRE(r.log.pongs.size() == 1);
    CHECK(r.log.pongs[0] == (Bytes{10, 20, 30, 40, 50, 60}));
}

TEST_CASE("ws end to end: a frame holding more than the reply buffer's free space")
{
    // Two replies in one ~1 KB frame; the stream hands them over as the buffer frees up.
    WsRig r;
    REQUIRE(r.send());
    Bytes two = cat_bytes(read_reply(500, 1), read_reply(512, 2));
    REQUIRE(two.size() > TNFS_MAX_REPLY_SIZE);
    r.net.push(frame(T::OP_BINARY, two));
    r.expect(read_reply(500, 1));
    r.expect(read_reply(512, 2));
    CHECK(r.buffered() == 0);
    CHECK(r.stream.is_open());
}

TEST_CASE("ws end to end: a stall inside a payload drops the connection, never misframes")
{
    WsRig r;
    REQUIRE(r.send());
    Bytes f = frame(T::OP_BINARY, STAT_OK);
    r.net.push(slice(f, 0, 12)); // header + 10 payload bytes
    r.net.stall();               // ESP-IDF times out and forgets the frame
    r.net.push(slice(f, 12, f.size()));
    // Awaiting the STAT that is cut off (its own header): only the stall can end it.
    CHECK(r.poll(WsRig::packet(slice(STAT_OK, 0, TNFS_HEADER_SIZE))).status == tnfsStreamStatus::LOST);
    CHECK(r.replies() == nullptr);
    CHECK_FALSE(r.stream.is_open());
    CHECK(r.log.close_frames == 0); // mid-frame: aborted, no CLOSE

    // The retry reconnects; nothing of the old connection is read on the new one.
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
    CHECK(r.poll().status == tnfsStreamStatus::LOST);
}

TEST_CASE("ws end to end: server CLOSE ends the connection; nothing after it is read")
{
    WsRig r;
    REQUIRE(r.send());
    r.net.push(frame(T::OP_BINARY, slice(STAT_OK, 0, 10)));
    r.net.push(frame(T::OP_CLOSE, {0x03, 0xE8}));
    r.net.push(frame(T::OP_BINARY, slice(STAT_OK, 10, 27)));
    CHECK(r.poll().status == tnfsStreamStatus::LOST);
    CHECK(r.log.close_frames == 1); // our CLOSE answers the server's
    CHECK(r.log.shutdowns >= 1);
    CHECK(r.replies() == nullptr); // the 10-byte fragment is dropped with the buffer
}

TEST_CASE("ws end to end: text and reserved frames are refused")
{
    for (int op : {T::OP_TEXT, 0x3, 0xB})
    {
        CAPTURE(op);
        WsRig r;
        REQUIRE(r.send());
        r.net.push(frame(op, STAT_OK));
        CHECK(r.poll().status == tnfsStreamStatus::LOST);
    }
}

TEST_CASE("ws end to end: a malformed TNFS reply drops the WebSocket too")
{
    WsRig r;
    REQUIRE(r.send());
    r.net.push(frame(T::OP_BINARY, {0xEF, 0xBE, 0x01}));       // a header in two frames...
    r.net.push(frame(T::OP_BINARY, {0x21, 0x00, 0xFF, 0xFF})); // ...of a READ of 65535
    CHECK(r.poll().status == tnfsStreamStatus::LOST);
    CHECK_FALSE(r.stream.is_open());
    CHECK(r.log.close_frames == 1); // a clean WebSocket, so a clean close
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
            stream = cat_bytes(stream, rep);
        }
        Bytes wire;
        for (size_t pos = 0; pos < stream.size();)
        {
            int parts = 1 + rng() % 3;
            for (int p = 0; p < parts && pos < stream.size(); p++)
            {
                size_t n = 1 + rng() % 600;
                bool last = p == parts - 1 || pos + n >= stream.size();
                wire = cat_bytes(wire, frame(p == 0 ? T::OP_BINARY : T::OP_CONTINUATION,
                                             slice(stream, pos, pos + n), last));
                pos += n;
                if (rng() % 4 == 0)
                    wire = cat_bytes(wire, frame(T::OP_PING, Bytes(rng() % 126, 0x5A)));
                if (last)
                    break;
            }
        }
        WsRig r;
        REQUIRE(r.send());
        for (size_t pos = 0; pos < wire.size();)
        {
            size_t n = 1 + rng() % 900;
            r.net.push(slice(wire, pos, pos + n));
            pos += n;
        }
        for (const Bytes &want : replies)
            r.expect(want);
        CHECK(r.buffered() == 0);
        CHECK(r.stream.is_open());
    }
}

// ---------------------------------------------------------------------------------------
// tnfsWsStream policy: control-frame budgets, masking, writes, cleanup

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
    CHECK(r.log.send_timeouts == std::vector<int>{tnfsWsStream::CONTROL_WRITE_TIMEOUT_MS});
    CHECK(r.stream.is_open());
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
    CHECK(r.log.send_timeouts == std::vector<int>{tnfsWsStream::CONTROL_WRITE_TIMEOUT_MS});
}

TEST_CASE("PONG under TCP back-pressure or failing: dropped cleanly, no CLOSE mid-stream")
{
    for (int mode = 0; mode < 2; mode++)
    {
        CAPTURE(mode);
        WsRig r;
        REQUIRE(r.send());
        r.log.pong_backpressure = mode == 0;
        r.log.fail_pong = mode == 1;
        int shutdowns = r.log.shutdowns;
        r.net.push(frame(T::OP_PING, {1, 2}));
        r.net.push(frame(T::OP_BINARY, slice(STAT_OK, 0, 10)));
        CHECK(r.poll().status == tnfsStreamStatus::LOST);
        CHECK_FALSE(r.stream.is_open());
        CHECK(r.replies() == nullptr);
        CHECK(r.log.close_frames == 0);
        CHECK(r.log.shutdowns > shutdowns);

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
    CHECK_FALSE(r.send(request));
    CHECK(request == original);
    CHECK_FALSE(r.stream.is_open()); // a failed write drops the connection: all or nothing
    CHECK(r.log.close_frames == 0);  // the frame header may be on the wire

    r.log.fail_binary = false;
    REQUIRE(r.send(request));
    CHECK(request == original);
    REQUIRE(r.log.requests.size() == 1);
    CHECK(r.log.requests[0] == original);
    CHECK(r.log.connects == 2);
}

TEST_CASE("a request frame gets a third of the write budget per ESP-IDF step")
{
    WsRig r;
    REQUIRE(r.send());
    REQUIRE(r.log.send_timeouts.size() == 1);
    CHECK(r.log.send_timeouts[0] == tnfsWsStream::WRITE_TIMEOUT_MS / tnfsWsStream::WRITE_STEPS);
}

TEST_CASE("writes that cannot be one TNFS request frame are refused and drop the connection")
{
    WsRig r;
    REQUIRE(r.send());
    uint8_t big[TNFS_HEADER_SIZE + TNFS_PAYLOAD_SIZE + 1] = {};
    CHECK(r.stream.write(big, sizeof(big)) == 0);
    CHECK_FALSE(r.stream.is_open());

    REQUIRE(r.send());
    CHECK(r.stream.write(big, 0) == 0); // ESP-IDF would send an empty frame as a PING
    CHECK_FALSE(r.stream.is_open());
    CHECK(r.stream.write(big, 1) == 0); // not connected
    CHECK(r.log.frames_sent == 2);
}

TEST_CASE("the largest TNFS request fits one frame")
{
    WsRig r;
    Bytes request(TNFS_HEADER_SIZE + TNFS_PAYLOAD_SIZE, 0x11);
    REQUIRE(r.send(request));
    REQUIRE(r.log.requests.size() == 1);
    CHECK(r.log.requests[0] == request);
}

TEST_CASE("a failed open is shut down and never sends a frame")
{
    WsRig r;
    r.log.fail_open = true;
    CHECK_FALSE(r.send());
    CHECK(r.log.frames_sent == 0);
    CHECK(r.log.close_frames == 0);
    CHECK(r.log.shutdowns >= 1);
    CHECK_FALSE(r.stream.is_open());
    CHECK(r.replies() == nullptr);
}

TEST_CASE("an empty host name is refused without connecting")
{
    WsRig r;
    in_addr_t a = 0;
    CHECK(r.stream.open("", a, 443, 5000).is_error());
    CHECK(r.stream.open(nullptr, a, 443, 5000).is_error());
    CHECK(r.log.connects == 0);
}

TEST_CASE("nothing is read before connecting or after closing")
{
    WsRig r;
    r.net.push(frame(T::OP_BINARY, STAT_OK));
    CHECK(r.stream.available() == 0);
    REQUIRE(r.send());
    r.stream.close();
    CHECK(r.stream.available() == 0);
    uint8_t b[4];
    CHECK(r.stream.read(b, sizeof(b)) == 0);
}

TEST_CASE("bytes staged on one connection never reach the next one")
{
    WsRig r;
    REQUIRE(r.send());
    r.net.push(frame(T::OP_BINARY, read_reply(100, 1)));
    REQUIRE(r.stream.available() > 0); // staged, not read
    r.stream.close();
    r.net.runs.clear();
    REQUIRE(r.send());
    CHECK(r.stream.available() == 0);
    r.net.push(frame(T::OP_BINARY, STAT_OK));
    r.expect(STAT_OK);
}

TEST_CASE("a clean close sends one CLOSE; closing again or destroying sends none")
{
    Net net;
    WsLog log;
    {
        tnfsWsStream stream(std::unique_ptr<tnfsWsIo>(new SimIo(net, log)));
        in_addr_t a = 0;
        REQUIRE(stream.open("h", a, 443, 5000));
        stream.close();
        CHECK(log.close_frames == 1);
        stream.close();
        CHECK(log.close_frames == 1);
    }
    CHECK(log.close_frames == 1);
    CHECK(log.shutdowns >= 3);
}

TEST_CASE("destroying an open stream closes it cleanly")
{
    Net net;
    WsLog log;
    {
        tnfsWsStream stream(std::unique_ptr<tnfsWsIo>(new SimIo(net, log)));
        in_addr_t a = 0;
        REQUIRE(stream.open("h", a, 443, 5000));
    }
    CHECK(log.close_frames == 1);
    CHECK(log.shutdowns >= 1);
}

TEST_CASE("a transport in mount memory is destroyed and freed by its deleter")
{
    Net net;
    WsLog log;
    {
        void *memory = tnfs_stream_transport_memory(sizeof(tnfsWsStream));
        REQUIRE(memory != nullptr);
        tnfsStreamTransportPtr p(
            new (memory) tnfsWsStream(std::unique_ptr<tnfsWsIo>(new SimIo(net, log))));
        in_addr_t a = 0;
        REQUIRE(p->open("h", a, 443, 5000));
    }
    CHECK(log.close_frames == 1); // the virtual destructor ran
}

TEST_CASE("PC builds have no WSS transport")
{
    CHECK(tnfs_new_wss_stream("/") == nullptr);
}

TEST_CASE("allow_connect(false): open() fails at once, the library is never asked to connect")
{
    Net net;
    WsLog log;
    tnfsWsStream stream(std::unique_ptr<tnfsWsIo>(new SimIo(net, log)));
    in_addr_t a = 0;
    stream.allow_connect(false);
    CHECK(stream.open("h", a, 443, 5000).is_error());
    CHECK(log.connects == 0); // SimIo::open() stands for DNS + TCP + TLS + upgrade
    CHECK_FALSE(stream.is_open());
    stream.allow_connect(true);
    CHECK(stream.open("h", a, 443, 5000).is_success());
    CHECK(log.connects == 1);
}

TEST_CASE("connecting always gets the WSS connect budget, even with a shorter reply wait")
{
    Net net;
    WsLog log;
    tnfsWsStream stream(std::unique_ptr<tnfsWsIo>(new SimIo(net, log)));
    in_addr_t a = 0;
    REQUIRE(stream.open("h", a, 443, 2000));
    stream.close();
    REQUIRE(stream.open("h", a, 443, 8000));
    CHECK(log.open_timeouts ==
          std::vector<int>{tnfsWsStream::CONNECT_TIMEOUT_MS, 8000}); // a longer one is kept
}
