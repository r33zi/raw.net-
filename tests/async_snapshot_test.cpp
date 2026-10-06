#include "async_snapshot.h"
#include <cassert>
#include <future>
#include <stdexcept>

using namespace std::chrono_literals;

// A bound makes regressions fail rather than leave CI waiting indefinitely.
template<class Predicate>
static void WaitFor(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

int main() {
    AsyncSnapshot<int, int> worker;
    assert(!worker.Latest());
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    int calls = 0;
    assert(worker.Start(7, 1ms, [&](int request) {
        if (++calls == 2) {
            entered.set_value();
            released.wait(); // Simulate a long driver read or scan.
        }
        return request;
    }));
    assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    auto pinned = worker.Latest();
    assert(pinned && *pinned == 7);
    // Both operations must complete while collection remains blocked.
    worker.SetRequest(42);
    for (int i = 0; i < 1000; ++i) assert(*worker.Latest() == 7);
    release.set_value();
    WaitFor([&] { return *worker.Latest() == 42; });
    assert(*pinned == 7); // Old frames stay immutable and alive.
    worker.Stop();
    assert(!worker.Failed());

    // Stop must join work in progress before its owner releases dependencies.
    std::promise<void> collecting, finish;
    auto finished = finish.get_future().share();
    assert(worker.Start(9, 10s, [&](int request) {
        collecting.set_value();
        finished.wait();
        return request;
    }));
    assert(collecting.get_future().wait_for(2s) == std::future_status::ready);
    assert(!worker.Latest()); // Restart discards the previous run's result.
    auto stopped = std::async(std::launch::async, [&] { worker.Stop(); });
    assert(stopped.wait_for(20ms) == std::future_status::timeout);
    finish.set_value();
    assert(stopped.wait_for(2s) == std::future_status::ready);
    stopped.get();

    // Stop also interrupts the timer rather than waiting the whole interval.
    assert(worker.Start(10, 10s, [](int request) { return request; }));
    WaitFor([&] { return bool(worker.Latest()); });
    auto idleStop = std::async(std::launch::async, [&] { worker.Stop(); });
    assert(idleStop.wait_for(2s) == std::future_status::ready);
    idleStop.get();

    // Collector failures leave the UI running and are observable.
    assert(worker.Start(0, 1ms, [](int) -> int { throw std::runtime_error("read failed"); }));
    WaitFor([&] { return worker.Failed(); });
    worker.Stop();
    assert(!worker.Latest());
    assert(worker.Start(11, 1ms, [](int request) { return request; }));
    WaitFor([&] { return bool(worker.Latest()); });
    assert(!worker.Failed() && *worker.Latest() == 11);
    worker.Stop();
}
