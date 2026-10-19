#ifndef BARELIEF_ABS_TASK_H
#define BARELIEF_ABS_TASK_H
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>
class ITask {
public:
    virtual ~ITask() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};
class AbstractTask : public ITask {
    std::thread _thread;
    std::mutex _lifecycle_mutex;
    std::function<void(bool, uint64_t, const std::string &)> _lifecycle_handler;
    void _notify(bool ready, uint64_t result, const std::string &error);
    void _run_task();
    std::mutex _sleep_mutex;
    std::condition_variable _wake;
protected:
    std::atomic<bool> _running{false};
    virtual uint64_t _before_run_task();
    virtual uint64_t _task() = 0;
    virtual void _after_close_task() {};
    void _sleep(float ms);
public:
    ~AbstractTask() override;
    void stop() override;
    void start() override;
    void set_lifecycle_handler(std::function<void(bool, uint64_t, const std::string &)> handler);
    bool is_running() const { return _running.load(); }
};
#endif
