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
    static void stop_running(const std::shared_ptr<State>& state) {
        state->running=false;state->stopped.notify_all();
    }
    static std::optional<Input> take_pending(const std::shared_ptr<State>& state) {
        std::lock_guard lock(state->mutex);
        if(state->closing || !state->pending){stop_running(state);return {};}
        auto input=std::move(state->pending);state->pending.reset();return input;
    }
    static bool prepare_work(const std::shared_ptr<State>& state,const Input& input) noexcept {
        try{state->prepare(state->work,input);return true;}catch(...){return false;}
    }
    static std::function<void()> publish_ready(const std::shared_ptr<State>& state,bool prepared) {
        std::lock_guard lock(state->mutex);
        if(state->closing || !prepared)return {};
        std::swap(state->work,state->ready);state->available=true;return state->wake;
    }
    static void finish_job(const std::shared_ptr<State>& state) {
        std::lock_guard lock(state->mutex);
        // Requeue between jobs so another view can use the presentation pool.
        if(!state->closing && state->pending && enqueue(state))return;
        stop_running(state);
    }
    static void execute_job(const std::shared_ptr<State>& state) {
        try {
            const auto input=take_pending(state);
            if(!input)return;
            const bool prepared=prepare_work(state,*input);
            auto wake=publish_ready(state,prepared);
            // A failed notification cannot strand shutdown or the next job.
            try{if(wake)wake();}catch(...){}
            finish_job(state);
        }catch(...) {
            // Input/result moves and notification allocation must also unblock destruction.
            std::lock_guard lock(state->mutex);stop_running(state);
        }
    }
    static bool enqueue(const std::shared_ptr<State>& state) noexcept {
        try {
            auto task=std::unique_ptr<QRunnable>(QRunnable::create([state]{execute_job(state);}));
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
