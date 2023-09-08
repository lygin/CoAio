#include "coro.h"

thread_local Corot::Sche *_sche = nullptr;
void Corot::Sche::start()
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(63, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    _sche = this;
    while (stop_.load() == false)
    {
        polling();
        dispatch();
        if (unlikely(ready_list_.empty())) {
            _mm_pause();
            continue;
        }
        current_ = ready_list_.front();
        ready_list_.pop_front();
        // start to execute coroutine
        current_->resume();
        switch (current_->state_)
        {
        case State::IDLE:
            idle_list_.push_back(current_);
            break;
        case State::Ready:
            ready_list_.push_back(current_);
            break;
        case State::Wait:
            break;
        }
        current_ = nullptr;
    }
}

Corot::Coro *current()
{
    if (likely(_sche != nullptr))
        return _sche->current_;
    return nullptr;
}

void co_yield ()
{
    if (likely(_sche != nullptr))
    {
        current()->yield();
    }
}

void co_wait()
{
    if (likely(_sche != nullptr))
        current()->wait();
}
