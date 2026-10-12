#pragma once
#include <QThreadPool>
#include <QRunnable>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace visualization::rendering {
// Presentation-only pool. Jobs own immutable input; no GUI or GL access here.
inline QThreadPool& preparationPool() {
    static QThreadPool pool;
    static const bool configured=[] { pool.setMaxThreadCount(2);return true; }();
    (void)configured;return pool;
}

// One executing job, one replaceable pending input, one latest completion.
// A slow consumer never creates an unbounded queue or blocks CPU preparation.
template<class Input,class Result> class LatestPreparation final {
    struct State {
        std::mutex mutex;
        std::condition_variable stopped;
        std::optional<Input> pending;
        Result ready,work;
        std::function<void(Result&,const Input&)> prepare;
        std::function<void()> wake;
        bool running=false,closing=false,available=false;
    };
    std::shared_ptr<State> state_=std::make_shared<State>();
    static bool enqueue(const std::shared_ptr<State>& state) noexcept {
        try {
        auto task=std::unique_ptr<QRunnable>(QRunnable::create([state]{
            try {
            std::optional<Input> input;
            {
                std::lock_guard lock(state->mutex);
                if(state->closing||!state->pending) {
                    state->running=false;state->stopped.notify_all();return;
                }
                input=std::move(state->pending);state->pending.reset();
            }
            bool prepared=true;
            try{state->prepare(state->work,*input);}catch(...){prepared=false;}
            std::function<void()> wake;
            {
                std::lock_guard lock(state->mutex);
                if(!state->closing&&prepared) {
                    std::swap(state->work,state->ready);state->available=true;wake=state->wake;
                }
            }
            // A failed notification cannot strand shutdown or the next job.
            try{if(wake)wake();}catch(...){}
            std::lock_guard lock(state->mutex);
            // Requeue between jobs so a continuously changing view cannot
            // monopolize a pool thread ahead of other views.
            if(!state->closing&&state->pending&&enqueue(state))return;
            state->running=false;state->stopped.notify_all();
            }catch(...) {
                // Includes input/result moves and notification allocation,
                // not just user callbacks. Destruction must always unblock.
                std::lock_guard lock(state->mutex);
                state->running=false;state->stopped.notify_all();
            }
        }));
        preparationPool().start(task.get());task.release();return true;
        }catch(...){return false;}
    }
public:
    explicit LatestPreparation(std::function<void(Result&,const Input&)> prepare) {
        state_->prepare=std::move(prepare);
    }
    ~LatestPreparation() {
        std::unique_lock lock(state_->mutex);state_->closing=true;state_->pending.reset();
        state_->stopped.wait(lock,[&]{return !state_->running;});
    }
    LatestPreparation(const LatestPreparation&)=delete;
    LatestPreparation& operator=(const LatestPreparation&)=delete;
    bool submit(Input input,std::function<void()> wake) {
        auto state=state_;
        std::lock_guard lock(state->mutex);
        if(state->closing)return false;
        state->pending=std::move(input);state->wake=std::move(wake);
        if(state->running)return true;
        state->running=true;
        if(!enqueue(state)){state->running=false;state->stopped.notify_all();return false;}
        return true;
    }
    bool take(Result& result) {
        std::lock_guard lock(state_->mutex);
        if(!state_->available)return false;
        std::swap(result,state_->ready);state_->available=false;return true;
    }
};
}
