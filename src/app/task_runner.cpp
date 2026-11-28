#include "task_runner.h"

#include <windows.h>

#include "somike/common/log.h"
#include "somike/common/strings.h"
#include "somike/core/com_init.h"

namespace somike::app {

TaskRunner::TaskRunner(std::size_t threads) {
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back(&TaskRunner::WorkerLoop, this);
    }
}

TaskRunner::~TaskRunner() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
}

void TaskRunner::SetNotify(std::function<void()> notify) {
    std::scoped_lock lock(mutex_);
    notify_ = std::move(notify);
}

void TaskRunner::Post(Work work) {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            return;
        }
        queue_.push_back(std::move(work));
    }
    wake_.notify_one();
}

std::size_t TaskRunner::RunCompletions() {
    std::vector<Completion> ready;
    {
        std::scoped_lock lock(mutex_);
        ready.swap(done_);
    }
    for (Completion& completion : ready) {
        if (completion) {
            completion();
        }
    }
    return ready.size();
}

void TaskRunner::WorkerLoop() {
    // Media Foundation and the audio APIs want COM on the worker.
    const core::ComInit com(COINIT_MULTITHREADED);
    for (;;) {
        Work work;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }

        Completion completion;
        try {
            completion = work();
        } catch (const std::exception& e) {
            log::Error(L"Background task failed: {}", text::FromUtf8(e.what()));
        } catch (...) {
            log::Error(L"Background task failed");
        }

        std::function<void()> notify;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) {
                return;
            }
            if (completion) {
                done_.push_back(std::move(completion));
                notify = notify_;
            }
        }
        if (notify) {
            notify();
        }
    }
}

}  // namespace somike::app
