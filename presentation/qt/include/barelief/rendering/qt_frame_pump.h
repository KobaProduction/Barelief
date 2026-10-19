#pragma once
#include <barelief/rendering/frame_schedule.h>
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <utility>

namespace barelief::rendering {
// GUI-thread adapter: one single-shot timer, one outstanding submission, latest
// dirty state. Completion must be delivered to this object's thread by its host.
class QtFramePump final : public QObject {
public:
    using Token=FrameSchedule::Token;
    explicit QtFramePump(std::function<void(Token)> submit):submit_(std::move(submit)) {
        clock_.start();timer_.setSingleShot(true);timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_,&QTimer::timeout,this,[this]{drive();});
    }
    ~QtFramePump() override {timer_.stop();}
    void request(){
        schedule_.request();
        // Setters project axes, ranges and samples in one GUI delivery. Submit
        // after that delivery completes, never a half-updated presentation.
        if(schedule_.runnable()&&!timer_.isActive())timer_.start(0);
    }
    void complete(Token token){if(schedule_.complete(token))drive();}
    void setRate(int fps){timer_.stop();schedule_.setRate(fps);drive();}
    void setVisible(bool visible){
        if(visible_==visible)return;
        visible_=visible;timer_.stop();schedule_.setVisible(visible);drive();
    }
private:
    void drive(){
        if(!schedule_.runnable() || timer_.isActive())return;
        const auto now=FrameSchedule::Time(clock_.nsecsElapsed());
        const auto delay=schedule_.delay(now);
        if(delay){timer_.start(int((delay+999999)/1000000));return;}
        const auto token=schedule_.submit(now);
        if(token)submit_(token);
    }
    bool visible_=true;
    FrameSchedule schedule_;
    QElapsedTimer clock_;
    QTimer timer_;
    std::function<void(Token)> submit_;
};
}
