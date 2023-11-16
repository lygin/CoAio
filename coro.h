#pragma once
#include <atomic>
#include <functional>
#include <list>
#include <cstddef>
#include <cstdlib>
#include <vector>
#include <memory>
#include <emmintrin.h>
#include <thread>
#include <cstdio>
#include <sched.h>
#include <libaio.h>
#include "waitgroup.h"

#include "concurrentqueue.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define coro_run(_sche, _func) _sche->addTask(_func)

enum AioOP
{
    READ,
    WRITE
};
using AioCbFunc = std::function<void(void*)>;
struct AioTask
{
    AioOP op;
    int fd;
    void *buf;
    size_t off;
    size_t len;
    AioCbFunc cb_func;
    void *cb_data;
};

typedef void *fctx_t;
struct filectx
{
    fctx_t fctx;
    void *coro;
};
extern "C"
{
    filectx jump_fcontext(fctx_t to_ctx, void *to_coro_ptr);
    // jump_fcontext会走到enter函数，其中from为原来coro上下文
    fctx_t make_fcontext(void *sp, size_t size, void (*enter)(filectx from));
}

void co_wait();
void co_yield ();

namespace Corot
{

    using Task = std::function<void()>;
    using TaskQueue = moodycamel::ConcurrentQueue<Task>;

    enum class State
    {
        IDLE,
        Ready,
        Wait,
    };

    class Sche;
    class Coro
    {
        friend class Sche;
        static constexpr size_t kCoroStackSize = 1 << 20;

     public:
        class StackCtx
        {
        public:
            StackCtx()
            {
                stack_base_ = malloc(kCoroStackSize);
                stack_size_ = kCoroStackSize;
                // 构造协程的栈空间，返回协程的上下文地址（以后只要jump到这个上下文地址，就能切换协程）
                fctx_ = make_fcontext(stack_base_ + stack_size_, stack_size_, &Coro::enter);
            }
            ~StackCtx() { free(stack_base_); }
            void *sp() { return stack_base_ + stack_size_; }
            size_t size() { return stack_size_; }

            fctx_t fctx() { return fctx_; }
            void set_fctx(fctx_t fctx) { fctx_ = fctx; }

        private:
            void *stack_base_;
            size_t stack_size_;
            fctx_t fctx_;
        };

        explicit Coro(Sche *sche) : sche_(sche) {}

        void yield()
        {
            state_ = State::Ready;
            switch_coro((Coro *)sche_);
        }

        void wait()
        {
            state_ = State::Wait;
            switch_coro((Coro *)sche_);
        }

        void resume() { switch_coro(this); }

     private:
        static void enter(filectx from)
        {
            Coro *coro = reinterpret_cast<Coro *>(from.coro);
            Coro *sche = (Coro *)coro->sche_;
            sche->ctx_.set_fctx(from.fctx);
            coro->run();
        }

        void switch_coro(Coro *to)
        {
            // 保存寄存器(rbp,rsp,...)到栈中，将需要恢复的协程to的上下文恢复到寄存器中，开始执行to
            // 返回to协程切换后的上下文状态(to_fctx)
            filectx t = jump_fcontext(to->ctx_.fctx(), to);
            // boost每次切换都会更新context的地址，因此每次切换jump都必须重新更新context
            to->ctx_.set_fctx(t.fctx);
        }

        void run()
        {
            while (true)
            {
                state_ = State::Ready;
                task_();
                state_ = State::IDLE;
                switch_coro((Coro *)sche_);
            }
        }

        StackCtx ctx_;
        Sche *sche_;
        Task task_;
        State state_{State::IDLE};
        std::thread *thr_;
    };
    class Sche : public Coro
    {
        using CoroQueue = moodycamel::ConcurrentQueue<Coro *>;
        static constexpr uint32_t kMaxCoro = 128;
        static constexpr uint32_t kMaxEvents = 128;

     public:
        struct AioCbData
        {
            Coro *coro;
            AioCbFunc cb_func;
            void* cb_data;
        };
        explicit Sche(int coro_num) : Coro(this), coro_num_(coro_num)
        {
            for (int i = 0; i < coro_num; ++i)
            {
                coros_.emplace_back(new Coro(this));
                idle_list_.emplace_back(coros_.back().get());
            }
            int ret = io_setup(kMaxEvents, &aioctx_);
            if (ret < 0)
            {
                assert(0);
            }
            thr_ = new std::thread(&Sche::start, this);
        }
        ~Sche()
        {
            io_destroy(aioctx_);
        }
        void start();
        void exit() { stop_.store(true); }
        void addTask(Task &&task) { task_queue_.enqueue(std::move(task)); }
        void polling()
        {
            struct io_event events[kMaxEvents];
            timespec ts;
            ts.tv_nsec = 0;
            ts.tv_sec = 0;
            int ret = io_getevents(aioctx_, 0, kMaxEvents, events, &ts);
            if (ret < 0)
            {
                assert(0);
            }
            else if (ret == 0)
            {
                return;
            }

            for (int i = 0; i < ret; i++)
            {
                struct iocb *completed_iocb = events[i].obj;
                // Process the completed I/O operation here
                AioCbData *cbdata = (AioCbData *)completed_iocb->data;
                AioCbFunc cb_func = cbdata->cb_func;
                void *cb_data = cbdata->cb_data;
                cb_func(cb_data);
                Coro *coro = cbdata->coro;
                if (coro->state_ == State::Wait) {
                    coro->state_ = State::Ready;
                    ready_list_.emplace_back(coro);
                }
            }
        }

        void submit_aio(AioTask *aiotask)
        {
            addTask(std::bind([&](AioTask *task){
                assert(((size_t)task->buf & 0x1ff) == 0);
                assert((task->off & 0x1ff) == 0);
                assert((task->len & 0x1ff) == 0);
                struct iocb *cb = (struct iocb *)calloc(1, sizeof(struct iocb));
                AioCbData *cb_data = new AioCbData;
                cb_data->coro = current_;
                cb_data->cb_func = task->cb_func;
                cb_data->cb_data = task->cb_data;
                if (task->op == WRITE) {
                    io_prep_pwrite(cb, task->fd, task->buf, task->len, task->off);
                } else {
                    io_prep_pread(cb, task->fd, task->buf, task->len, task->off);
                }
                
                cb->data = cb_data;
                int ret = io_submit(aioctx_, 1, &cb);
                assert(ret == 1);
                //等待io完成
                current_->wait();
                // do some other things until io done
            },aiotask)
            );
        }

        void submit_aio_batch(AioTask **aiotask, uint32_t count)
        {
            addTask(std::bind([&](AioTask **task, uint32_t count){
                struct iocb *cb = (struct iocb *)calloc(count, sizeof(struct iocb));
                struct iocb **cbs = (struct iocb **)calloc(count, sizeof(struct iocb*));

                for(int i=0; i<count; ++i) {
                    if(task[i]->op == READ) {
                        io_prep_pread(&cb[i], task[i]->fd, task[i]->buf, task[i]->len, task[i]->off);
                    } else {
                        io_prep_pwrite(&cb[i], task[i]->fd, task[i]->buf, task[i]->len, task[i]->off);
                    }
                    AioCbData *cb_data = new AioCbData;
                    cb_data->coro = current_;
                    cb_data->cb_func = task[i]->cb_func;
                    cb_data->cb_data = task[i]->cb_data;
                    cb[i].data = cb_data;
                    cbs[i] = &cb[i];
                }
                int ret = 0;
                while(ret < count) {
                    assert(ret >= 0);
                    ret += io_submit(aioctx_, count, cbs);
                }
            },aiotask, count)
            );
        }

        Coro *current_;

    private:
        void dispatch()
        {
            if (unlikely(task_begin_ == task_size_))
            {
                task_size_ = task_queue_.try_dequeue_bulk(task_buf_, kMaxCoro);
                task_begin_ = 0;
            }
            auto iter = idle_list_.begin();
            while (iter != idle_list_.end() && task_begin_ < task_size_)
            {
                auto coro = *iter;
                coro->task_ = std::move(task_buf_[task_begin_++]);
                coro->state_ = State::Ready;
                idle_list_.erase(iter++);
                ready_list_.emplace_back(coro);
            }
        }
        int coro_num_;

        TaskQueue task_queue_;
        Task task_buf_[kMaxCoro];
        uint32_t task_begin_{0};
        uint32_t task_size_{0};
        std::atomic<bool> stop_;
        CoroQueue wait_queue_;

        std::list<Coro *> idle_list_;
        std::list<Coro *> ready_list_;

        std::vector<std::shared_ptr<Coro>> coros_;
        io_context_t aioctx_{nullptr};
    };
};
