#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <deque>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "tnfslib_stream.h"

using Bytes = std::vector<uint8_t>;

// Counts heap allocations while `g_count_allocs` is set, to show send/recv allocate nothing.
static bool g_count_allocs = false;
static int g_allocs = 0;

void *operator new(std::size_t size)
{
    if (g_count_allocs)
        g_allocs++;
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

// ---------------------------------------------------------------------------------------
// Fake transport. Its state lives outside it, so tests can inspect it after the link has
// taken ownership and even after the link has destroyed it.

enum class Ev
{
    DATA,    // read() returns these bytes, at most `max` of them at a time
    TIMEOUT, // read() returns 0
    LOST,    // read() returns -1 and the connection drops
    OVERRUN, // read() claims more bytes than it was allowed (contract violation)
};

struct ReadEvent
{
    Ev kind;
    Bytes bytes;
};

// Fake monotonic clock for the write deadline.
static uint32_t g_now = 0;
static uint32_t fake_millis() { return g_now; }

struct FakeState
{
    std::deque<bool> connect_results; // empty: connect succeeds
    std::deque<ReadEvent> script;     // empty: read() times out
    int write_chunk = INT_MAX;        // most bytes accepted per write()
    int write_lost_after = -1;        // drop the connection once this many bytes were written
    bool write_stalls = false;        // write() makes no progress
    uint32_t write_ms = 0;            // time each write() takes on the fake clock

    bool connected = false;
    int connects = 0, closes = 0, destroyed = 0, reads = 0, writes = 0;
    std::string host;
    uint16_t port = 0;
    int connect_timeout = -1, read_timeout = -1, write_timeout = -1;
    Bytes written;
};

class FakeStreamTransport : public tnfsStreamTransport
{
public:
    explicit FakeStreamTransport(FakeState &s) : _s(s) {}
    ~FakeStreamTransport() override { _s.destroyed++; }

    success_is_true connect(const char *host, uint16_t port, int timeout_ms) override
    {
        REQUIRE_FALSE(_s.connected); // contract: only called while disconnected
        _s.connects++;
        _s.host = host;
        _s.port = port;
        _s.connect_timeout = timeout_ms;
        bool ok = true;
        if (!_s.connect_results.empty())
        {
            ok = _s.connect_results.front();
            _s.connect_results.pop_front();
        }
        _s.connected = ok;
        return success_is_true(ok);
    }

    bool is_connected() const override { return _s.connected; }

    int write(const uint8_t *buf, int len, int timeout_ms) override
    {
        _s.writes++;
        _s.write_timeout = timeout_ms;
        g_now += _s.write_ms;
        if (!_s.connected)
            return -1;
        if (_s.write_stalls)
            return 0;
        int n = std::min(len, _s.write_chunk);
        if (_s.write_lost_after >= 0)
        {
            int left = _s.write_lost_after - static_cast<int>(_s.written.size());
            if (left <= 0)
            {
                _s.connected = false;
                return -1;
            }
            n = std::min(n, left);
        }
        _s.written.insert(_s.written.end(), buf, buf + n);
        return n;
    }

    int read(uint8_t *buf, int max, int timeout_ms) override
    {
        _s.reads++;
        _s.read_timeout = timeout_ms;
        REQUIRE(max > 0);
        if (!_s.connected)
            return -1;
        if (_s.script.empty())
            return 0;
        ReadEvent &ev = _s.script.front();
        switch (ev.kind)
        {
        case Ev::TIMEOUT:
            _s.script.pop_front();
            return 0;
        case Ev::LOST:
            _s.script.pop_front();
            _s.connected = false;
            return -1;
        case Ev::OVERRUN:
            _s.script.pop_front();
            return max + 1;
        case Ev::DATA:
        default:
            break;
        }
        int n = std::min<int>(max, ev.bytes.size());
        std::copy(ev.bytes.begin(), ev.bytes.begin() + n, buf);
        ev.bytes.erase(ev.bytes.begin(), ev.bytes.begin() + n);
        if (ev.bytes.empty())
            _s.script.pop_front();
        return n;
    }

    void close() override
    {
        _s.closes++;
        _s.connected = false;
    }

private:
    FakeState &_s;
};

// ---------------------------------------------------------------------------------------
// Fixtures

// Reply header (session 0xBEEF, little endian) + status + data.
static Bytes reply(uint8_t command, uint8_t status, const Bytes &data = {}, uint8_t seq = 1)
{
    Bytes r = {0xEF, 0xBE, seq, command, status};
    r.insert(r.end(), data.begin(), data.end());
    return r;
}

static Bytes read_reply(int count, uint8_t seq = 1)
{
    Bytes data = {static_cast<uint8_t>(count & 0xFF), static_cast<uint8_t>(count >> 8)};
    for (int i = 0; i < count; i++)
        data.push_back(static_cast<uint8_t>(i * 7 + seq));
    return reply(TNFS_CMD_READ, TNFS_RESULT_SUCCESS, data, seq);
}

// Live Q4 capture through the Cloudflare tunnel: MOUNT, STAT and UMOUNT replies.
static const Bytes MOUNT_OK = {0x7d, 0xed, 0x00, 0x00, 0x00, 0x03, 0x01, 0xe8, 0x03};
static const Bytes STAT_OK = {0x7d, 0xed, 0x01, 0x24, 0x00, 0xed, 0x41, 0xe9, 0x03,
                              0xe9, 0x03, 0x00, 0x10, 0x00, 0x00, 0x1f, 0x9a, 0x4f,
                              0x6a, 0xbe, 0x72, 0x66, 0x6a, 0xbe, 0x72, 0x66, 0x6a};
static const Bytes UMOUNT_OK = {0x7d, 0xed, 0x02, 0x01, 0x00};
// EAGAIN carries a two-byte back-off; a MOUNT error carries the server version.
static const Bytes BACKOFF_REPLY = {0xEF, 0xBE, 0x03, 0x21, TNFS_RESULT_TRY_AGAIN, 0xE8, 0x03};
static const Bytes MOUNT_ERR = {0x00, 0x00, 0x00, 0x00, TNFS_RESULT_FILE_NOT_FOUND,
                                0x02, 0x01};

static const Bytes REQUEST = {0x00, 0x00, 0x00, 0x00, 0x02, 0x01, '/', 0x00, 0x00, 0x00};

static Bytes cat(std::initializer_list<Bytes> parts)
{
    Bytes all;
    for (const auto &p : parts)
        all.insert(all.end(), p.begin(), p.end());
    return all;
}

static void script_chunks(FakeState &s, const Bytes &stream, int chunk)
{
    for (size_t pos = 0; pos < stream.size(); pos += chunk)
    {
        size_t end = std::min(stream.size(), pos + chunk);
        s.script.push_back({Ev::DATA, Bytes(stream.begin() + pos, stream.begin() + end)});
    }
}

struct Rig
{
    FakeState s;
    tnfsStreamLink link{std::unique_ptr<tnfsStreamTransport>(new FakeStreamTransport(s))};
    uint8_t out[TNFS_MAX_REPLY_SIZE];

    success_is_true send(const Bytes &req = REQUEST)
    {
        return link.send("tnfs.example", 443, req.data(), static_cast<int>(req.size()), 6000,
                         2000);
    }

    // Poll like the transaction loop does, at most `polls` times.
    tnfsStreamReply poll(int polls = 1000)
    {
        tnfsStreamReply r{tnfsStreamStatus::PENDING, 0};
        for (int i = 0; i < polls && r.status == tnfsStreamStatus::PENDING; i++)
            r = link.recv(out, 20);
        return r;
    }

    Bytes last(const tnfsStreamReply &r) const { return Bytes(out, out + r.length); }

    void expect_reply(const Bytes &want, int polls = 1000)
    {
        tnfsStreamReply r = poll(polls);
        REQUIRE(r.status == tnfsStreamStatus::REPLY);
        CHECK(last(r) == want);
    }
};

// ---------------------------------------------------------------------------------------
// Connect and write

TEST_CASE("connect success: the first send connects, then writes the whole request")
{
    Rig r;
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.send());
    CHECK(r.s.connects == 1);
    CHECK(r.s.host == "tnfs.example");
    CHECK(r.s.port == 443);
    CHECK(r.s.connect_timeout == 6000);
    CHECK(r.s.write_timeout == 2000);
    CHECK(r.s.written == REQUEST);
    CHECK(r.link.is_connected());

    // Still connected: the next request reuses the connection.
    CHECK(r.send());
    CHECK(r.s.connects == 1);
    CHECK(r.s.written == cat({REQUEST, REQUEST}));
}

TEST_CASE("connect failure: nothing is written, the link stays down, the next send retries")
{
    Rig r;
    r.s.connect_results = {false, true};
    CHECK_FALSE(r.send());
    CHECK(r.s.connects == 1);
    CHECK(r.s.writes == 0);
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);
    CHECK(r.s.reads == 0);

    CHECK(r.send());
    CHECK(r.s.connects == 2);
    CHECK(r.s.written == REQUEST);
}

TEST_CASE("invalid requests are refused without touching the transport")
{
    Rig r;
    Bytes too_big(tnfsStreamLink::MAX_REQUEST_SIZE + 1, 0);
    Bytes biggest(tnfsStreamLink::MAX_REQUEST_SIZE, 0);
    CHECK_FALSE(r.link.send("h", 1, nullptr, 5, 10, 10));
    CHECK_FALSE(r.link.send("h", 1, REQUEST.data(), 0, 10, 10));
    CHECK_FALSE(r.link.send("h", 1, REQUEST.data(), -1, 10, 10));
    CHECK_FALSE(r.send(too_big));
    CHECK(r.s.connects == 0);
    CHECK(r.s.writes == 0);
    CHECK(r.send(biggest));
    CHECK(r.s.written.size() == biggest.size());
}

TEST_CASE("partial writes are completed in order")
{
    for (int chunk : {1, 2, 3, 7, 64})
    {
        CAPTURE(chunk);
        Rig r;
        r.s.write_chunk = chunk;
        Bytes req = read_reply(300); // any 307 bytes
        CHECK(r.send(req));
        CHECK(r.s.written == req);
        CHECK(r.s.writes == (static_cast<int>(req.size()) + chunk - 1) / chunk);
        CHECK(r.link.is_connected());
    }
}

TEST_CASE("a write that times out without progress fails the send and closes")
{
    Rig r;
    r.s.write_stalls = true;
    CHECK_FALSE(r.send());
    CHECK(r.s.writes == 1); // no retry loop on a stalled write
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.s.closes >= 1);
}

TEST_CASE("connection lost mid-write: the send fails and the next one reconnects")
{
    Rig r;
    r.s.write_chunk = 4;
    r.s.write_lost_after = 6; // the server sees a truncated request
    CHECK_FALSE(r.send());
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.s.written == Bytes(REQUEST.begin(), REQUEST.begin() + 6));

    r.s.write_lost_after = -1;
    r.s.written.clear();
    CHECK(r.send());
    CHECK(r.s.connects == 2);
    CHECK(r.s.written == REQUEST);
}

// ---------------------------------------------------------------------------------------
// Reading replies

TEST_CASE("partial reads: a reply arriving one byte at a time is delivered once whole")
{
    Rig r;
    REQUIRE(r.send());
    script_chunks(r.s, STAT_OK, 1);
    for (size_t i = 0; i + 1 < STAT_OK.size(); i++)
        REQUIRE(r.link.recv(r.out, 20).status == tnfsStreamStatus::PENDING);
    tnfsStreamReply rep = r.link.recv(r.out, 20);
    REQUIRE(rep.status == tnfsStreamStatus::REPLY);
    CHECK(r.last(rep) == STAT_OK);
    CHECK(r.link.buffered() == 0);
    CHECK(r.s.reads == static_cast<int>(STAT_OK.size()));
}

TEST_CASE("fragmented replies of every shape, at every chunk size")
{
    const std::vector<Bytes> replies = {MOUNT_OK,      STAT_OK,        UMOUNT_OK,
                                        BACKOFF_REPLY, MOUNT_ERR,      read_reply(0),
                                        read_reply(1), read_reply(525)};
    for (const Bytes &want : replies)
        for (int chunk = 1; chunk <= 64; chunk++)
        {
            CAPTURE(chunk);
            Rig r;
            REQUIRE(r.send());
            script_chunks(r.s, want, chunk);
            r.expect_reply(want);
            CHECK(r.link.buffered() == 0);
        }
}

TEST_CASE("coalesced replies come out one per recv without another read")
{
    Rig r;
    REQUIRE(r.send());
    Bytes stream = cat({MOUNT_OK, STAT_OK, UMOUNT_OK});
    r.s.script.push_back({Ev::DATA, stream});
    r.expect_reply(MOUNT_OK);
    CHECK(r.s.reads == 1);
    r.expect_reply(STAT_OK);
    r.expect_reply(UMOUNT_OK);
    CHECK(r.s.reads == 1);
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::PENDING);
    CHECK(r.s.reads == 2);
}

TEST_CASE("coalesced and fragmented: a reply plus the head of the next")
{
    Rig r;
    REQUIRE(r.send());
    Bytes big = read_reply(525, 2);
    Bytes stream = cat({read_reply(400, 1), big});
    // Two maximum reads: the first carries the second reply's head.
    script_chunks(r.s, stream, 450);
    r.expect_reply(read_reply(400, 1));
    r.expect_reply(big);
    CHECK(r.link.buffered() == 0);
}

TEST_CASE("a late duplicate is passed through; the transaction's sequence check drops it")
{
    Rig r;
    REQUIRE(r.send());
    r.s.script.push_back(
        {Ev::DATA, cat({read_reply(10, 5), read_reply(10, 5), read_reply(10, 6)})});
    r.expect_reply(read_reply(10, 5));
    r.expect_reply(read_reply(10, 5));
    r.expect_reply(read_reply(10, 6));
}

TEST_CASE("random chunking, timeouts included, preserves every reply in order")
{
    std::mt19937 rng(0x7F53);
    for (int round = 0; round < 300; round++)
    {
        CAPTURE(round);
        std::vector<Bytes> sent;
        Bytes stream;
        for (int i = 0; i < 6; i++)
        {
            Bytes rep;
            switch (rng() % 4)
            {
            case 0:
                rep = read_reply(rng() % 526, static_cast<uint8_t>(i));
                break;
            case 1:
                rep = STAT_OK;
                break;
            case 2:
                rep = BACKOFF_REPLY;
                break;
            default:
                rep = MOUNT_OK;
                break;
            }
            sent.push_back(rep);
            stream.insert(stream.end(), rep.begin(), rep.end());
        }
        Rig r;
        REQUIRE(r.send());
        for (size_t pos = 0; pos < stream.size();)
        {
            if (rng() % 4 == 0)
                r.s.script.push_back({Ev::TIMEOUT, {}});
            size_t n = std::min<size_t>(1 + rng() % 700, stream.size() - pos);
            r.s.script.push_back(
                {Ev::DATA, Bytes(stream.begin() + pos, stream.begin() + pos + n)});
            pos += n;
        }
        for (const Bytes &want : sent)
            r.expect_reply(want);
        CHECK(r.link.buffered() == 0);
        CHECK(r.s.script.empty());
    }
}

// ---------------------------------------------------------------------------------------
// Disconnects, reconnects and timeouts

TEST_CASE("connection lost during a reply: DISCONNECTED, and the fragment is dropped")
{
    Rig r;
    REQUIRE(r.send());
    r.s.script.push_back({Ev::DATA, Bytes(STAT_OK.begin(), STAT_OK.begin() + 10)});
    r.s.script.push_back({Ev::LOST, {}});
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::PENDING);
    CHECK(r.link.buffered() == 10);
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.link.buffered() == 0);
    // Polling on does not touch the dead transport.
    int reads = r.s.reads;
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);
    CHECK(r.s.reads == reads);
}

TEST_CASE("reconnection starts with an empty assembler, so replies stay aligned")
{
    Rig r;
    REQUIRE(r.send());
    // The first connection delivers half a reply, then the peer goes away without the
    // link noticing in recv (it is detected by the next send instead).
    r.s.script.push_back({Ev::DATA, Bytes(STAT_OK.begin(), STAT_OK.begin() + 12)});
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::PENDING);
    CHECK(r.link.buffered() == 12);
    r.s.connected = false;

    // The retry reconnects; the stale 12 bytes must not prefix the new stream.
    REQUIRE(r.send());
    CHECK(r.s.connects == 2);
    CHECK(r.link.buffered() == 0);
    r.s.script.push_back({Ev::DATA, STAT_OK});
    r.expect_reply(STAT_OK);
}

TEST_CASE("a whole buffered reply is still delivered after the connection drops")
{
    Rig r;
    REQUIRE(r.send());
    r.s.script.push_back({Ev::DATA, cat({MOUNT_OK, UMOUNT_OK})});
    r.expect_reply(MOUNT_OK);
    r.s.connected = false;
    r.expect_reply(UMOUNT_OK);
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);
}

TEST_CASE("timeouts: one bounded read per recv; the caller's budget ends the wait")
{
    Rig r;
    REQUIRE(r.send());
    tnfsStreamReply rep = r.link.recv(r.out, 37);
    CHECK(rep.status == tnfsStreamStatus::PENDING);
    CHECK(rep.length == 0);
    CHECK(r.s.read_timeout == 37);
    CHECK(r.s.reads == 1);

    // A transaction that polls 100 times gives up with the link still usable.
    rep = r.poll(100);
    CHECK(rep.status == tnfsStreamStatus::PENDING);
    CHECK(r.s.reads == 101);
    CHECK(r.link.is_connected());

    // A late reply after a timed-out request is still framed correctly.
    r.s.script.push_back({Ev::DATA, MOUNT_OK});
    r.expect_reply(MOUNT_OK);
}

TEST_CASE("an EAGAIN without its back-off stalls until the caller times out and closes")
{
    // TNFSD's OS-error path can send EAGAIN as 5 bytes (P2 limitation 1). The framer waits
    // for the 2 back-off bytes, so the transaction times out; closing then realigns.
    Rig r;
    REQUIRE(r.send());
    r.s.script.push_back({Ev::DATA, {0xEF, 0xBE, 0x03, TNFS_CMD_READ, TNFS_RESULT_TRY_AGAIN}});
    CHECK(r.poll(100).status == tnfsStreamStatus::PENDING);
    CHECK(r.link.buffered() == 5);

    r.link.close();
    REQUIRE(r.send());
    CHECK(r.s.connects == 2);
    r.s.script.push_back({Ev::DATA, STAT_OK});
    r.expect_reply(STAT_OK);
}

TEST_CASE("close: repeated, before connect and after a loss")
{
    Rig r;
    r.link.close();
    r.link.close();
    CHECK_FALSE(r.link.is_connected());
    REQUIRE(r.send());
    r.s.script.push_back({Ev::DATA, Bytes(MOUNT_OK.begin(), MOUNT_OK.begin() + 4)});
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::PENDING);
    r.link.close();
    CHECK(r.link.buffered() == 0);
    CHECK_FALSE(r.link.is_connected());
    r.link.close();
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);
    int closes = r.s.closes;
    r.link.close();
    CHECK(r.s.closes == closes + 1); // forwarded every time; the transport makes it a no-op
    CHECK(r.send());                 // and the link is usable again
    CHECK(r.s.connects == 2);
}

// ---------------------------------------------------------------------------------------
// Invalid input

TEST_CASE("malformed replies close the stream; the next send resynchronises")
{
    for (const Bytes &bad : {reply(0x99, 0x00),                         // unknown command
                             reply(TNFS_CMD_STAT, 0x55),                // unknown status
                             reply(TNFS_CMD_READ, 0x00, {0x0E, 0x02})}) // count 526 > 525
    {
        Rig r;
        REQUIRE(r.send());
        r.s.script.push_back({Ev::DATA, cat({bad, MOUNT_OK})});
        tnfsStreamReply rep = r.link.recv(r.out, 20);
        CHECK(rep.status == tnfsStreamStatus::MALFORMED);
        CHECK(rep.length == 0);
        CHECK_FALSE(r.link.is_connected());
        CHECK(r.link.buffered() == 0);
        CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);

        REQUIRE(r.send());
        CHECK(r.s.connects == 2);
        r.s.script.push_back({Ev::DATA, MOUNT_OK});
        r.expect_reply(MOUNT_OK);
    }
}

TEST_CASE("a valid reply ahead of garbage is delivered before the stream is dropped")
{
    Rig r;
    REQUIRE(r.send());
    r.s.script.push_back({Ev::DATA, cat({UMOUNT_OK, reply(0x99, 0x00)})});
    r.expect_reply(UMOUNT_OK);
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::MALFORMED);
    CHECK_FALSE(r.link.is_connected());
}

TEST_CASE("a transport reporting more bytes than allowed is treated as lost")
{
    Rig r;
    REQUIRE(r.send());
    r.s.script.push_back({Ev::OVERRUN, {}});
    CHECK(r.link.recv(r.out, 20).status == tnfsStreamStatus::DISCONNECTED);
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.link.buffered() == 0);
}

// ---------------------------------------------------------------------------------------
// Resources

TEST_CASE("destroying the link closes and destroys its transport exactly once")
{
    FakeState s;
    {
        tnfsStreamLink link(std::unique_ptr<tnfsStreamTransport>(new FakeStreamTransport(s)));
        REQUIRE(link.send("h", 1, REQUEST.data(), static_cast<int>(REQUEST.size()), 10, 10));
        CHECK(s.connected);
        CHECK(s.destroyed == 0);
    }
    CHECK_FALSE(s.connected);
    CHECK(s.closes >= 1);
    CHECK(s.destroyed == 1);
}

TEST_CASE("a link without a transport fails cleanly")
{
    tnfsStreamLink link(nullptr);
    uint8_t out[TNFS_MAX_REPLY_SIZE];
    CHECK_FALSE(link.is_connected());
    CHECK_FALSE(link.send("h", 1, REQUEST.data(), static_cast<int>(REQUEST.size()), 10, 10));
    CHECK(link.recv(out, 20).status == tnfsStreamStatus::DISCONNECTED);
    link.close();
    link.close();
}

TEST_CASE("send and recv allocate nothing and buffering stays within the assembler")
{
    Rig r;
    r.s.write_chunk = 5;
    r.s.written.reserve(4096);
    Bytes stream = cat({read_reply(525, 1), read_reply(525, 2), STAT_OK});
    script_chunks(r.s, stream, 333);
    int replies = 0;
    int max_buffered = 0;

    // The counter really sees allocations.
    g_allocs = 0;
    g_count_allocs = true;
    Bytes probe(16);
    g_count_allocs = false;
    REQUIRE(g_allocs == 1);

    g_allocs = 0;
    g_count_allocs = true;
    bool sent = r.send();
    for (int i = 0; i < 50 && replies < 3; i++)
    {
        tnfsStreamReply rep = r.link.recv(r.out, 20);
        if (rep.status == tnfsStreamStatus::REPLY)
            replies++;
        max_buffered = std::max(max_buffered, r.link.buffered());
    }
    g_count_allocs = false;

    CHECK(sent);
    CHECK(replies == 3);
    CHECK(g_allocs == 0);
    CHECK(max_buffered <= tnfsReplyAssembler::CAPACITY);
    CHECK(sizeof(tnfsStreamLink) <= sizeof(tnfsReplyAssembler) + 3 * sizeof(void *));
}

// ---------------------------------------------------------------------------------------
// Write deadline and timeout policy (P4)

struct ClockRig
{
    FakeState s;
    tnfsStreamLink link{std::unique_ptr<tnfsStreamTransport>(new FakeStreamTransport(s)),
                        fake_millis};
    uint8_t out[TNFS_MAX_REPLY_SIZE];

    success_is_true send(const Bytes &req, int write_timeout_ms)
    {
        return link.send("h", 443, req.data(), static_cast<int>(req.size()), 6000,
                         write_timeout_ms);
    }
};

TEST_CASE("write deadline: partial writes share one budget")
{
    for (uint32_t origin : {0u, 1000u, 0xFFFFFFF0u}) // the last one wraps around
    {
        CAPTURE(origin);
        g_now = origin;
        ClockRig r;
        r.s.write_chunk = 100;
        r.s.write_ms = 300;
        Bytes req(450, 0x11); // five writes of 300 ms each
        CHECK(r.send(req, 2000));
        CHECK(r.s.writes == 5);
        CHECK(r.s.write_timeout == 2000 - 4 * 300); // each call gets what is left

        // The same request against a 1,000 ms budget: the fourth call is never made.
        g_now = origin;
        ClockRig late;
        late.s.write_chunk = 100;
        late.s.write_ms = 300;
        CHECK_FALSE(late.send(req, 1000));
        CHECK(late.s.writes == 4);
        CHECK(late.s.write_timeout == 100);
        CHECK_FALSE(late.link.is_connected()); // a truncated request is never left behind
        CHECK(late.s.written.size() == 400);
    }
}

TEST_CASE("write deadline: a budget exhausted exactly fails before writing again")
{
    g_now = 0;
    ClockRig r;
    r.s.write_chunk = 1;
    r.s.write_ms = 10;
    CHECK_FALSE(r.send(Bytes(5, 0), 30));
    CHECK(r.s.writes == 3);
    CHECK(r.s.written.size() == 3);
}

TEST_CASE("write deadline: without a clock each write gets the full timeout")
{
    Rig r;
    r.s.write_chunk = 2;
    r.s.write_ms = 5000;
    CHECK(r.send());
    CHECK(r.s.write_timeout == 2000);
    CHECK(r.s.written == REQUEST);
}

TEST_CASE("expire: drops a partial reply with its connection, keeps an idle one")
{
    g_now = 0;
    ClockRig r;
    REQUIRE(r.send(REQUEST, 2000));
    int closes = r.s.closes; // send() closes once before connecting
    r.link.expire();         // nothing buffered: keep the connection
    CHECK(r.link.is_connected());
    CHECK(r.s.closes == closes);

    r.s.script.push_back({Ev::DATA, Bytes(STAT_OK.begin(), STAT_OK.begin() + 9)});
    REQUIRE(r.link.recv(r.out, 20).status == tnfsStreamStatus::PENDING);
    r.link.expire();
    CHECK_FALSE(r.link.is_connected());
    CHECK(r.link.buffered() == 0);

    // The retry reconnects; the stale 9 bytes cannot prefix the new reply.
    REQUIRE(r.send(REQUEST, 2000));
    CHECK(r.s.connects == 2);
    r.s.script.push_back({Ev::DATA, STAT_OK});
    tnfsStreamReply rep = r.link.recv(r.out, 20);
    REQUIRE(rep.status == tnfsStreamStatus::REPLY);
    CHECK(Bytes(r.out, r.out + rep.length) == STAT_OK);
}

TEST_CASE("exhausted: only when disconnected with nothing left to deliver")
{
    Rig r;
    CHECK(r.link.exhausted());
    REQUIRE(r.send());
    CHECK_FALSE(r.link.exhausted());
    r.s.script.push_back({Ev::DATA, cat({MOUNT_OK, UMOUNT_OK})});
    r.expect_reply(MOUNT_OK);
    r.s.connected = false; // dropped, but one reply is still buffered
    CHECK_FALSE(r.link.exhausted());
    r.expect_reply(UMOUNT_OK);
    CHECK(r.link.exhausted());
}

TEST_CASE("allow_connect(false): only an open connection is used")
{
    Rig r;
    r.link.allow_connect(false);
    CHECK_FALSE(r.link.can_send());
    CHECK_FALSE(r.send()); // would need a new connection
    CHECK(r.s.connects == 0);
    CHECK(r.s.writes == 0);

    r.link.allow_connect(true);
    CHECK(r.link.can_send());
    REQUIRE(r.send());
    r.link.allow_connect(false);
    CHECK(r.link.can_send());
    CHECK(r.send()); // the open connection is reused
    CHECK(r.s.connects == 1);

    r.s.connected = false; // dropped: no reconnect while disallowed
    CHECK_FALSE(r.link.can_send());
    CHECK_FALSE(r.send());
    CHECK(r.s.connects == 1);
}

TEST_CASE("can_send is false without a transport")
{
    tnfsStreamLink link(nullptr);
    CHECK_FALSE(link.can_send());
}
