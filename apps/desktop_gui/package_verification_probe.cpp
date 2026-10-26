#include "package_verification_probe.h"
#include <core_app/core_application.h>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <utility>

namespace {
bool available(const pipeline::DeliveredNumericFrame& frame) {
    return frame.status==signal_contracts::Status::ok && frame.frame && frame.frame.view().stamp.available;
}
class PackageVerificationProbe final:public QObject {
    CoreApplication& app_;
    QString path_;
    QElapsedTimer elapsed_;
    QTimer timer_;
    quint64 frames_=0,rows_=0,snapshots_=0;

    bool received() const {return frames_+rows_+snapshots_>0;}
    QJsonObject report() const {
        return {{"schema",1},{"kind","packaged_gui_delivery"},{"sample_started",elapsed_.isValid()},
            {"numeric_frames_delivered",double(frames_)},{"history_rows_delivered",double(rows_)},
            {"history_snapshots_delivered",double(snapshots_)},{"elapsed_ms",double(elapsed_.elapsed())},
            {"apply_status",app_.presenter().apply_status()}};
    }
    bool save() const {
        QSaveFile file(path_);
        if(!file.open(QIODevice::WriteOnly))return false;
        const auto bytes=QJsonDocument(report()).toJson();
        return file.write(bytes)==bytes.size() && file.commit();
    }
    void tick() {
        if(elapsed_.elapsed()<1000 || (!received() && elapsed_.elapsed()<10000))return;
        timer_.stop();
        const bool saved=save();
        if(!saved)qCritical("Could not write packaged GUI delivery report");
        app_.exit(saved&&received()?0:2);
    }
    void bind_delivery() {
        auto& presenter=app_.presenter();
        connect(&presenter,&GuiPresenter::numeric_frame_ready,this,[this](const auto& frame){frames_+=available(frame);});
        connect(&presenter,&GuiPresenter::numeric_history_ready,this,[this](const auto& row){
            rows_+=row.event==pipeline::NumericHistoryEvent::Row && available(row.compact);
        });
        connect(&presenter,&GuiPresenter::numeric_history_snapshot_ready,this,[this](const auto& frame,quint64,quint64 filled){
            snapshots_+=filled>0 && available(frame);
        });
    }
public:
    PackageVerificationProbe(CoreApplication& app,QString path):QObject(&app),app_(app),path_(std::move(path)) {
        bind_delivery();
        connect(&timer_,&QTimer::timeout,this,[this]{tick();});
        elapsed_.start();timer_.start(100);
    }
};
}

void install_package_verification_probe(CoreApplication& app) {
    const auto path=qEnvironmentVariable("BARELIEF_PACKAGE_PROBE_FILE");
    if(!path.isEmpty())new PackageVerificationProbe(app,path);
}
