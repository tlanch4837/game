#pragma once

#include <windows.h>
#pragma comment(lib, "Synchronization.lib") // WaitOnAddress

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace tbd {

class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

private:
    friend class JobSystem;
    // The decrement is a job's last touch of the counter: the waiter may free it the moment it reads zero. The wake after
    // it only uses the address as a key and never reads the memory, so that stays safe.
    void Done() {
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        WakeByAddressAll(&pending_);
    }
    std::atomic<std::size_t> pending_{0};
};

class JobSystem {
    struct Queue {
        std::mutex mutex;
        std::vector<std::function<void()>> jobs;
    };

public:
    JobSystem() {
        // Two cores stay free (the main thread and Windows), and the workers run below normal priority: a world loading in the
        // background must not make the desktop, the editor's input or the fans pay for it.
        const unsigned int hardware = std::thread::hardware_concurrency();
        const std::size_t count = std::max<std::size_t>(1, hardware > 2 ? hardware - 2 : 1);
        queues_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) queues_.push_back(new Queue());
        workers_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers_.emplace_back([this, i] { WorkerLoop(i); });
            SetThreadPriority((HANDLE)workers_.back().native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
        }
    }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    ~JobSystem() {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        for (Queue* queue : queues_) delete queue;
    }

    void Run(std::function<void()> job, JobCounter* counter = nullptr) {
        if (!job) return;
        if (counter) counter->pending_.fetch_add(1, std::memory_order_relaxed);
        const std::size_t index = nextQueue_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        {
            std::lock_guard<std::mutex> lock(queues_[index]->mutex);
            queues_[index]->jobs.emplace_back([job = std::move(job), counter] {
                try { job(); } catch (...) {
                    if (counter) counter->Done();
                    throw;
                }
                if (counter) counter->Done();
            });
        }
        { std::lock_guard<std::mutex> lock(stateMutex_); } // a worker between its check and its sleep can't miss this
        wake_.notify_one();
    }

    // Never in a timed sleep while a frame's job finishes: Windows rounds even a 1 ms wait up to its 15.6 ms timer tick,
    // which was the engine's frame spikes. Once nothing is left to help with, the waiter blocks on the counter itself:
    // WaitOnAddress wakes the moment a job finishes (Done), with no timer tick and no core spinning meanwhile.
    void Wait(JobCounter& counter) {
        for (;;) {
            std::size_t pending = counter.pending_.load(std::memory_order_acquire);
            if (pending == 0) return;
            if (TryRunOne()) continue;
            WaitOnAddress(&counter.pending_, &pending, sizeof(pending), 20); // the timeout is only a safety net
        }
    }

    // A long job (file import, image decode) for the workers only, first in first out. Wait() never picks one up, so a
    // ParallelFor on the main thread can't get stuck in a five second import; a few workers stay free for the frame's jobs.
    void RunBackground(std::function<void()> job) {
        if (!job) return;
        {
            std::lock_guard<std::mutex> lock(backgroundMutex_);
            background_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

    template <class F>
    void ParallelFor(std::size_t count, std::size_t minBatch, F&& fn) {
        if (count == 0) return;
        minBatch = std::max<std::size_t>(1, minBatch);
        if (count <= minBatch || queues_.size() <= 1) { fn(0, count); return; }
        const std::size_t batches = std::min(queues_.size(), (count + minBatch - 1) / minBatch);
        const std::size_t batchSize = (count + batches - 1) / batches;
        JobCounter counter;
        auto callable = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        for (std::size_t begin = 0; begin < count; begin += batchSize) {
            const std::size_t end = std::min(count, begin + batchSize);
            Run([callable, begin, end] { (*callable)(begin, end); }, &counter);
        }
        Wait(counter);
    }

    std::size_t WorkerCount() const { return workers_.size(); }

private:
    bool TryRunOne() {
        const std::size_t start = nextSteal_.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t offset = 0; offset < queues_.size(); ++offset) {
            Queue& queue = *queues_[(start + offset) % queues_.size()];
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lock(queue.mutex);
                if (queue.jobs.empty()) continue;
                job = std::move(queue.jobs.back());
                queue.jobs.pop_back();
            }
            job();
            return true;
        }
        return false;
    }

    void WorkerLoop(std::size_t) {
        for (;;) {
            if (TryRunOne()) continue;
            if (TryRunBackground()) continue;
            std::unique_lock<std::mutex> lock(stateMutex_);
            // Run and RunBackground notify, and a background slot freeing up does too: the timeout is only a safety net,
            // long enough that idle workers stop waking hundreds of times a second.
            wake_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stopping_ || HasQueuedJobs() || BackgroundRunnable(); });
            if (stopping_ && !HasQueuedJobs() && BackgroundIdle()) return;
        }
    }

    bool BackgroundRunnable() {
        std::lock_guard<std::mutex> lock(backgroundMutex_);
        return !background_.empty() && backgroundRunning_ < BackgroundLimit();
    }

    bool BackgroundIdle() {
        std::lock_guard<std::mutex> lock(backgroundMutex_);
        return background_.empty() && backgroundRunning_ == 0;
    }

    std::size_t BackgroundLimit() const { return workers_.size() > 3 ? workers_.size() - 3 : 1; }

    bool TryRunBackground() {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(backgroundMutex_);
            if (background_.empty() || backgroundRunning_ >= BackgroundLimit()) return false;
            job = std::move(background_.front());
            background_.pop_front();
            ++backgroundRunning_;
        }
        job();
        {
            std::lock_guard<std::mutex> lock(backgroundMutex_);
            --backgroundRunning_;
        }
        { std::lock_guard<std::mutex> lock(stateMutex_); }
        wake_.notify_one(); // a queued background job may have been waiting for this slot
        return true;
    }

    bool HasQueuedJobs() {
        for (Queue* queue : queues_) {
            std::lock_guard<std::mutex> lock(queue->mutex);
            if (!queue->jobs.empty()) return true;
        }
        return false;
    }

    std::vector<Queue*> queues_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> nextQueue_{0};
    std::atomic<std::size_t> nextSteal_{0};
    std::mutex stateMutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::mutex backgroundMutex_;
    std::deque<std::function<void()>> background_;
    std::size_t backgroundRunning_ = 0;
};

} // namespace tbd

