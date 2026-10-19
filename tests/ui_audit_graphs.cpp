#include <core_app/plot_composition.h>
#include "ui_audit_recorder.h"
#include <core_app/graph_panel.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_image_view.h>
#include <barelief/numeric_metrics_view.h>
#include <barelief/numeric_pie_view.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/ui_text.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJSValue>
#include <QQuickItem>
#include <QQuickWidget>
#include <QThread>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ui_audit {
namespace {
bool settle(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed()<2000) {
        QCoreApplication::processEvents(); QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return predicate();
}
QList<QQuickItem*> items(QQuickItem* root) {
    QList<QQuickItem*> result;
    if (!root) return result;
    result.append(root);
    for (auto* child:root->childItems()) result.append(items(child));
    return result;
}
QQuickItem* named(QQuickItem* root,const QString& name) {
    for(auto* item:items(root)) if(item->objectName()==name)return item;
    return nullptr;
}
QQuickItem* axis(QQuickItem* root,const QString& lowKey) {
    for(auto* item:items(root)) if(item->property("lowKey").toString()==lowKey)return item;
    return nullptr;
}
QList<QQuickItem*> fields(QQuickItem* root) {
    QList<QQuickItem*> result;
    for(auto* item:items(root))
        if(item->metaObject()->indexOfSignal("editingFinished()")>=0 && item->property("validator").isValid())
            result.append(item);
    return result;
}
QQuickItem* checkBox(QQuickItem* root,const char* label) {
    for(auto* item:items(root))
        if(item->property("text").toString()==ui::text(label) && item->property("checked").isValid()
           && item->metaObject()->indexOfSignal("clicked()")>=0)return item;
    return nullptr;
}
bool click(QQuickItem* item,bool checked) {
    // Deliberately local signal dispatch, not desktop input or native hit testing.
    if(!item || !item->isEnabled() || !item->isVisible())return false;
    return item->setProperty("checked",checked) && QMetaObject::invokeMethod(item,"clicked");
}
bool activate(QQuickItem* item,int index) {
    if(!item || !item->isEnabled() || !item->isVisible())return false;
    // Do not synthesize an activation for an option disabled by the delegate.
    auto enabled=item->property("itemEnabled").value<QJSValue>();
    if(enabled.isCallable() && !enabled.call({QJSValue(index)}).toBool())return false;
    return item->setProperty("currentIndex",index) && QMetaObject::invokeMethod(item,"activated",Q_ARG(int,index));
}
QJsonObject state(GraphPanel& panel,QQuickWidget* controls) {
    QJsonArray controlsState;
    for(auto* item:items(controls?controls->rootObject():nullptr)) {
        if(item->objectName().isEmpty() && !item->property("lowKey").isValid()
           && !item->property("validator").isValid() && !item->property("checked").isValid())continue;
        QJsonObject entry{{"objectName",item->objectName()},{"class",item->metaObject()->className()},
                          {"visible",item->isVisible()},{"enabled",item->isEnabled()},
                          {"width",item->width()},{"height",item->height()}};
        for(const char* property:{"text","lowKey","autoKey","currentIndex","checked","displayText","acceptableInput"}) {
            auto value=item->property(property);
            if(value.isValid())entry[property]=QJsonValue::fromVariant(value);
        }
        controlsState.append(entry);
    }
    return {{"mode",panel.display_mode()},{"preferences",QJsonObject::fromVariantMap(panel.preferences())},
            {"physicalFrequency",panel.physical_frequency()},{"physicalTime",panel.physical_time()},
            {"qmlStatus",controls?int(controls->status()):-1},{"qmlControls",controlsState},
            {"theme",ui::Theme::instance().code()},{"language",ui::Language::instance().code()}};
}
bool prepare(GraphPanel& panel,Recorder& recorder,const QString& id,QQuickWidget*& controls) {
    panel.setAttribute(Qt::WA_ShowWithoutActivating);
    panel.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    panel.resize(1000,700); panel.show();
    controls=panel.findChild<QQuickWidget*>("numericGraphSettings");
    const bool ready=controls && settle([&]{return controls->status()!=QQuickWidget::Loading;})
        && controls->status()==QQuickWidget::Ready && controls->rootObject();
    if(!ready)recorder.finding(id+".qml","error","construction","Graph settings QML did not become ready",
                             "Actual graph sidebar must load before transition audit",state(panel,controls));
    return ready;
}
void record(Recorder& recorder,const QString& id,GraphPanel& panel,QQuickWidget* controls,
            const QString& intent,const std::function<void()>& action,
            const std::function<QStringList()>& violations,const QString& dispatch="public_api") {
    recorder.evidence(id+".before",state(panel,controls));
    recorder.transition(id,intent,&panel,action,violations,dispatch);
    recorder.evidence(id+".after",state(panel,controls));
}
}

void audit_graphs(Recorder& recorder) {
    if(QGuiApplication::platformName()!=QStringLiteral("offscreen")) {
        recorder.coverage("graphs","not_covered","Graph probe requires the isolated offscreen platform.");
        return;
    }
    const QString originalTheme=ui::Theme::instance().code(),originalLanguage=ui::Language::instance().code();
    ui::Language::instance().set("en");
    GraphPanel vector("auditVector",new LineChartView,{{"kind","FFTView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"line","points","bars","pie"},{},true}));
    GraphPanel matrix("auditMatrix",new LineChartView,{{"kind","SpectrogramView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d","surface3d"},{},false}));
    GraphPanel metrics("auditMetrics",new LineChartView,{{"kind","ScalarView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"metrics","signedbar"},{},false}));
    int modesVisited=0;
    for(auto* panel:{&vector,&matrix,&metrics}) {
        QQuickWidget* controls=nullptr;
        if(!prepare(*panel,recorder,panel->objectName(),controls))continue;
        for(const auto& mode:panel->modes()) {
            const QString id="graphs.mode."+mode;
            bool routed=false;
            const auto before=panel->preferences();
            record(recorder,id,*panel,controls,"Select actual QML display mode "+mode,[&]{
                if(panel->modes().size()==1) {routed=panel->display_mode()==mode;return;}
                routed=activate(named(controls->rootObject(),"numericDisplayMode"),panel->modes().indexOf(mode));
            },[&]{
                QStringList failures;
                if(!routed)failures<<"Display selector action was not routed";
                if(!settle([&]{return panel->display_mode()==mode;}))failures<<"Requested mode was not selected";
                auto* selector=named(controls->rootObject(),"numericDisplayMode");
                if(!selector || selector->property("currentIndex").toInt()!=panel->modes().indexOf(mode))failures<<"Display selector disagrees with active mode";
                const bool cartesian=mode!="metrics" && mode!="surface3d" && mode!="pie";
                if(bool(axis(controls->rootObject(),"minimum_x"))!=cartesian)failures<<"Generic axis editors do not match mode";
                for(const auto& key:{"minimum","maximum","minimum_x","maximum_x"})
                    if(panel->preferences().value(key)!=before.value(key))failures<<QString("Mode change altered %1").arg(key);
                if(failures.isEmpty())++modesVisited;
                return failures;
            },panel->modes().size()==1?"public_api":"qt_local_signal");
        }
    }
    // A single scalar mode is inspected, not falsely presented as an enabled mode-selection action.
    recorder.evidence("graphs.mode.metrics.selection",{{"selectorAction","not_applicable_single_mode"}});
    vector.set_display_mode("line");
    auto* controls=vector.findChild<QQuickWidget*>("numericGraphSettings");
    if(controls && controls->rootObject()) {
        for(const QString lowKey:{"minimum_x","minimum"}) {
            const QString autoKey=lowKey=="minimum_x"?"auto_range_x":"auto_range";
            const QString highKey=lowKey=="minimum_x"?"maximum_x":"maximum";
            int toggle=0;
            for(bool automatic:{false,true,false}) {
                bool routed=false;
                record(recorder,"graphs.axes."+autoKey+QString(".toggle%1.").arg(++toggle)+(automatic?"automatic":"manual"),vector,controls,
                       "Toggle axis range and inspect dependent editors",[&]{
                    routed=click(checkBox(axis(controls->rootObject(),lowKey),"Auto"),automatic);
                },[&]{
                    QStringList failures;
                    if(!routed)failures<<"Auto checkbox action was not routed";
                    if(vector.preferences().value(autoKey).toBool()!=automatic)failures<<"Auto preference did not follow checkbox";
                    const auto editors=fields(axis(controls->rootObject(),lowKey));
                    if(editors.size()<2)failures<<"Axis bound editors missing";
                    for(int i=0;i<std::min(2,int(editors.size()));++i)
                        if(editors[i]->isEnabled()==automatic)failures<<"Bound editor enablement disagrees with Auto";
                    if(lowKey=="minimum" && editors.size()>2 && editors[2]->isEnabled()!=automatic)
                        failures<<"Automatic interval editor enablement disagrees with Auto";
                    return failures;
                },"qt_local_signal");
            }
            bool routed=false;
            record(recorder,"graphs.axes."+lowKey+".valid",vector,controls,"Commit valid manual lower bound through QML field",[&]{
                const auto editors=fields(axis(controls->rootObject(),lowKey));
                if(!editors.isEmpty() && editors[0]->isEnabled()) {
                    routed=editors[0]->setProperty("text","-2") && QMetaObject::invokeMethod(editors[0],"editingFinished");
                }
            },[&]{return routed && vector.preferences().value(lowKey).toDouble()==-2 ? QStringList{}:
                    QStringList{"Manual field commit did not route to preference"};},"qt_local_signal");
            const auto valid=vector.preferences();
            bool rejectedCommitRouted=false;
            record(recorder,"graphs.axes."+lowKey+".crossed_ui",vector,controls,
                   "Commit finite numeric minimum above maximum and inspect rollback",[&]{
                const auto editors=fields(axis(controls->rootObject(),lowKey));
                if(!editors.isEmpty() && editors[0]->isEnabled()) {
                    rejectedCommitRouted=editors[0]->setProperty("text",QString::number(valid.value(highKey).toDouble()+1))
                        && QMetaObject::invokeMethod(editors[0],"editingFinished");
                }
            },[&]{
                QStringList failures;
                if(!rejectedCommitRouted)failures<<"Crossed-bound field commit was not routed";
                if(vector.preferences()!=valid)failures<<"Crossed-bound commit changed preferences";
                const auto editors=fields(axis(controls->rootObject(),lowKey));
                if(editors.isEmpty() || editors[0]->property("text").toString()!=QString::number(valid.value(lowKey).toDouble()))
                    failures<<"Rejected field did not return to stored bound";
                return failures;
            },"qt_local_signal");
            if(rejectedCommitRouted && vector.preferences()==valid)
                recorder.finding("graphs.axes."+lowKey+".rejection_feedback","review","usability",
                                 "Crossed numeric limit returns to its stored value after field commit",
                                 "Review whether silent rollback communicates the required minimum < maximum relationship",
                                 state(vector,controls));
            record(recorder,"graphs.axes."+lowKey+".invalid",vector,controls,"Reject reversed, nonfinite and mistyped bounds at presentation boundary",[&]{
                vector.changePreference(lowKey,valid.value(highKey).toDouble()+1);
                vector.changePreference(highKey,std::numeric_limits<double>::infinity());
                vector.changePreference(lowKey,QString("-3"));
            },[&]{return vector.preferences()==valid?QStringList{}:QStringList{"Invalid bound changed persisted preferences"};});
        }
        const auto saved=vector.preferences();
        int writes=0;
        const auto connection=QObject::connect(&vector,&GraphPanel::presentation_changed,&vector,[&]{++writes;});
        // Restore must undo a real divergent state, rather than reapply the same map.
        vector.set_display_mode("bars");
        vector.changePreference("auto_range_x",!saved.value("auto_range_x").toBool());
        vector.changePreference("auto_range",!saved.value("auto_range").toBool());
        vector.changePreference("minimum_x",-5.0);
        vector.changePreference("maximum_x",8.0);
        vector.changePreference("minimum",-6.0);
        vector.changePreference("maximum",9.0);
        QCoreApplication::processEvents();
        const bool divergent=vector.preferences()!=saved && vector.display_mode()!=saved.value("display_mode").toString();
        writes=0;
        record(recorder,"graphs.restore",vector,controls,"Restore manual axes and mode without document writes",[&]{
            vector.restore_presentation(QJsonObject::fromVariantMap(saved));
        },[&]{
            QStringList failures;
            if(!divergent)failures<<"Restore setup did not establish divergent mode/settings";
            if(vector.preferences()!=saved || writes!=0)failures<<"Restore changed saved settings or emitted a persistence write";
            const QString expectedMode=saved.value("display_mode").toString();
            if(vector.display_mode()!=expectedMode)failures<<"Restore did not reinstate display mode";
            auto* selector=named(controls->rootObject(),"numericDisplayMode");
            if(!selector || !settle([&]{return selector->property("currentIndex").toInt()==vector.modes().indexOf(expectedMode);}))
                failures<<"Restored mode not reflected by QML selector";
            for(const QString lowKey:{"minimum_x","minimum"}) {
                const QString autoKey=lowKey=="minimum_x"?"auto_range_x":"auto_range";
                const QString highKey=lowKey=="minimum_x"?"maximum_x":"maximum";
                const bool automatic=saved.value(autoKey).toBool();
                auto* editor=axis(controls->rootObject(),lowKey);
                auto* automaticBox=checkBox(editor,"Auto");
                if(!automaticBox || !settle([&]{return automaticBox->property("checked").toBool()==automatic;}))
                    failures<<autoKey+": restored checkbox disagrees with saved preference";
                const auto bounds=fields(editor);
                if(bounds.size()<2) {failures<<lowKey+": restored bound editors missing";continue;}
                for(int i=0;i<2;++i) {
                    const QString key=i==0?lowKey:highKey;
                    if(!settle([&]{return bounds[i]->property("text").toString()==QString::number(saved.value(key).toDouble());}))
                        failures<<key+": restored field text disagrees with saved preference";
                    if(bounds[i]->isEnabled()==automatic)failures<<key+": restored field enablement disagrees with Auto";
                }
            }
            return failures;
        });
        QObject::disconnect(connection);
        vector.set_display_mode("pie");
        auto* pie=vector.pie_view();
        pie->set_values({1,3});
        record(recorder,"graphs.pie.mode_specific_controls",vector,controls,
               "Pie exposes slice labels and hides unrelated Cartesian bounds",[]{},[&]{
            auto* labels=named(controls->rootObject(),"pieSliceLabels");
            return !axis(controls->rootObject(),"minimum") && !axis(controls->rootObject(),"minimum_x")
                && labels && labels->isVisible()?QStringList{}:QStringList{"Pie-specific settings missing or Cartesian fields exposed"};
        });
    }

    {
        GraphPanel automatic("auditAutomatic",new LineChartView,
                             {{"kind","NumericView"},{"auto_range",false},{"minimum",-.5},{"maximum",.5}},nullptr);
        QQuickWidget* automaticControls=nullptr;
        if(prepare(automatic,recorder,"graphs.auto_display",automaticControls)) {
            std::vector<float> samples(256);
            constexpr double pi=3.14159265358979323846;
            // Eight bounded synthetic tones, delivered only to this chart fixture.
            // No audio device, playback, benchmark, or unbounded history is involved.
            for(size_t i=0;i<samples.size();++i)
                for(int tone=0;tone<8;++tone)
                    samples[i]+=float(2*std::sin(2*pi*(3+tone*4)*double(i)/256.0));
            const auto manual=automatic.preferences();
            bool routed=false;
            auto* chart=automatic.active_chart();
            record(recorder,"graphs.auto_display.manual_vs_effective",automatic,automaticControls,
                   "Enable Auto and compare disabled stored bounds with effective range after eight synthetic tones",[&]{
                routed=click(checkBox(axis(automaticControls->rootObject(),"minimum"),"Auto"),true);
                chart->update(samples);
            },[&]{
                QStringList failures;
                if(!routed || !automatic.preferences().value("auto_range").toBool() || !chart->automatic_y_range())
                    failures<<"Auto checkbox did not route to automatic chart range";
                if(!settle([&]{const auto range=chart->y_range();return range.first<-.5 || range.second>.5;}))
                    failures<<"Effective automatic range did not expand beyond stored manual bounds";
                if(automatic.preferences().value("minimum")!=manual.value("minimum")
                   || automatic.preferences().value("maximum")!=manual.value("maximum"))
                    failures<<"Automatic processing rewrote stored manual bounds";
                const auto editors=fields(axis(automaticControls->rootObject(),"minimum"));
                if(editors.size()<2)failures<<"Auto mode bound fields missing";
                else for(int i=0;i<2;++i) {
                    const QString key=i==0?"minimum":"maximum";
                    if(editors[i]->isEnabled())failures<<key+": manual field remains enabled in Auto";
                    if(editors[i]->property("text").toString()!=QString::number(manual.value(key).toDouble()))
                        failures<<key+": disabled field does not reflect retained manual setting";
                }
                return failures;
            },"qt_local_signal");
            const auto range=chart->y_range();
            const auto editors=fields(axis(automaticControls->rootObject(),"minimum"));
            const auto extrema=std::minmax_element(samples.begin(),samples.end());
            QJsonObject rangeEvidence{{"storedManualMinimum",manual.value("minimum").toDouble()},
                {"storedManualMaximum",manual.value("maximum").toDouble()},
                {"effectiveChartMinimum",range.first},{"effectiveChartMaximum",range.second},
                {"sampleMinimum",double(*extrema.first)},{"sampleMaximum",double(*extrema.second)},
                {"sampleCount",256},{"syntheticToneCount",8},{"automatic",chart->automatic_y_range()},
                {"qml",state(automatic,automaticControls)}};
            if(editors.size()>1) {
                rangeEvidence["disabledMinimumText"]=editors[0]->property("text").toString();
                rangeEvidence["disabledMaximumText"]=editors[1]->property("text").toString();
            }
            recorder.evidence("graphs.auto_display.ranges",rangeEvidence);
            if(routed && chart->automatic_y_range() && editors.size()>1
               && (range.first!=manual.value("minimum").toDouble() || range.second!=manual.value("maximum").toDouble()))
                recorder.finding("graphs.auto_display.clarity","review","usability",
                                 "Auto scales the chart to measured data while disabled Min/Max fields continue displaying the retained manual bounds",
                                 "Review whether the sidebar clearly distinguishes saved manual limits from effective automatic limits; retaining manual settings is expected",
                                 rangeEvidence);
            recorder.coverage("graphs.auto_display","partial","Compared actual public chart y_range with disabled QML limits and retained manual preferences after 256 samples containing eight synthetic tones; clarity remains a UX review, not a processing defect.");
        }
    }
    controls=matrix.findChild<QQuickWidget*>("numericGraphSettings");
    matrix.set_display_mode("image2d");
    if(controls && controls->rootObject()) {
        SpectrogramAxisMetadata metadata;
        metadata.column_coordinates=true;metadata.columns={100,200};metadata.column_title_key="Frequency, Hz (Z)";
        metadata.time_coordinates=true;metadata.ages={1,0};metadata.time_title_key="Time delay, seconds (X)";
        record(recorder,"graphs.axes.coordinates.available",matrix,controls,"Publish physical source coordinate capabilities",[&]{
            matrix.image_view()->set_axis_metadata(metadata);
        },[&]{return matrix.physical_frequency() && matrix.physical_time()?QStringList{}:QStringList{"Physical coordinate capabilities unavailable"};});
        bool routedFrequency=false,routedTime=false;
        record(recorder,"graphs.axes.positions",matrix,controls,"Choose index axes through QML selectors",[&]{
            routedFrequency=activate(named(controls->rootObject(),"frequencyAxisMode"),1);
            routedTime=activate(named(controls->rootObject(),"timeAxisMode"),1);
        },[&]{return routedFrequency && routedTime && matrix.preferences().value("frequency_axis_mode")=="positions"
                && matrix.preferences().value("time_axis_mode")=="positions"?QStringList{}:QStringList{"Index-axis selection did not route"};},"qt_local_signal");
        const auto saved=matrix.preferences();
        record(recorder,"graphs.axes.coordinates.unavailable",matrix,controls,"Remove source coordinates and retain explicit index preferences",[&]{
            matrix.image_view()->set_axis_metadata({});
        },[&]{
            QStringList failures;
            if(matrix.physical_frequency() || matrix.physical_time())failures<<"Missing coordinates still advertised as physical";
            if(matrix.preferences()!=saved)failures<<"Capability loss rewrote explicit preferences";
            for(const QString name:{"frequencyAxisMode","timeAxisMode"}) {
                auto* selector=named(controls->rootObject(),name);
                auto enabled=selector?selector->property("itemEnabled").value<QJSValue>():QJSValue{};
                if(!enabled.isCallable() || enabled.call({QJSValue(0)}).toBool())failures<<name+": unavailable physical option enabled";
            }
            return failures;
        });
        matrix.set_display_mode("surface3d");QCoreApplication::processEvents();
        bool routed=false;
        record(recorder,"graphs.surface.pause",matrix,controls,"Pause surface through embedded QML control",[&]{
            routed=click(checkBox(controls->rootObject(),"Pause"),true);
        },[&]{return routed && matrix.active_spectrogram() && matrix.active_spectrogram()->paused()
                && matrix.preferences().value("renderer").toMap().value("pause").toBool()?QStringList{}:QStringList{"Surface pause control did not route and persist"};},"qt_local_signal");
        record(recorder,"graphs.surface.pause.mode_restore",matrix,controls,"Retain pause across image/surface mode changes and restore",[&]{
            const auto paused=matrix.preferences();matrix.set_display_mode("image2d");matrix.restore_presentation(QJsonObject::fromVariantMap(paused));
        },[&]{return matrix.active_spectrogram() && matrix.active_spectrogram()->paused()?QStringList{}:QStringList{"Pause lost across mode/restore"};});
    }

    auto* values=metrics.metrics_view();
    auto* metricControls=metrics.findChild<QQuickWidget*>("numericGraphSettings");
    if(values && metricControls && metricControls->rootObject()) {
        record(recorder,"graphs.metrics.exact",metrics,metricControls,"Present exact signed/unsigned scalar text without numeric rounding",[&]{
            values->set_inputs({{0,"Literal <b>name</b>"},{1,"Unsigned maximum"}});
            values->set_value(0,"-9007199254740993");values->set_value(1,"18446744073709551615");
        },[&]{
            QStringList failures;
            for(const auto pair:{qMakePair(QString("metricValue_0"),QString("-9007199254740993")),
                                qMakePair(QString("metricValue_1"),QString("18446744073709551615"))})
                if(!settle([&]{auto* item=named(values->rootObject(),pair.first);return item && item->property("text").toString()==pair.second;}))
                    failures<<pair.first+": exact text not displayed";
            return failures;
        });
        recorder.evidence("graphs.metrics.exactValues",{{"signed",values->exact_value(0)},{"unsigned",values->exact_value(1)}});
    }
    for(const QString theme:{"light","dark"})for(const QString language:{"ru","en"}) {
        const auto vectorPrefs=vector.preferences(),matrixPrefs=matrix.preferences(),metricPrefs=metrics.preferences();
        record(recorder,"graphs.appearance."+theme+"."+language,matrix,controls,"Refresh theme/language while retaining graph settings",[&]{
            ui::Theme::instance().set(theme);ui::Language::instance().set(language);QCoreApplication::processEvents();
        },[&]{
            QStringList failures;
            if(vector.preferences()!=vectorPrefs || matrix.preferences()!=matrixPrefs || metrics.preferences()!=metricPrefs)
                failures<<"Appearance refresh changed graph preferences";
            if(controls && controls->rootObject()) {
                auto* selector=named(controls->rootObject(),"numericDisplayMode");
                if(!selector || !settle([&]{return selector->property("displayText").toString()==matrix.modeLabel(matrix.display_mode());}))
                    failures<<"Mode label did not refresh to current language";
            }
            if(values && (values->exact_value(0)!="-9007199254740993" || values->exact_value(1)!="18446744073709551615"))
                failures<<"Appearance refresh altered exact metric text";
            return failures;
        });
        recorder.evidence("graphs.appearance.vector."+theme+"."+language,state(vector,vector.findChild<QQuickWidget*>("numericGraphSettings")));
        recorder.evidence("graphs.appearance.metrics."+theme+"."+language,state(metrics,metricControls));
    }
    recorder.evidence("graphs.scope",{{"modesWithNoTransitionViolations",modesVisited},{"expectedModes",7},
        {"dispatch","QML signal dispatch plus presentation public API; no mouse/keyboard, no native hit test"},
        {"native3dAccepted",false},{"performanceMeasured",false}});
    recorder.coverage("graphs.mode_and_conditional_settings","partial","All seven actual modes; QML signal routing, axis enablement, source capabilities, pause, restore, RU/EN and Light/Dark state. Native input/hit targets and visual acceptance excluded.");
    recorder.coverage("graphs.invalid_input_feedback","partial","Boundary rejection is checked. Native typing, validator intermediate states and usability of rejection feedback need separate acceptance.");
    recorder.coverage("graphs.native_surface_rendering","not_covered","Software/offscreen run establishes QML state only; native GPU rendering and hardware interaction require separate authorized scenario.");
    if(!originalTheme.isEmpty())ui::Theme::instance().set(originalTheme);
    ui::Language::instance().set(originalLanguage);

    // Exercise hit testing, popup lifecycle and actual TextInput validation, not handlers.
    ui::Language::instance().set("en");
    GraphPanel input("auditInput",new LineChartView,{{"kind","FFTView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"line","points","bars","pie"},{},true}));
    QQuickWidget* inputHost=nullptr;
    if(prepare(input,recorder,"graphs.input",inputHost)) {
        auto* root=inputHost->rootObject();
        auto* selector=named(root,"numericDisplayMode");
        const auto press=[&](QQuickItem* target){
            if(!target || !target->isVisible() || !target->isEnabled())throw std::runtime_error("Input target unavailable");
            const auto point=target->mapToScene(QPointF(target->width()/2,target->height()/2)).toPoint();
            if(!inputHost->rect().contains(point))throw std::runtime_error("Input target outside host viewport");
            QTest::mouseClick(inputHost,Qt::LeftButton,Qt::NoModifier,point);QCoreApplication::processEvents();
        };
        const auto popup=[&](){return selector?selector->property("popup").value<QObject*>():nullptr;};
        record(recorder,"graphs.input.mode.popup",input,inputHost,"Click display selector and open its popup",[&]{press(selector);},[&]{
            return popup() && popup()->property("visible").toBool()?QStringList{}:QStringList{"Mouse click did not open mode popup"};
        },"qt_input_mouse");
        const QString oldMode=input.display_mode();
        record(recorder,"graphs.input.mode.escape",input,inputHost,"Escape closes mode popup without choosing another mode",[&]{
            QTest::keyClick(inputHost,Qt::Key_Escape);
        },[&]{return popup() && !popup()->property("visible").toBool() && input.display_mode()==oldMode?QStringList{}:QStringList{"Escape did not cancel popup"};},"qt_input_keyboard");
        const int expectedIndex=(input.modes().indexOf(oldMode)+1)%input.modes().size();bool opened=false;
        record(recorder,"graphs.input.mode.choose",input,inputHost,"Open selector and choose next mode with keyboard",[&]{
            press(selector);opened=popup() && popup()->property("visible").toBool();
            QTest::keyClick(inputHost,Qt::Key_Down);QTest::keyClick(inputHost,Qt::Key_Return);
        },[&]{return opened && input.display_mode()==input.modes()[expectedIndex]
            && selector && selector->property("currentIndex").toInt()==expectedIndex
            && popup() && !popup()->property("visible").toBool()?QStringList{}:QStringList{"Keyboard mode choice did not commit exact next mode and close popup"};},"qt_input_mouse_keyboard");
        input.set_display_mode("line");QCoreApplication::processEvents();
        auto* yAxis=axis(root,"minimum");
        auto* autoBox=checkBox(yAxis,"Auto");
        const bool wasAuto=input.preferences().value("auto_range").toBool();
        record(recorder,"graphs.input.auto.click",input,inputHost,"Click Auto checkbox and update dependent editors",[&]{press(autoBox);},[&]{
            const auto axisFields=fields(yAxis);
            const bool automatic=input.preferences().value("auto_range").toBool();
            return automatic!=wasAuto && axisFields.size()>=2 && axisFields[0]->isEnabled()!=automatic && axisFields[1]->isEnabled()!=automatic?QStringList{}:QStringList{"Auto click or dependent fields did not update"};
        },"qt_input_mouse");
        // Manual setup is explicit; only the transition below claims input coverage.
        input.changePreference("auto_range",false);QCoreApplication::processEvents();
        const auto editors=fields(yAxis);
        if(editors.size()>=2) {
            bool focused=false;
            record(recorder,"graphs.input.bounds.valid_text",input,inputHost,"Type a real valid bound and commit with Tab",[&]{
                press(editors[0]);focused=editors[0]->hasActiveFocus();
                QTest::keyClick(inputHost,Qt::Key_A,Qt::ControlModifier);
                QTest::keyClicks(inputHost,"-3.25");QTest::keyClick(inputHost,Qt::Key_Tab);
            },[&]{return focused && input.preferences().value("minimum").toDouble()==-3.25 && !editors[0]->hasActiveFocus()?QStringList{}:QStringList{"Numeric input did not focus, commit exact bound and move focus"};},"qt_input_mouse_keyboard");
            const auto previous=input.preferences();
            QString intermediate;bool acceptable=true;
            record(recorder,"graphs.input.bounds.invalid_text",input,inputHost,"Type intermediate minus sign and leave editor without changing stored bounds",[&]{
                press(editors[0]);focused=editors[0]->hasActiveFocus();
                QTest::keyClick(inputHost,Qt::Key_A,Qt::ControlModifier);
                QTest::keyClicks(inputHost,"-");intermediate=editors[0]->property("text").toString();
                acceptable=editors[0]->property("acceptableInput").toBool();QTest::keyClick(inputHost,Qt::Key_Tab);
            },[&]{return focused && intermediate=="-" && !acceptable && !editors[0]->hasActiveFocus()
                && input.preferences().value("minimum")==previous.value("minimum") && input.preferences().value("maximum")==previous.value("maximum")
                ?QStringList{}:QStringList{"Intermediate typing/focus was not exercised or stored bounds changed"};},"qt_input_mouse_keyboard");
            recorder.evidence("graphs.input.bounds.invalid_text.result",{{"editorText",editors[0]->property("text").toString()},
                {"duringText",intermediate},{"duringAcceptableInput",acceptable},{"focusMoved",!editors[0]->hasActiveFocus()},
                {"acceptableInput",editors[0]->property("acceptableInput").toBool()},{"storedMinimum",QJsonValue::fromVariant(input.preferences().value("minimum"))}});
        } else recorder.finding("graphs.input.bounds.missing","error","construction","Numeric editors missing","Two actual numeric editors required for typing probe");
    }
    recorder.coverage("graphs.local_pointer_keyboard","partial","Real Qt mouse/key dispatch: mode popup, Escape, keyboard choice, Auto checkbox and invalid typing. Owned widget before/after PNGs; software/offscreen, no native window/GPU acceptance.");
    if(!originalTheme.isEmpty())ui::Theme::instance().set(originalTheme);
    ui::Language::instance().set(originalLanguage);
}
}
