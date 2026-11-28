#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace somike::app {

// Runs blocking work (decoding audio, HTTP) on background threads and hands
// the results back to the owner's thread (the UI thread).
//
// A work item returns a completion; completions queue up until the owner
// calls RunCompletions(). The notify callback (from a worker thread) tells the
// owner that one is ready, e.g. by posting a window message.
class TaskRunner {
public:
    using Completion = std::function<void()>;
    using Work = std::function<Completion()>;

    explicit TaskRunner(std::size_t threads);
    // Waits for running work; queued work and pending completions are dropped.
    ~TaskRunner();
    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;

    void SetNotify(std::function<void()> notify);
    void Post(Work work);
    // Owner thread. Returns the number of completions that ran.
    std::size_t RunCompletions();

private:
    void WorkerLoop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Work> queue_;
    std::vector<Completion> done_;
    std::function<void()> notify_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace somike::app
