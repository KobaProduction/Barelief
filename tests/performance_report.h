#pragma once
#include "core/performance.h"
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>
inline bool save_performance_report(const QString &path, const QJsonArray &gui_events = {}) {
    using namespace core::perf;
    enabled=false; // Export outside capture/DSP, after the measured interval.
    for (size_t i=0; i<slow_gui_event_count; ++i) {
        const auto &event=slow_gui_events[i];
        qInfo("SLOW_GUI_EVENT receiver=%s type=%d duration_ms=%.3f", event.receiver.data(),
              event.type, event.duration/1000000.0);
    }
    QJsonArray statistics,trace;
    for(size_t i=0;i<counters.size();++i) {
        const auto &counter=counters[i];const auto count=counter.count.load();if(!count) continue;
        const auto p50=percentile_upper_ns(counter,50)/1000000.0;
        const auto p95=percentile_upper_ns(counter,95)/1000000.0;
        const auto p99=percentile_upper_ns(counter,99)/1000000.0;
        const auto mean=counter.total.load()/double(count)/1000000.0,maximum=counter.maximum.load()/1000000.0;
        statistics.append(QJsonObject{{"stage",names[i]},{"count",double(count)},{"mean_ms",mean},{"p50_upper_ms",p50},{"p95_upper_ms",p95},{"p99_upper_ms",p99},{"max_ms",maximum}});
        qInfo("PROFILE %s count=%llu mean=%.3f ms p50<=%.3f ms p95<=%.3f ms p99<=%.3f ms max=%.3f ms",names[i],static_cast<unsigned long long>(count),mean,p50,p95,p99,maximum);
    }
    const auto count=event_count.load();
    for(size_t i=0;i<std::min<uint64_t>(count,event_capacity);++i) {
        const auto &event=events[i];if(!event.ready.load(std::memory_order_acquire)) continue;
        trace.append(QJsonObject{{"name",names[static_cast<size_t>(event.stage)]},{"cat","pipeline"},{"ph","X"},
            {"pid",1},{"tid",double(event.thread)},{"ts",event.start/1000.0},{"dur",event.duration/1000.0}});
    }
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)) return false;
    const auto data=QJsonDocument(QJsonObject{{"traceEvents",trace},{"statistics",statistics},
        {"gui_event_groups",gui_events},
        {"dropped_events",double(count>event_capacity ? count-event_capacity : 0)}}).toJson();
    return file.write(data)==data.size() && file.commit();
}
