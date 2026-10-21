#include "isolated_gui_platform.h"
#include "ui_audit_recorder.h"
#include <barelief/quick/quick_ui.h>
#include <QApplication>
#include <QAction>
#include <QAbstractButton>
#include <QLabel>
#include <QQuickWidget>
#include <QQuickItem>
#include <QJsonDocument>
#include <QSaveFile>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QDateTime>
#include <QPointer>
#include <QRegularExpression>
#include <cstdio>
#include <stdexcept>

namespace ui_audit {
namespace {
QJsonArray rect(const QRectF& r){return {r.x(),r.y(),r.width(),r.height()};}
QJsonObject properties(QObject* object){
    QJsonObject result;
    // A bounded allow-list avoids unrelated internals and configuration dumps.
    for(const char* key:{"text","title","checked","checkable","currentIndex","count",
                        "value","minimum","maximum","from","to","readOnly","placeholderText",
                        "accessibleName","accessibleDescription","toolTip"}){
        const auto value=object->property(key);
        if(value.isValid())result[key]=QJsonValue::fromVariant(value);
    }
    return result;
}
void settle(){QElapsedTimer clock;clock.start();while(clock.elapsed()<25){QCoreApplication::processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}}
}
Recorder::Recorder(QString directory):directory_(std::move(directory)){}
QJsonObject Recorder::snapshot(QWidget* root) const {
    if(!root)return {{"absent",true}};
    QJsonArray widgets,quick,actions;
    int widgetCount=0,quickCount=0;
    const auto visitQuick=[&](auto&& self,QQuickItem* item,const QString& path)->void{
        if(!item||quickCount>=1200)return;
        ++quickCount;
        QJsonObject row{{"path",path},{"id",item->objectName()},{"class",item->metaObject()->className()},
                        {"visible",item->isVisible()},{"enabled",item->isEnabled()},
                        {"bounds",rect(QRectF(item->mapToScene(QPointF()),QSizeF(item->width(),item->height())))},
                        {"properties",properties(item)}};
        quick.append(row);
        int index=0;for(auto* child:item->childItems())self(self,child,path+"/"+QString::number(index++));
    };
    const auto visit=[&](auto&& self,QWidget* widget,const QString& path)->void{
        if(widgetCount>=1600)return;
        ++widgetCount;
        QJsonObject row{{"path",path},{"id",widget->objectName()},{"class",widget->metaObject()->className()},
                        {"visible",widget->isVisible()},{"enabled",widget->isEnabled()},
                        {"bounds",rect(QRectF(widget->mapTo(root,QPoint()),widget->size()))},
                        {"focusPolicy",int(widget->focusPolicy())},{"hasFocus",widget->hasFocus()},
                        {"accessibleName",widget->accessibleName()},{"tooltip",widget->toolTip()},
                        {"properties",properties(widget)}};
        if(auto* label=qobject_cast<QLabel*>(widget))row["buddy"]=label->buddy()?label->buddy()->objectName():QString{};
        widgets.append(row);
        if(auto* host=qobject_cast<QQuickWidget*>(widget)){
            visitQuick(visitQuick,host->rootObject(),path+"/qml");
        }
        int index=0;for(auto* child:widget->findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly))self(self,child,path+"/"+QString::number(index++));
    };
    visit(visit,root,"root");
    for(auto* action:root->findChildren<QAction*>()){
        QJsonArray shortcuts;for(const auto& shortcut:action->shortcuts())shortcuts.append(shortcut.toString(QKeySequence::PortableText));
        actions.append(QJsonObject{{"id",action->objectName()},{"text",action->text()},
                                  {"enabled",action->isEnabled()},{"visible",action->isVisible()},
                                  {"checkable",action->isCheckable()},{"checked",action->isChecked()},
                                  {"shortcutContext",int(action->shortcutContext())},{"shortcuts",shortcuts}});
    }
    return {{"widgets",widgets},{"quickItems",quick},{"actions",actions},
            {"widgetLimitReached",widgetCount>=1600},{"quickLimitReached",quickCount>=1200},
            {"windowLogicalSize",QJsonArray{root->width(),root->height()}},{"dpr",root->devicePixelRatioF()}};
}
void Recorder::transition(const QString& id,const QString& intent,QWidget* root,
                          const std::function<void()>& action,const std::function<QStringList()>& violations,
                          const QString& dispatch){
    QPointer<QWidget> guarded(root);
    evidence_["runtime"]=QJsonObject{{"platform",QGuiApplication::platformName()},
                                    {"quickBackend",qEnvironmentVariable("QT_QUICK_BACKEND")}};
    QJsonObject entry{{"id",id},{"intent",intent},{"dispatch",dispatch},{"before",snapshot(root)}};
    const bool capture = dispatch.startsWith("qt_input") || id.startsWith("graphs.mode.");
    const auto captureOwned = [&](QWidget* widget,const QString& phase) {
        if (!capture || !widget) return;
        QDir directory(directory_); directory.mkpath("captures");
        QString name=id; name.replace(QRegularExpression("[^a-zA-Z0-9_.-]"),"_");
        const QString relative="captures/"+name+"-"+phase+".png";
        // QWidget rendering of this owned fixture, never a desktop/window-system capture.
        if(widget->grab().save(directory.filePath(relative)))entry[phase+"Image"]=relative;
        else finding(id+".capture","error","harness","Owned widget image could not be saved",relative);
    };
    captureOwned(root,"before");
    QElapsedTimer clock;clock.start();
    try{
        action();settle();
        const auto failures=violations();QJsonArray errors;for(const auto& failure:failures)errors.append(failure);
        entry["violations"]=errors;entry["status"]=failures.isEmpty()?"pass":"fail";
        for(const auto& failure:failures)finding(id,"error","behavior",failure,intent,{{"transition",id}});
    }catch(const std::exception& error){
        entry["status"]="execution_error";entry["exception"]=QString::fromUtf8(error.what());
        finding(id,"error","harness",QString::fromUtf8(error.what()),intent);
    }
    entry["elapsed_ms"]=double(clock.elapsed());entry["after"]=snapshot(guarded.data());
    captureOwned(guarded.data(),"after");
    if(auto* popup=QApplication::activePopupWidget()) {
        entry["popup"]=snapshot(popup);captureOwned(popup,"popup");
    }
    transitions_.append(entry);
}
void Recorder::finding(const QString& id,const QString& severity,const QString& category,
                       const QString& observed,const QString& expected,const QJsonObject& evidence){
    findings_.append(QJsonObject{{"id",id},{"severity",severity},{"category",category},
                                {"observed",observed},{"expected",expected},{"evidence",evidence}});
}
void Recorder::coverage(const QString& area,const QString& status,const QString& reason){
    coverage_.append(QJsonObject{{"area",area},{"status",status},{"reason",reason}});
}
void Recorder::evidence(const QString& key,const QJsonObject& value){evidence_[key]=value;}
int Recorder::executionErrors() const {
    int count=0;for(const auto& item:findings_)count+=item.toObject().value("category")==QStringLiteral("harness");return count;
}
bool Recorder::save(const QString& group) const {
    if(!QDir().mkpath(directory_))return false;
    int failed=0;const int execution=executionErrors();
    for(const auto& value:transitions_){const auto status=value.toObject().value("status").toString();failed+=status=="fail";}
    QJsonObject report{{"schema",1},{"group",group},{"timestampUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"qtVersion",qVersion()},{"requestedPlatform",qEnvironmentVariable("QT_QPA_PLATFORM")},
        {"scope","Owned offscreen UI fixtures and local Qt events; no desktop input, native 3D or aesthetic acceptance"},
        {"transitions",transitions_},{"findings",findings_},{"coverage",coverage_},{"evidence",evidence_},
        {"summary",QJsonObject{{"transitions",transitions_.size()},{"behaviorFailures",failed},{"executionErrors",execution}}}};
    QSaveFile file(QDir(directory_).filePath(group+".json"));
    if(!file.open(QIODevice::WriteOnly))return false;
    const auto bytes=QJsonDocument(report).toJson();return file.write(bytes)==bytes.size()&&file.commit();
}
}
int main(int argc,char** argv){
    if(argc!=3||!test_fixture::isolated_gui_platform()){
        std::fprintf(stderr,"usage: QT_QPA_PLATFORM=offscreen ui_transition_audit artifact-directory workspace|graphs|application\n");return 2;
    }
    const QString directory=QString::fromLocal8Bit(argv[1]),group=QString::fromLocal8Bit(argv[2]);
    ui_audit::Recorder recorder(directory);
    try{
        ui::quick::configure();
        if(group=="application")ui_audit::audit_application(recorder);
        else {
            QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
            QTimer safety;safety.setSingleShot(true);QObject::connect(&safety,&QTimer::timeout,[]{std::_Exit(3);});safety.start(90000);
            if(group=="workspace")ui_audit::audit_workspace(recorder);
            else if(group=="graphs")ui_audit::audit_graphs(recorder);
            else throw std::runtime_error("unknown audit group");
        }
    }catch(const std::exception& error){
        recorder.finding("audit.aborted","error","harness",QString::fromUtf8(error.what()),"Complete requested audit group");
        recorder.coverage(group,"partial","Group aborted; inspect harness finding");recorder.save(group);return 2;
    }
    if(!recorder.save(group))return 2;
    std::printf("UI audit evidence saved: %s (findings are report data, not acceptance)\n",group.toUtf8().constData());return recorder.executionErrors()?2:0;
}
