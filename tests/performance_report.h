#pragma once
#include "core/performance.h"
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>
inline bool save_performance_report(const QString &path) {
    using namespace core::perf;
    enabled=false; // Export outside capture/DSP, after the measured interval.
    QJsonArray statistics,trace;
    for(size_t i=0;i<counters.size();++i) {
        const auto &counter=counters[i];const auto count=counter.count.load();if(!count) continue;
        uint64_t accumulated=0;double p95=0;
        for(size_t bin=0;bin<histogram_bins;++bin) {
            accumulated+=counter.histogram[bin].load();
            if(accumulated>=((count*95+99)/100)) { p95=(bin+1)*bin_ns/1000000.0;break; }
        }
        const auto mean=counter.total.load()/double(count)/1000000.0,maximum=counter.maximum.load()/1000000.0;
        statistics.append(QJsonObject{{"stage",names[i]},{"count",double(count)},{"mean_ms",mean},{"p95_upper_ms",p95},{"max_ms",maximum}});
        qInfo("PROFILE %s count=%llu mean=%.3f ms p95<=%.3f ms max=%.3f ms",names[i],static_cast<unsigned long long>(count),mean,p95,maximum);
    }
    const auto count=event_count.load();
    for(size_t i=0;i<std::min<uint64_t>(count,event_capacity);++i) {
        const auto &event=events[i];if(!event.ready.load(std::memory_order_acquire)) continue;
        trace.append(QJsonObject{{"name",names[static_cast<size_t>(event.stage)]},{"cat","pipeline"},{"ph","X"},
            {"pid",1},{"tid",double(event.thread)},{"ts",event.start/1000.0},{"dur",event.duration/1000.0}});
    }
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)) return false;
    const auto data=QJsonDocument(QJsonObject{{"traceEvents",trace},{"statistics",statistics},
        {"dropped_events",double(count>event_capacity ? count-event_capacity : 0)}}).toJson();
    return file.write(data)==data.size() && file.commit();
}
