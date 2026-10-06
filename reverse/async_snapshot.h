#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// One producer owns collection state. The UI only reads immutable, completed
// snapshots; neither publication nor request updates hold a lock during work.
// Start/Stop/SetRequest belong to the UI thread. Stop joins in-flight work.
template<class Request, class Snapshot>
class AsyncSnapshot {
public:
    ~AsyncSnapshot() { Stop(); }

    template<class Collect>
    bool Start(Request initial, std::chrono::milliseconds interval, Collect collect) {
        Stop();
        request_ = std::move(initial);
        stopping_ = false;
        failed_ = false;
        std::atomic_store(&latest_, std::shared_ptr<const Snapshot>{});
        try {
            thread_ = std::thread([this, interval, collect = std::move(collect)] {
                try {
                    std::unique_lock<std::mutex> lock(mutex_);
                    while (!stopping_) {
                        const auto deadline = std::chrono::steady_clock::now() + interval;
                        const Request request = request_;
                        lock.unlock();
                        auto next = std::make_shared<const Snapshot>(collect(request));
                        std::atomic_store(&latest_, std::move(next));
                        lock.lock();
                        wake_.wait_until(lock, deadline, [this] { return stopping_; });
                    }
                } catch (...) {
                    failed_ = true;
                }
            });
        } catch (...) {
            failed_ = true;
            return false;
        }
        return true;
    }

    void SetRequest(const Request& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = request;
    }

    std::shared_ptr<const Snapshot> Latest() const {
        return std::atomic_load(&latest_);
    }

    bool Failed() const { return failed_.load(); }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    Request request_{};
    std::shared_ptr<const Snapshot> latest_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stopping_ = false;
    std::atomic<bool> failed_{false};
};
