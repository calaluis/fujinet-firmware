// The real TNFS transaction, mount, stat and keep-alive code (lib/TNFSlib) over a fake
// TNFS server reached through tnfsStreamTransport, on a virtual clock. The harness headers
// in tests/tnfs_harness replace the bus, the clock and the TCP/UDP clients.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "bus.h"
#include "fnSystem.h"
#include "tnfslib.h"
#include "tnfslib_stream.h"

// Harness globals (declared in tests/tnfs_harness).
HarnessSystem fnSystem;
HarnessBus SYSTEM_BUS;
int harness_udp_calls = 0;
int harness_tcp_connects = 0;

using Bytes = std::vector<uint8_t>;

static uint32_t now_ms() { return static_cast<uint32_t>(fnSystem.millis()); }

// ---------------------------------------------------------------------------------------
// A TNFS server behind a WSS bridge, as seen through the stream port.

struct Server
{
    // Behaviour
    bool refuse_connect = false;
    bool mute = false;                        // take requests, never answer
    int drop_on_request = -1;                 // lose the connection when request N arrives
    bool invalid_session_once = false;        // answer the next STAT with INVALID_HANDLE
    bool eagain_without_backoff_once = false; // answer the next request with 5 bytes
    uint32_t reply_delay_ms = 30;

    // State
    bool connected = false;
    int connects = 0, closes = 0, requests = 0, mounts = 0;
    std::vector<uint8_t> commands;
    uint16_t session = 0;
    std::deque<std::pair<uint64_t, Bytes>> replies; // (ready at ms, bytes)
    Bytes ready;

    void answer(const uint8_t *req, int len)
    {
        requests++;
        uint8_t seq = req[2], cmd = req[3];
        commands.push_back(cmd);
        if (requests == drop_on_request)
        {
            connected = false;
            return;
        }
        if (mute)
            return;
        Bytes r = {req[0], req[1], seq, cmd};
        if (eagain_without_backoff_once)
        {
            eagain_without_backoff_once = false;
            r.push_back(TNFS_RESULT_TRY_AGAIN); // OS-error path of TNFSD: no back-off bytes
        }
        else if (cmd == TNFS_CMD_MOUNT)
        {
            mounts++;
            session = static_cast<uint16_t>(0xBE00 + mounts);
            r[0] = session & 0xFF;
            r[1] = session >> 8;
            Bytes ok = {0x00, 0x02, 0x01, 0xE8, 0x03}; // version 1.2, retry 1000 ms
            r.insert(r.end(), ok.begin(), ok.end());
        }
        else if (cmd == TNFS_CMD_STAT && invalid_session_once)
        {
            invalid_session_once = false;
            r.push_back(TNFS_RESULT_INVALID_HANDLE);
        }
        else if (cmd == TNFS_CMD_STAT)
        {
            r.push_back(0x00);
            Bytes st(22, 0);
            st[0] = 0xA4; // mode 0644
            st[1] = 0x81;
            st[6] = 42; // size
            r.insert(r.end(), st.begin(), st.end());
        }
        else
            r.push_back(0x00); // UNMOUNT and other status-only replies
        (void)len;
        replies.push_back({fnSystem.millis() + reply_delay_ms, r});
    }
};

class FakeWss : public tnfsStreamTransport
{
public:
    explicit FakeWss(Server &s) : _s(s) {}
    success_is_true connect(const char *, uint16_t, int timeout_ms) override
    {
        _s.connects++;
        fnSystem.delay(50); // a quick handshake
        if (_s.refuse_connect)
        {
            fnSystem.delay(timeout_ms - 50);
            return success_is_true(false);
        }
        _s.connected = true;
        _s.replies.clear();
        _s.ready.clear();
        return success_is_true(true);
    }
    bool is_connected() const override { return _s.connected; }
    int write(const uint8_t *buf, int len, int) override
    {
        if (!_s.connected)
            return -1;
        _s.answer(buf, len);
        return len;
    }
    int read(uint8_t *buf, int max, int timeout_ms) override
    {
        if (!_s.connected)
            return -1;
        while (!_s.replies.empty() && _s.replies.front().first <= fnSystem.millis())
        {
            Bytes &r = _s.replies.front().second;
            _s.ready.insert(_s.ready.end(), r.begin(), r.end());
            _s.replies.pop_front();
        }
        if (_s.ready.empty())
        {
            fnSystem.delay(timeout_ms); // nothing arrived within the poll
            return 0;
        }
        int n = std::min<int>(max, _s.ready.size());
        std::copy(_s.ready.begin(), _s.ready.begin() + n, buf);
        _s.ready.erase(_s.ready.begin(), _s.ready.begin() + n);
        return n;
    }
    void close() override
    {
        if (_s.connected)
            _s.closes++;
        _s.connected = false;
    }

private:
    Server &_s;
};

struct Mount
{
    Server server;
    tnfsMountInfo info{"tnfs.example", 443};

    Mount()
    {
        harness_udp_calls = harness_tcp_connects = 0;
        info.protocol = TNFS_PROTOCOL_WS;
        info.stream_link.reset(new tnfsStreamLink(
            std::unique_ptr<tnfsStreamTransport>(new FakeWss(server)), now_ms));
    }
    tnfsStreamLink &link() { return *info.stream_link; }
    int stat()
    {
        tnfsStat st;
        return tnfs_stat(&info, &st, "/file");
    }
    void no_fallback() const
    {
        CHECK(harness_udp_calls == 0);
        CHECK(harness_tcp_connects == 0);
    }
};

static uint64_t elapsed_since(uint64_t t0) { return fnSystem.millis() - t0; }

// ---------------------------------------------------------------------------------------

TEST_CASE("mount, stat and unmount over WSS; UNMOUNT releases the connection")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    CHECK(m.info.session == m.server.session);
    CHECK(m.stat() == TNFS_RESULT_SUCCESS);
    CHECK(m.server.connects == 1);
    CHECK(m.link().is_connected());

    REQUIRE(tnfs_umount(&m.info) == TNFS_RESULT_SUCCESS);
    CHECK_FALSE(m.link().is_connected());
    CHECK(m.server.closes == 1);
    m.no_fallback();
}

TEST_CASE("a refused connection fails the mount within the WSS budget, without fallback")
{
    Mount m;
    m.server.refuse_connect = true;
    uint64_t t0 = fnSystem.millis();
    CHECK(tnfs_mount(&m.info) == -1);
    // No attempt starts after 8 s: attempts at 0 s (5 s connect + 1 s delay) and 6 s.
    CHECK(m.server.connects == 2);
    CHECK(elapsed_since(t0) <= 8000 + 5000);
    m.no_fallback();
}

TEST_CASE("a mute server: retries stop at the budget, the connection is kept")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    m.server.mute = true;
    int before = m.server.requests;
    uint64_t t0 = fnSystem.millis();
    CHECK(m.stat() == -1);
    // 2 s wait + 1 s retry delay per attempt: attempts start at 0, 3 and 6 s.
    CHECK(m.server.requests - before == 3);
    CHECK(elapsed_since(t0) >= 8000);
    CHECK(elapsed_since(t0) <= 8000 + 2200);
    CHECK(m.server.connects == 1); // nothing partial was buffered, so no reconnect
    m.no_fallback();
}

TEST_CASE("EAGAIN without its back-off: the partial reply is dropped and the retry realigns")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    m.server.eagain_without_backoff_once = true;
    CHECK(m.stat() == TNFS_RESULT_SUCCESS);
    CHECK(m.server.connects == 2); // expire() closed the misaligned stream
    CHECK(m.link().buffered() == 0);
    m.no_fallback();
}

TEST_CASE("a connection lost while waiting ends the wait at once and the retry reconnects")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    m.server.drop_on_request = m.server.requests + 1;
    uint64_t t0 = fnSystem.millis();
    CHECK(m.stat() == TNFS_RESULT_SUCCESS);
    CHECK(m.server.connects == 2);
    // The first attempt did not sit out the 2 s reply wait: only the 1 s retry delay.
    CHECK(elapsed_since(t0) < 1500);
    m.no_fallback();
}

TEST_CASE("an expired session is recovered over WSS and the command retried")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    uint16_t first = m.info.session;
    m.server.invalid_session_once = true;
    CHECK(m.stat() == TNFS_RESULT_SUCCESS);
    CHECK(m.server.mounts == 2);
    CHECK(m.info.session != first);
    CHECK(m.server.connects == 1);
    m.no_fallback();
}

// ---------------------------------------------------------------------------------------
// Keep-alive (F1): one bounded attempt, never a reconnect, never waits for another task.

TEST_CASE("keep-alive over a live connection: one STAT, settings restored")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    int before = m.server.requests;
    uint64_t t0 = fnSystem.millis();
    CHECK(tnfs_ws_keepalive(&m.info) == TNFS_RESULT_SUCCESS);
    CHECK(m.server.requests - before == 1);
    CHECK(m.server.commands.back() == TNFS_CMD_STAT);
    CHECK(elapsed_since(t0) < 200);
    CHECK(m.info.max_retries == TNFS_RETRIES);
    CHECK(m.info.timeout_ms == TNFS_TIMEOUT);
}

TEST_CASE("keep-alive against a stalled server: a single short attempt, no retry delay")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    m.server.mute = true;
    int before = m.server.requests;
    uint64_t t0 = fnSystem.millis();
    CHECK(tnfs_ws_keepalive(&m.info) == -1);
    CHECK(m.server.requests - before == 1);
    CHECK(elapsed_since(t0) <= 1600); // 1.5 s reply wait, nothing more
    CHECK(m.info.max_retries == TNFS_RETRIES);
    CHECK(m.info.timeout_ms == TNFS_TIMEOUT);
    CHECK(m.info.min_retry_ms == 1000); // the server's value, restored
    CHECK(m.link().can_send());         // connecting allowed again for normal operations
}

TEST_CASE("keep-alive with the connection down: no reconnect, no request, no time")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    m.server.connected = false; // e.g. Wi-Fi lost or the bridge idled out
    int before = m.server.requests;
    uint64_t t0 = fnSystem.millis();
    CHECK(tnfs_ws_keepalive(&m.info) == -1);
    CHECK(m.server.connects == 1);
    CHECK(m.server.requests == before);
    CHECK(elapsed_since(t0) == 0);
    // The next real operation reconnects.
    CHECK(m.stat() == TNFS_RESULT_SUCCESS);
    CHECK(m.server.connects == 2);
}

TEST_CASE("keep-alive while another task is in a transaction: skipped, never waits")
{
    Mount m;
    REQUIRE(tnfs_mount(&m.info) == TNFS_RESULT_SUCCESS);
    std::atomic<bool> locked{false}, release{false};
    std::thread other([&] {
        std::lock_guard<std::recursive_mutex> lock(m.info.transaction_mutex);
        locked = true;
        while (!release)
            std::this_thread::yield();
    });
    while (!locked)
        std::this_thread::yield();
    int before = m.server.requests;
    CHECK(tnfs_ws_keepalive(&m.info) == -1);
    CHECK(m.server.requests == before);
    release = true;
    other.join();
}

TEST_CASE("keep-alive on a TCP mount is refused (TCP keeps its own timer keep-alive)")
{
    tnfsMountInfo info("tnfs.example", 16384);
    info.protocol = TNFS_PROTOCOL_TCP;
    harness_tcp_connects = 0;
    CHECK(tnfs_ws_keepalive(&info) == -1);
    CHECK(harness_tcp_connects == 0);
}

// ---------------------------------------------------------------------------------------
// TCP and UDP keep their behaviour: no WSS budget, no WSS early exits.

TEST_CASE("a TCP mount still makes all of its retries")
{
    tnfsMountInfo info("tnfs.example", 16384);
    info.protocol = TNFS_PROTOCOL_TCP;
    harness_udp_calls = harness_tcp_connects = 0;
    uint64_t t0 = fnSystem.millis();
    CHECK(tnfs_mount(&info) == -1);
    CHECK(harness_tcp_connects == TNFS_RETRIES);
    CHECK(harness_udp_calls == 0);
    CHECK(elapsed_since(t0) >= uint64_t(TNFS_RETRIES) * TNFS_RETRY_DELAY);
}

TEST_CASE("an unprefixed mount still falls back from TCP to UDP")
{
    tnfsMountInfo info("tnfs.example", 16384);
    harness_udp_calls = harness_tcp_connects = 0;
    CHECK(tnfs_mount(&info) == -1);
    CHECK(harness_tcp_connects == 1);
    CHECK(info.protocol == TNFS_PROTOCOL_UDP);
    CHECK(harness_udp_calls > 0);
}
