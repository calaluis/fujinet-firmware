// tnfsCallbackGate (U3): a keep-alive timer callback holds a raw FileSystemTNFS pointer
// and may still be running when the object is destroyed. These tests model that lifecycle
// on PC threads: "timer" threads call run() with a pointer captured at creation, a
// "destructor" stops the timer, closes the gate, unregisters from the WSS queue and only
// then marks the object dead. Objects come from a fixed pool, so addresses are reused the
// way an allocator reuses them, and a callback seeing a dead object is a use-after-free.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "tnfslib_callback_gate.h"
#include "tnfslib_keepalive.h"

using namespace std::chrono_literals;

namespace
{
struct Mount
{
    std::atomic<bool> alive{false};
    std::atomic<int> keep_alives{0};
};

std::atomic<int> dead_access{0}; // work that found its object already destroyed

void keep_alive(void *client)
{
    Mount *m = static_cast<Mount *>(client);
    if (!m->alive)
        dead_access++;
    std::this_thread::yield(); // widen the window a destructor could slip into
    if (!m->alive)
        dead_access++;
    m->keep_alives++;
}

tnfsKeepAliveQueue *&keep_alive_queue_for_test()
{
    static tnfsKeepAliveQueue *queue = nullptr;
    return queue;
}

void mark_due(void *client) { keep_alive_queue_for_test()->mark_due(client); }

std::atomic<bool> in_work{false};
std::atomic<bool> release_work{false};
void blocking_work(void *)
{
    in_work = true;
    while (!release_work)
        std::this_thread::yield();
    in_work = false;
}
} // namespace

TEST_CASE("gate: work runs only for open clients")
{
    tnfsCallbackGate gate;
    Mount a, b;
    a.alive = true;
    CHECK_FALSE(gate.run(&a, keep_alive)); // never opened
    REQUIRE(gate.open(&a));
    CHECK_FALSE(gate.open(&a)); // already open
    CHECK_FALSE(gate.open(nullptr));
    CHECK(gate.run(&a, keep_alive));
    CHECK_FALSE(gate.run(&b, keep_alive));
    CHECK_FALSE(gate.run(&a, nullptr));
    gate.close(&a);
    CHECK_FALSE(gate.is_open(&a));
    CHECK_FALSE(gate.run(&a, keep_alive));
    CHECK(a.keep_alives == 1);
    gate.close(&a); // closing twice, or something never opened, is harmless
    gate.close(&b);
}

TEST_CASE("gate: fixed capacity, slots reused after close")
{
    tnfsCallbackGate gate;
    Mount m[tnfsCallbackGate::CAPACITY + 1];
    for (int i = 0; i < tnfsCallbackGate::CAPACITY; i++)
        REQUIRE(gate.open(&m[i]));
    CHECK_FALSE(gate.open(&m[tnfsCallbackGate::CAPACITY]));
    gate.close(&m[5]);
    CHECK(gate.open(&m[tnfsCallbackGate::CAPACITY]));
    CHECK(gate.is_open(&m[tnfsCallbackGate::CAPACITY]));
}

TEST_CASE("gate: close waits for a callback in progress and lets no new one in")
{
    tnfsCallbackGate gate;
    Mount a;
    REQUIRE(gate.open(&a));
    in_work = false;
    release_work = false;

    std::thread timer([&] { gate.run(&a, blocking_work); });
    while (!in_work)
        std::this_thread::yield();

    std::atomic<bool> closed{false};
    std::thread destructor([&] {
        gate.close(&a);
        closed = true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(closed);                   // still waiting for the running callback
    CHECK_FALSE(gate.run(&a, keep_alive)); // closing: a second firing does not get in
    CHECK(a.keep_alives == 0);

    release_work = true;
    destructor.join();
    timer.join();
    CHECK(closed);
    CHECK_FALSE(gate.is_open(&a));
}

TEST_CASE("gate: a long callback for one mount does not hold up another mount's close")
{
    // The gate lock is not held while work runs (a TCP keep-alive can take seconds).
    tnfsCallbackGate gate;
    Mount a, b;
    REQUIRE(gate.open(&a));
    REQUIRE(gate.open(&b));
    in_work = false;
    release_work = false;
    std::thread timer([&] { gate.run(&a, blocking_work); });
    while (!in_work)
        std::this_thread::yield();

    auto t0 = std::chrono::steady_clock::now();
    gate.close(&b);
    CHECK(std::chrono::steady_clock::now() - t0 < 50ms);
    CHECK(gate.open(&b)); // and b's slot is usable again meanwhile

    release_work = true;
    timer.join();
    gate.close(&a);
    gate.close(&b);
}

TEST_CASE("lifecycle: rapid mount/unmount against firing timers never touches a dead mount")
{
    // Per mount: open the gate, start a timer thread that fires as fast as it can with the
    // pointer it was created with; then "destroy": stop the timer (the thread may still be
    // inside a callback), close the gate, mark the object dead. The timer thread keeps
    // firing a little after the stop, as a late esp_timer callback would.
    tnfsCallbackGate gate;
    static Mount pool[3]; // reused addresses
    dead_access = 0;
    long cycles = 0, callbacks = 0;
    auto end = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < end)
    {
        Mount *m = &pool[cycles % 3];
        m->alive = true;
        m->keep_alives = 0;
        REQUIRE(gate.open(m));

        std::atomic<bool> stopped{false};
        std::thread timer([&gate, m, &stopped] {
            while (!stopped)
                gate.run(m, keep_alive);
            for (int late = 0; late < 3; late++) // callbacks already dispatched
                gate.run(m, keep_alive);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(50 + cycles % 200));

        stopped = true; // esp_timer_stop()/esp_timer_delete(): does not wait
        gate.close(m);  // waits for a callback in progress
        callbacks += m->keep_alives;
        m->alive = false; // the object is gone from here on
        timer.join();
        cycles++;
    }
    MESSAGE("cycles: " << cycles << ", callbacks: " << callbacks);
    CHECK(cycles > 100);
    CHECK(callbacks > 0);
    CHECK(dead_access == 0);
}

TEST_CASE("lifecycle: TCP-style and WSS-style mounts with the main-loop queue, slots reused")
{
    // Several mounts at once. A WSS mount's callback only marks it due; the main loop runs
    // the keep-alive. The destructor order is the one in ~FileSystemTNFS: timer, gate,
    // queue, then the object dies. Neither path may touch a dead mount.
    tnfsCallbackGate gate;
    tnfsKeepAliveQueue queue;
    static Mount pool[4];
    dead_access = 0;
    std::atomic<bool> done{false};
    std::atomic<int> setup_failures{0}; // no REQUIRE in worker threads: it would abort

    std::thread main_loop([&] {
        while (!done)
            queue.service(keep_alive);
    });

    auto mount_cycle = [&](int base) {
        for (int i = 0; i < 400; i++)
        {
            Mount *m = &pool[base + i % 2];
            bool wss = i % 3 != 0;
            m->alive = true;
            if ((wss && !queue.add(m)) || !gate.open(m))
            {
                setup_failures++;
                return;
            }
            std::atomic<bool> stopped{false};
            std::thread timer([&, m, wss] {
                while (!stopped)
                    gate.run(m, wss ? mark_due : keep_alive); // WSS: mark due only
            });
            std::this_thread::yield();
            stopped = true;
            gate.close(m);
            queue.remove(m);
            m->alive = false;
            timer.join();
        }
    };
    keep_alive_queue_for_test() = &queue;
    std::thread a(mount_cycle, 0), b(mount_cycle, 2);
    a.join();
    b.join();
    done = true;
    main_loop.join();
    keep_alive_queue_for_test() = nullptr;

    CHECK(setup_failures == 0);
    CHECK(dead_access == 0);
    CHECK(queue.registered() == 0);
    CHECK(queue.pending() == 0);
}
