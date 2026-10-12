// tnfsKeepAliveQueue (timer to main-loop hand-off of WSS keep-alives) and tnfs_parse_host
// (host-slot prefixes, including the empty "_wss." case).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "tnfslib_host.h"
#include "tnfslib_keepalive.h"
#include "tnfslibMountInfo.h"

// ---------------------------------------------------------------------------------------
// tnfsKeepAliveQueue

static std::vector<void *> serviced;
static void record(void *client) { serviced.push_back(client); }

TEST_CASE("queue: only due clients are serviced, once per mark")
{
    tnfsKeepAliveQueue q;
    int a, b;
    REQUIRE(q.add(&a));
    REQUIRE(q.add(&b));
    serviced.clear();
    CHECK(q.service(record) == 0);
    q.mark_due(&b);
    q.mark_due(&b); // marking twice before service still means one keep-alive
    CHECK(q.is_due(&b));
    CHECK(q.service(record) == 1);
    CHECK(serviced == std::vector<void *>{&b});
    CHECK_FALSE(q.is_due(&b));
    CHECK(q.service(record) == 0);
}

TEST_CASE("queue: unregistered clients and null are ignored")
{
    tnfsKeepAliveQueue q;
    int a, stranger;
    REQUIRE(q.add(&a));
    q.mark_due(&stranger);
    q.mark_due(nullptr);
    serviced.clear();
    CHECK(q.service(record) == 0);
    CHECK_FALSE(q.add(nullptr));
    CHECK_FALSE(q.add(&a)); // already registered
}

TEST_CASE("queue: fixed capacity, slots are reused after remove")
{
    tnfsKeepAliveQueue q;
    int c[tnfsKeepAliveQueue::CAPACITY + 1];
    for (int i = 0; i < tnfsKeepAliveQueue::CAPACITY; i++)
        REQUIRE(q.add(&c[i]));
    CHECK_FALSE(q.add(&c[tnfsKeepAliveQueue::CAPACITY]));
    q.remove(&c[3]);
    CHECK(q.registered() == tnfsKeepAliveQueue::CAPACITY - 1);
    CHECK(q.add(&c[tnfsKeepAliveQueue::CAPACITY]));
}

TEST_CASE("queue: a removed client is never serviced, even if it was due")
{
    tnfsKeepAliveQueue q;
    int a, b;
    REQUIRE(q.add(&a));
    q.mark_due(&a);
    q.remove(&a);
    serviced.clear();
    CHECK(q.service(record) == 0);
    // A new client in the old slot does not inherit the stale mark.
    REQUIRE(q.add(&b));
    CHECK_FALSE(q.is_due(&b));
    CHECK(q.service(record) == 0);
}

static std::atomic<bool> in_work{false};
static std::atomic<bool> work_may_finish{false};
static std::atomic<int> work_done{0};
static void slow_work(void *)
{
    in_work = true;
    while (!work_may_finish)
        std::this_thread::yield();
    work_done++;
    in_work = false;
}

TEST_CASE("queue: remove waits for a keep-alive in progress (timer shutdown is safe)")
{
    tnfsKeepAliveQueue q;
    int a;
    REQUIRE(q.add(&a));
    q.mark_due(&a);
    in_work = false;
    work_may_finish = false;
    work_done = 0;

    std::thread main_loop([&] { q.service(slow_work); });
    while (!in_work)
        std::this_thread::yield();

    std::atomic<bool> removed{false};
    std::thread destructor([&] {
        q.remove(&a);
        removed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(removed); // blocked while the keep-alive runs
    work_may_finish = true;
    destructor.join();
    main_loop.join();
    CHECK(removed);
    CHECK(work_done == 1);
    CHECK(q.registered() == 0);
}

static std::atomic<int> hits{0};
static void count_hit(void *) { hits++; }

TEST_CASE("queue: marking from a timer thread never blocks and is never lost")
{
    tnfsKeepAliveQueue q;
    int a;
    REQUIRE(q.add(&a));
    hits = 0;
    std::atomic<bool> stop{false};
    std::thread timer([&] {
        for (int i = 0; i < 20000; i++)
            q.mark_due(&a);
        stop = true;
    });
    while (!stop)
        q.service(count_hit);
    timer.join();
    q.service(count_hit);
    CHECK(hits >= 1);
    CHECK_FALSE(q.is_due(&a)); // the last mark was serviced
    CHECK(q.service(count_hit) == 0);
}

// ---------------------------------------------------------------------------------------
// tnfs_parse_host (F6)

TEST_CASE("host: prefixes force a protocol; no prefix leaves it to TNFS")
{
    tnfsHostSpec s = tnfs_parse_host("tnfs.example", TNFS_DEFAULT_PORT);
    CHECK(s.valid);
    CHECK_FALSE(s.forced);
    CHECK(s.protocol == TNFS_PROTOCOL_UNKNOWN);
    CHECK(std::string(s.host) == "tnfs.example");
    CHECK(s.port == TNFS_DEFAULT_PORT);

    s = tnfs_parse_host("_tcp.tnfs.example", TNFS_DEFAULT_PORT);
    CHECK((s.valid && s.forced && s.protocol == TNFS_PROTOCOL_TCP));
    CHECK(std::string(s.host) == "tnfs.example");

    s = tnfs_parse_host("_udp.tnfs.example", 1234);
    CHECK((s.valid && s.forced && s.protocol == TNFS_PROTOCOL_UDP));
    CHECK(s.port == 1234);
}

TEST_CASE("host: _wss. uses port 443 unless another port was given")
{
    tnfsHostSpec s = tnfs_parse_host("_wss.tnfs.example", TNFS_DEFAULT_PORT);
    CHECK((s.valid && s.forced && s.protocol == TNFS_PROTOCOL_WS));
    CHECK(std::string(s.host) == "tnfs.example");
    CHECK(s.port == TNFS_WS_DEFAULT_PORT);
    CHECK(tnfs_parse_host("_wss.tnfs.example", 8443).port == 8443);
}

TEST_CASE("host: an empty name or a prefix with no host is invalid (F6)")
{
    CHECK_FALSE(tnfs_parse_host(nullptr, TNFS_DEFAULT_PORT).valid);
    CHECK_FALSE(tnfs_parse_host("", TNFS_DEFAULT_PORT).valid);
    CHECK_FALSE(tnfs_parse_host("_wss.", TNFS_DEFAULT_PORT).valid);
    CHECK_FALSE(tnfs_parse_host("_tcp.", TNFS_DEFAULT_PORT).valid);
    CHECK_FALSE(tnfs_parse_host("_udp.", TNFS_DEFAULT_PORT).valid);
}

TEST_CASE("host: prefixes are exact and case-sensitive, as before")
{
    tnfsHostSpec s = tnfs_parse_host("_WSS.tnfs.example", TNFS_DEFAULT_PORT);
    CHECK(s.valid);
    CHECK_FALSE(s.forced);
    CHECK(std::string(s.host) == "_WSS.tnfs.example");
    CHECK_FALSE(tnfs_parse_host("_wss", TNFS_DEFAULT_PORT).forced);
}
